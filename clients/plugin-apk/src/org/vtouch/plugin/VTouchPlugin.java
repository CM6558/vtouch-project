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
import java.io.FileOutputStream;
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
    private static final String VTOUCHD_BIN = "/data/local/tmp/vtouchd";
    private static final String MERGE_BIN = "/data/local/tmp/vtouchmerge";
    private static final String WS_BIN = "/data/local/tmp/vtouchws";
    private static final String VTOUCHD_PID = RUNTIME_DIR + "/vtouchd.pid";
    private static final String MERGE_SOCK = RUNTIME_DIR + "/merge.sock";
    private static final String MERGE_PID = RUNTIME_DIR + "/merge.pid";
    private static final String WS_PID = RUNTIME_DIR + "/websocket.pid";

    /** AutoJs6 传入的应用上下文（宿主 AutoJs6 的 Context），供跨包 startService。 */
    private static Context sContext;
    /** 插件自己的 Context（AutoJs6 通过 createPackageContext 创建），用于读取插件 APK 的 assets。 */
    private static Context sPluginContext;

    /** AutoJs6 反射入口：返回插件实例（必须实现 ServiceConnection）。 */
    public static Object loadDefault(Context context, Context selfContext, Object runtime, Object topLevelScope) {
        sContext = context != null ? context : selfContext;
        sPluginContext = selfContext;  // selfContext 是插件包 Context，能读 assets
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

    /** 服务健康检查（只读，不启动）。vtouchd 单进程或旧双进程任一健康即就绪。 */
    public boolean isServiceReady() {
        return serviceReady();
    }

    /** 插件负责服务运行：startForegroundService(VTouchService) 托管后端。
     *  ColorOS 启动管理：重装后应用视为"未打开"，后台拉起服务会被 OplusAppStartupManager 拦截
     *  （静默 prevent，不抛异常）。解决：先拉起一次 MainActivity（透明、即开即关）标记"已打开"，
     *  再重试一次服务启动。失败时返回 false，由 JS 胶水层决定是否回退并提示用户。
     *  优化：首次检查是否已就绪，避免重复启动开销。 */
    public boolean startVTouchService() {
        // 已就绪则快速返回（重复启动保护）
        if (serviceReady()) {
            Log.d(TAG, "startVTouchService: already ready, skip");
            return true;
        }
        Context ctx = sContext;
        if (ctx != null) {
            if (startServiceViaIntent(ctx)) return true;
            if (ensureOpened(ctx)) {
                if (startServiceViaIntent(ctx)) return true;
            }
            Log.e(TAG, "VTouchService 未能启动（可能是权限/启动管理限制）");
        }
        return false;
    }

    /** 进程内直接启动后端（回退方案，由 JS 胶水层显式调用）。 */
    public boolean startBackendDirect() {
        Log.w(TAG, "使用进程内直启后端（VTouchService 不可用）");
        return startBackend();
    }

    /** startForegroundService(VTouchService) 并轮询就绪；就绪返回 true。
     *  优化：减少轮询次数和间隔，Service 启动失败时快速返回（从15次降到5次）。 */
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
        /* 等待 VTouchService 内 startBackend() 把后端拉起（幂等：已就绪则快速返回）。
           优化：缩短超时从1.5秒到0.5秒（5次x100ms），Service被拦截时快速失败。 */
        for (int i = 0; i < 5; i++) {
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

    /** 后端状态自检（替代 verify 脚本）：pid 存活 + 27183 端口可连通；
     *  任一不通则用 root 再复核一次（应用挂载命名空间可能看不见运行时文件）。 */
    public String getBackendStatus() {
        StringBuilder sb = new StringBuilder();
        try {
            int pid = readPid(new File(VTOUCHD_PID));
            boolean pidOk = isAlive(pid);
            boolean portOk = false;
            Socket s = null;
            try {
                s = new Socket();
                s.connect(new InetSocketAddress("127.0.0.1", 27183), 1500);
                portOk = true;
            } catch (Exception ignored) {
            } finally {
                if (s != null) { try { s.close(); } catch (Exception ignored) {} }
            }
            String rootNote = "";
            if (!pidOk || !portOk) {
                /* 应用挂载命名空间可能看不见运行时文件，root 说了算。 */
                String out = execRoot("B=" + RUNTIME_DIR + "; P=$(cat $B/vtouchd.pid 2>/dev/null);"
                        + "kill -0 $P 2>/dev/null && echo R_PID_OK PID=$P; ss -ltn 2>/dev/null | grep -q 27183 && echo R_PORT_OK");
                if (out != null) {
                    if (!pidOk && out.contains("R_PID_OK")) {
                        pidOk = true;
                        try {
                            int ps = out.indexOf("PID=");
                            if (ps >= 0) pid = Integer.parseInt(out.substring(ps + 4).trim().split("[^0-9]")[0]);
                        } catch (Exception ignored) {}
                    }
                    if (!portOk && out.contains("R_PORT_OK")) portOk = true;
                    rootNote = " root:{" + out.trim().replace('\n', ' ') + "}";
                } else {
                    rootNote = " root:{n/a}";
                }
            }
            sb.append("pid=").append(pidOk ? pid + ":alive" : "-:dead").append(" ");
            sb.append("port=27183:").append(portOk ? "open" : "closed").append(rootNote).append(" ");
            sb.append(pidOk && portOk ? "STATUS=READY" : "STATUS=NOT_READY");
        } catch (Exception e) {
            sb.append("STATUS=ERROR ").append(e);
        }
        return sb.toString();
    }

    /* ---- 内部实现 ---- */

    private static boolean serviceReady() {
        if (serviceReadyFast()) return true;
        /* 应用进程可能看不见 /data/local/tmp（挂载命名空间隔离），用 root 再确认一次。 */
        try {
            String out = execRoot("B=" + RUNTIME_DIR + ";"
                    + "([ -f $B/vtouchd.pid ] && kill -0 $(cat $B/vtouchd.pid 2>/dev/null) 2>/dev/null && echo VD_OK);"
                    + "([ -S $B/merge.sock ] && kill -0 $(cat $B/merge.pid 2>/dev/null) 2>/dev/null"
                    + " && kill -0 $(cat $B/websocket.pid 2>/dev/null) 2>/dev/null && echo LEGACY_OK)");
            if (out != null && (out.contains("VD_OK") || out.contains("LEGACY_OK"))) return true;
        } catch (Exception e) {
            Log.e(TAG, "serviceReady root check failed: " + e);
        }
        return false;
    }

    /** 快速 File 检查（可见命名空间下命中则直接返回，不起 su）。 */
    private static boolean serviceReadyFast() {
        try {
            File dp = new File(VTOUCHD_PID);
            if (dp.exists() && isAlive(readPid(dp))) return true;
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

    /** 确保 /data/local/tmp 下二进制已释放（APK assets 自动释放）。
     *  按运行时 ABI 从 assets/native/<abi>/ 选择二进制（arm64-v8a 实机 / x86_64 模拟器）。
     *  普通进程无权直接写 /data/local/tmp -> 先写 app 私有目录, 再 root cp + chmod。
     *  幂等: vtouchd 已存在且非空直接返回（二次脚本零开销）。旧双进程文件按需补齐。 */
    static boolean ensureBinaries() {
        Context ctx = sPluginContext != null ? sPluginContext : sContext;
        if (ctx == null) {
            Log.e(TAG, "ensureBinaries: sContext is null");
            return false;
        }
        try {
            File d = new File(VTOUCHD_BIN);
            // 快速路径：同命名空间可见则直接返回，不起 su。
            if (d.exists() && d.length() > 0) {
                Log.d(TAG, "ensureBinaries: vtouchd already in /data/local/tmp");
                return true;
            }
            // root 侧确认：应用挂载命名空间可能看不见 /data/local/tmp，
            // 已安装则跳过解压（常见情况省一次 asset I/O + 一次 cp）。
            String have = execRoot("[ -s " + VTOUCHD_BIN + " ] && echo HAVE_BIN");
            if (have != null && have.contains("HAVE_BIN")) return true;
            Log.d(TAG, "ensureBinaries: context pkg=" + ctx.getPackageName() + " filesDir=" + ctx.getFilesDir());
            String abi = (Build.SUPPORTED_ABIS != null && Build.SUPPORTED_ABIS.length > 0)
                    ? Build.SUPPORTED_ABIS[0] : "arm64-v8a";
            String assetDir = "native/" + abi + "/";
            Log.d(TAG, "ensureBinaries: ABI=" + abi + " assetDir=" + assetDir);
            // 用宿主 Context 的 filesDir（AutoJs6 有写权限），插件 Context 的 filesDir 可能无权限
            Context hostCtx = sContext != null ? sContext : ctx;
            File filesDir = hostCtx.getFilesDir();
            if (filesDir == null || (!filesDir.exists() && !filesDir.mkdirs())) {
                Log.e(TAG, "ensureBinaries: cannot create filesDir");
                return false;
            }
            File pd = new File(filesDir, "vtouchd");
            Log.d(TAG, "ensureBinaries: extract to " + pd.getAbsolutePath());
            if (!pd.exists() || pd.length() == 0) writeAsset(ctx, assetDir + "vtouchd", pd);
            String out = execRoot("cp -f '" + pd.getAbsolutePath() + "' " + VTOUCHD_BIN
                    + " && chmod 755 " + VTOUCHD_BIN
                    + " && [ -s " + VTOUCHD_BIN + " ] && echo INSTALLED");
            if (out == null || !out.contains("INSTALLED")) {
                Log.e(TAG, "ensureBinaries: root cp failed");
                return false;
            }
            return true;
        } catch (Exception e) {
            Log.e(TAG, "ensureBinaries failed", e);
            return false;
        }
    }

    /** 把 assets 资源写出到文件（普通进程可写目录，如 app 私有目录）。 */
    private static void writeAsset(Context ctx, String asset, File dest) throws IOException {
        try (InputStream is = ctx.getAssets().open(asset);
             FileOutputStream fos = new FileOutputStream(dest)) {
            byte[] buf = new byte[16384];
            int n;
            while ((n = is.read(buf)) > 0) fos.write(buf, 0, n);
        }
        Log.i(TAG, "extracted asset " + asset + " -> " + dest.getAbsolutePath());
    }

    /** 启动后端：vtouchd 单进程（wm size 取真实分辨率，root 拉起）。
     *  su 调用压缩到最少：一次组合探测（就绪+HAVE），一次启动+存活确认。 */
    static boolean startBackend() {
        try {
            if (serviceReadyFast()) return true;
            String probe = execRoot("B=" + RUNTIME_DIR + "; D=" + VTOUCHD_BIN + "; "
                    + "([ -f $B/vtouchd.pid ] && kill -0 $(cat $B/vtouchd.pid 2>/dev/null) 2>/dev/null && echo VD_OK);"
                    + "([ -S $B/merge.sock ] && kill -0 $(cat $B/merge.pid 2>/dev/null) 2>/dev/null"
                    + " && kill -0 $(cat $B/websocket.pid 2>/dev/null) 2>/dev/null && echo LEGACY_OK);"
                    + "[ -s $D ] && echo HAVE_BIN");
            if (probe != null && (probe.contains("VD_OK") || probe.contains("LEGACY_OK"))) return true;
            if (probe == null || !probe.contains("HAVE_BIN")) {
                if (!ensureBinaries()) {
                    Log.e(TAG, "startBackend: binaries missing and auto-extract failed");
                    return false;
                }
            }
            String cmd = "SIZE=$(wm size 2>/dev/null | sed -n 's/.*Physical size: //p' | head -n 1); "
                    + "W=${SIZE%x*}; H=${SIZE#*x}; "
                    + "case \"$W:$H\" in ''|*[!0-9:]*) echo 'no size'; exit 12;; esac; "
                    + "B=" + RUNTIME_DIR + "; D=" + VTOUCHD_BIN + "; "
                    + "mkdir -p $B; killall vtouchd 2>/dev/null; killall vtouchmerge 2>/dev/null; killall vtouchws 2>/dev/null; "
                    + "rm -f $B/vtouchd.pid $B/merge.sock $B/merge.pid $B/websocket.pid; "
                    + "nohup $D -w $W -h $H -p 27183 >$B/vtouchd.log 2>&1 </dev/null & P=$!; echo $P > $B/vtouchd.pid; "
                    /* 同一次 su 内等 0.3s 并确认子进程存活：初始化失败（无设备/uinput/grab）都发生在这之前。
                       端口就绪由 JS 侧 WS 连接确认，不在这里轮询。 */
                    + "sleep 0.3; kill -0 $P 2>/dev/null && echo STARTED";
            String out = execRoot(cmd);
            if (out != null && out.contains("no size")) return false;
            if (out != null && out.contains("STARTED")) return true;
            return serviceReady();
        } catch (Exception e) {
            Log.e(TAG, "startBackend failed", e);
            return false;
        }
    }

    static boolean stopBackend() {
        try {
            execRoot("killall vtouchd 2>/dev/null; killall vtouchmerge 2>/dev/null; killall vtouchws 2>/dev/null; "
                    + "rm -f " + VTOUCHD_PID + " " + MERGE_SOCK + " " + MERGE_PID + " " + WS_PID);
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
