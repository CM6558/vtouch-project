package org.vtouch.plugin;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
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
 * v2 架构（vtouchmerge 新架构）：本类以 root 拉起/守护
 *   vtouchmerge + vtouchws 两个进程，并暴露给 JS 胶水层：
 *     - isServiceReady()    服务是否健康（sock + pid 检查）
 *     - startVTouchService() root 启动 vtouchmerge/vtouchws
 *     - stopVTouchService()  停止并清理
 *     - runCommand(cmd)      经 WebSocket 桥向 vtouchmerge 发命令（触摸注入）
 *   VTouchService 组件提供相同的托管能力（供显式 startService 场景）。
 */
public class VTouchPlugin implements ServiceConnection {

    private static final String TAG = "VTouchPlugin";
    private static final String RUNTIME_DIR = "/data/local/tmp/vtouch-runtime";
    private static final String MERGE_BIN = "/data/local/tmp/vtouchmerge";
    private static final String WS_BIN = "/data/local/tmp/vtouchws";
    private static final String MERGE_SOCK = RUNTIME_DIR + "/merge.sock";
    private static final String MERGE_PID = RUNTIME_DIR + "/merge.pid";
    private static final String WS_PID = RUNTIME_DIR + "/websocket.pid";

    /** AutoJs6 反射入口：返回插件实例（必须实现 ServiceConnection）。 */
    public static Object loadDefault(Context context, Context selfContext, Object runtime, Object topLevelScope) {
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

    /** root 启动 vtouchmerge + vtouchws（新架构）。 */
    public boolean startVTouchService() {
        return startBackend();
    }

    /** 停止并清理 vtouch 服务。 */
    public boolean stopVTouchService() {
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

    /** 执行 root shell 命令（su -c），返回 stdout；失败返回 null。 */
    static String execRoot(String cmd) {
        Process p = null;
        try {
            p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
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
