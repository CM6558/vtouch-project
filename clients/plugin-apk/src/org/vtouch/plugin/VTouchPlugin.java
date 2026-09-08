package org.vtouch.plugin;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;

/**
 * VTouch AutoJs6 应用插件注册类。
 *
 * AutoJs6 (6.7.0+) 通过 meta-data "org.autojs.plugin.sdk.registry" 找到本类，
 * 反射调用静态方法 loadDefault(Context, Context, Object, Object) 得到插件实例。
 * 要求 (见 AutoJs6 源码 Plugin.kt):
 *   1. 实例必须是 android.content.ServiceConnection 类型（硬性检查）。
 *   2. 反射调 getVersion()/getAssetsScriptDir() 等方法（鸭子类型即可）。
 *   3. getVersion() < 2 时跳过 Service 绑定；v2 保持 1：纯脚本模式，
 *      插件 Java API（startVTouchService 等）为进程内直接调用，不依赖绑定。
 *
 * v2 架构（插件负责服务运行）：服务生命周期由 VTouchService 组件持有，
 *   本类只做转发 + 健康检查，暴露给 JS 胶水层：
 *     - isServiceReady()    服务是否健康（sock + pid + 进程存活）
 *     - startVTouchService() startService(VTouchService) -> root 启动后端
 *     - stopVTouchService()  startService(STOP) -> 停止并清理
 *     - runCommand(cmd)      经 WebSocket 桥向 vtouchmerge 发命令（触摸注入）
 *   生命周期与脚本绑定（脚本 load 启动、脚本退出停止），由 SDK 胶水层驱动。
 */
public class VTouchPlugin implements ServiceConnection {

    private static final String TAG = "VTouchPlugin";
    private static final String RUNTIME_DIR = "/data/local/tmp/vtouch-runtime";
    private static final String MERGE_BIN = "/data/local/tmp/vtouchmerge";
    private static final String WS_BIN = "/data/local/tmp/vtouchws";
    private static final String MERGE_SOCK = RUNTIME_DIR + "/merge.sock";
    private static final String MERGE_PID = RUNTIME_DIR + "/merge.pid";
    private static final String WS_PID = RUNTIME_DIR + "/websocket.pid";

    /** AutoJs6 传入的应用上下文（createPackageContext 得到的插件包 Context），供跨包 startService。 */
    private static Context sContext;

    /** AutoJs6 反射入口：返回插件实例（必须实现 ServiceConnection）。 */
    public static Object loadDefault(Context context, Context selfContext, Object runtime, Object topLevelScope) {
        sContext = context != null ? context : selfContext;
        return new VTouchPlugin();
    }

    /** 插件 JS 胶水层所在的 assets 目录，其下必须存在 index.js。 */
    public String getAssetsScriptDir() {
        return "vtouch";
    }

    /** v1：跳过 AutoJs6 bindService（Java API 进程内直调，无需绑定）。 */
    public int getVersion() {
        return 1;
    }

    /* ---- ServiceConnection 接口（纯脚本模式下不会被调用，保留类型要求） ---- */

    @Override
    public void onServiceConnected(ComponentName name, IBinder service) {
    }

    @Override
    public void onServiceDisconnected(ComponentName name) {
    }

    /* ---- 供 JS 胶水层经 plugin 对象调用的 Java API ---- */

    /** 服务健康检查（只读，不启动）。 */
    public boolean isServiceReady() {
        return serviceReady();
    }

    /** 插件负责服务运行：startForegroundService(VTouchService) 托管后端，失败回退进程内直启。
     *  ColorOS 启动管理：重装后应用视为"未打开"，后台拉起服务会被 OplusAppStartupManager 拦截
     *  （静默 prevent，不抛异常）。解决：先拉起一次 MainActivity（透明、即开即关）标记"已打开"，
     *  再重试一次服务启动；仍失败才回退。 */
    public boolean startVTouchService() {
        Context ctx = sContext;
        if (ctx != null) {
            if (startServiceViaIntent(ctx)) return true;
            if (ensureOpened(ctx)) {
                if (startServiceViaIntent(ctx)) return true;
            }
            Log.e(TAG, "VTouchService 未能启动，回退进程内直启");
            return startBackend();
        }
        return startBackend();
    }

