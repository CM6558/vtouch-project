import java.lang.reflect.Field;

/** ProbeMain —— 探针子进程（"UI 侧"）的 Java 壳：只做三件事：load .so → attach 共享区 → 循环 tick。
 *  不碰 Android API（app_process 下用 System.out 就够，输出直接落进父进程的日志）。
 */
public class ProbeMain {
    static native int  nativeAttach(int fd);
    static native long nativeTick();
    static native long nativeCoreCnt();
    static native void nativeStopReq();
    static native void nativeTryRoWrite();

    public static void main(String[] args) {
        int fd = -1;
        boolean tryRo = false;
        try {
            if (args.length > 0) fd = Integer.parseInt(args[0]);
            if (args.length > 1) tryRo = "ro".equals(args[1]);
        } catch (Throwable t) { System.out.println("C: 参数解析失败 " + t); }
        System.out.println("C: start fd=" + fd + " tryRoWrite=" + tryRo);
        try {
            System.load("/data/local/tmp/vtouch-probe/libprobe.so");
        } catch (Throwable t) { System.out.println("C: load so 失败 " + t); return; }
        int rc = nativeAttach(fd);
        System.out.println("C: nativeAttach rc=" + rc);
        if (rc != 0) System.exit(2);
        if (tryRo) {
            System.out.println("C: 走只读映射写一个字节（期望 SIGSEGV）");
            nativeTryRoWrite();
            System.out.println("C: 没有崩 —— 只读保护失效，方案要重新考虑");
        }
        long lastCore = nativeTick();
        int stale = 0;
        for (long k = 0;; k++) {
            try { Thread.sleep(100); } catch (Throwable t) {}
            long core = nativeTick();
            if (core == lastCore) stale++; else { stale = 0; lastCore = core; }
            if (k % 10 == 0)
                System.out.println("C: 每秒自报 core_hb=" + core + " core_cnt=" + nativeCoreCnt()
                                   + " stale100ms=" + stale);
            if (k == 900) { System.out.println("C: 90 秒到，主动写 stop_req 让核心收尾"); nativeStopReq(); }
            if (stale >= 20) { System.out.println("C: core_hb 停滞 2 秒 → 核心已死，自杀退出"); System.exit(0); }
        }
    }
}