    /** startForegroundService(VTouchService) 并轮询就绪；就绪返回 true。 */
    private static boolean startServiceViaIntent(Context ctx) {
        try {
            Intent i = new Intent();
            i.setComponent(new ComponentName("org.vtouch.plugin", "org.vtouch.plugin.VTouchService"));
            /* Android 12+/调用方 targetSdk 31+ 时普通 startService 会被判后台拒绝
               （Background start not allowed, startFg=false）。
               用 startForegroundService 走前台服务路径；VTouchService.onCreate
               立即 startForeground 满足 5s 时限。API<26 退回 startService。
               本机仅 android-24 platform，startForegroundService(API26+) 用反射调用。 */
            if (Build.VERSION.SDK_INT >= 26) {
                try {
                    Context.class.getMethod("startForegroundService", Intent.class).invoke(ctx, i);
                } catch (Exception e) {
                    Log.e(TAG, "startForegroundService failed, plain startService: " + e);
                    ctx.startService(i);
                }
            } else {
                ctx.startService(i);
            }
        } catch (Exception e) {
            Log.e(TAG, "startService intent failed: " + e);
            return false;
        }
        /* 等待 VTouchService 内 startBackend() 把后端拉起（幂等：已就绪则快速返回）。 */
        for (int i = 0; i < 15; i++) {
            if (serviceReady()) return true;
            try { Thread.sleep(100); } catch (InterruptedException e) { break; }
        }
        return serviceReady();
    }

    /** 打开一次 MainActivity，让 ColorOS 启动管理放行该包的后台服务拉起。
     *  必须用 root/shell 上下文（受信，ColorOS 记为"用户打开"）；从 AutoJs6 应用
     *  上下文 startActivity 会被记为 ignored（checkBackgroundActivityPermission deny）。 */
    private static boolean ensureOpened(Context ctx) {
        try {
            String out = execRoot("am start -n org.vtouch.plugin/.MainActivity");
            Thread.sleep(600); // 等 Activity 启动/结束，ColorOS 记录"已打开"
            return out != null;
        } catch (Exception e) {
            Log.e(TAG, "ensureOpened failed: " + e);
            return false;
        }
    }

    /** 停止并清理：通知 VTouchService 停止 + 兜底直接 killall。 */
    public boolean stopVTouchService() {
        Context ctx = sContext;
        if (ctx != null) {
            try {
                Intent i = new Intent();
                i.setComponent(new ComponentName("org.vtouch.plugin", "org.vtouch.plugin.VTouchService"));
                i.setAction(VTouchService.ACTION_STOP);
                /* 与启动一致用前台服务路径（普通 startService 在调用方 targetSdk 31+ 会被判后台拒绝，
                   导致 STOP action 无法送达 onStartCommand）。 */
                if (Build.VERSION.SDK_INT >= 26) {
                    try {
                        Context.class.getMethod("startForegroundService", Intent.class).invoke(ctx, i);
                    } catch (Exception e) {
                        Log.e(TAG, "stop startForegroundService failed: " + e);
                        ctx.startService(i);
                    }
                } else {
                    ctx.startService(i);
                }
                ctx.stopService(i);
            } catch (Exception e) {
                Log.e(TAG, "stopService intent failed: " + e);
            }
        }
        return stopBackend();
    }

    /** 经 WebSocket 桥向 vtouchmerge 发送一条命令（如 "down 0 100 200"）。 */
    public String runCommand(String cmd) {
        return sendCommand(cmd);
    }

    /* ---- 内部实现 ---- */

    private static boolean serviceReady() {
        try {
            File sock = new File(MERGE_SOCK);
            File mp = new File(MERGE_PID);
            File wp = new File(WS_PID);
            if (!sock.exists() || !mp.exists() || !wp.exists()) return false;
            return isAlive(readPid(mp)) && isAlive(readPid(wp));
        } catch (Exception e) {
            return false;
        }
    }

    private static int readPid(File f) {
        try (BufferedReader br = new BufferedReader(new FileReader(f))) {
            String s = br.readLine();
            return s == null ? -1 : Integer.parseInt(s.trim());
        } catch (Exception e) {
            return -1;
        }
    }

    private static boolean isAlive(int pid) {
        return pid > 0 && new File("/proc/" + pid).exists();
    }

    /** 启动后端：wm size 取真实分辨率，root 拉起 vtouchmerge/vtouchws。 */
    static boolean startBackend() {
        try {
            if (serviceReady()) return true;
            String cmd = "SIZE=$(wm size 2>/dev/null | sed -n 's/.*Physical size: //p' | head -n 1); "
                    + "W=${SIZE%x*}; H=${SIZE#*x}; "
                    + "case \"$W:$H\" in ''|*[!0-9:]*) echo 'no size'; exit 12;; esac; "
                    + "B=" + RUNTIME_DIR + "; M=" + MERGE_BIN + "; W2=" + WS_BIN + "; "
                    + "mkdir -p $B; killall vtouchmerge 2>/dev/null; killall vtouchws 2>/dev/null; "
                    + "rm -f $B/merge.sock $B/merge.pid $B/websocket.pid; "
                    + "nohup $M -s $B/merge.sock -v 10 -w $W -h $H >/dev/null 2>&1 </dev/null & echo $! > $B/merge.pid; "
                    + "nohup $W2 >/dev/null 2>&1 </dev/null & echo $! > $B/websocket.pid";
            String out = execRoot(cmd);
            if (out != null && out.contains("no size")) return false;
            for (int i = 0; i < 20; i++) {
                if (serviceReady()) return true;
                try { Thread.sleep(100); } catch (InterruptedException e) { break; }
            }
            return serviceReady();
        } catch (Exception e) {
            Log.e(TAG, "startBackend failed", e);
            return false;
        }
    }

    static boolean stopBackend() {
        try {
            execRoot("killall vtouchmerge 2>/dev/null; killall vtouchws 2>/dev/null; "
                    + "rm -f " + MERGE_SOCK + " " + MERGE_PID + " " + WS_PID);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /** 经 WebSocket(127.0.0.1:27183) 发一条命令并读回响应。 */
    private static String sendCommand(String cmd) {
        if (cmd == null || cmd.isEmpty()) return "err empty";
        if (!serviceReady()) return "err service not ready";
        Socket sock = null;
        try {
            sock = new Socket();
            sock.connect(new InetSocketAddress("127.0.0.1", 27183), 1000);
            sock.setSoTimeout(2000);
            OutputStream os = sock.getOutputStream();
            os.write((cmd + "\n").getBytes("UTF-8"));
            os.flush();
            InputStream is = sock.getInputStream();
            byte[] buf = new byte[1024];
            int n = is.read(buf);
            return n > 0 ? new String(buf, 0, n, "UTF-8").trim() : "err no reply";
        } catch (IOException e) {
            return "err " + e.getMessage();
        } finally {
            if (sock != null) {
                try { sock.close(); } catch (IOException ignored) {}
            }
        }
    }

    /** 执行 root shell 命令（su -c），返回 stdout；失败返回 null。
     *  用绝对路径候选：插件 app 进程的 PATH 可能不含 su 所在目录
     *  （现象：Cannot run program "su": error=2, No such file or directory）。 */
    static String execRoot(String cmd) {
        String[] suCandidates = {"/system/bin/su", "/system/xbin/su", "/sbin/su", "su"};
        Process p = null;
        for (String su : suCandidates) {
            try {
                p = Runtime.getRuntime().exec(new String[]{su, "-c", cmd});
                break;
            } catch (Exception e) {
                Log.e(TAG, "execRoot candidate " + su + " failed: " + e);
                p = null;
            }
        }
        if (p == null) {
            Log.e(TAG, "execRoot failed: no su binary in candidates");
            return null;
        }
        try {
            BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = br.readLine()) != null) sb.append(line).append("\n");
            p.waitFor();
            return sb.toString();
        } catch (Exception e) {
            Log.e(TAG, "execRoot failed", e);
            return null;
        } finally {
            if (p != null) {
                try { p.destroy(); } catch (Exception ignored) {}
            }
        }
    }
}
