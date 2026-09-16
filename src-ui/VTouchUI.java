import android.util.Log;
import android.view.Surface;

/* VTouchUI — app_process 入口（root 直起，免安装）。
 * 只干三件事：load .so → 建一个全屏 composer 图层 → 把 Surface 递给 native。
 * 触摸/渲染/点击全在 native + vtouch 侧，Java 不碰输入。
 * 用法: CLASSPATH=.../classes.dex app_process /system/bin --nice-name=vtouch-ui VTouchUI <w> <h>
 */
public class VTouchUI {
    static final String TAG = "VTouchUI";
    static Class<?> SCC, TXN;

    static native int nativeInit(int w, int h);
    static native void nativeOnSurface(int id, Surface surface);
    static native void nativeOnDisplay(int w, int h, int rot);
    static native void nativeDestroy();
    static native int nativeWantLayerVisible();
    static native int nativeTakeSwapDone();      /* 转屏后"新 surface 首帧已提交"（读到即清零） */

    /* 显示变化：事件驱动（公开 API DisplayManager.registerDisplayListener）。
     * 事件只告诉我们"去查"——实测回调常早于状态更新（getRotation() 仍返回旧值），所以配一个
     * 500ms 观察窗复查到值真变；注册失败自动回落轮询，绝不影响可用性。 */
    static Object sysCtx;
    static volatile long settleUntil = 0;
    static volatile boolean listenerOk = false;
    static volatile Thread mainTh;      /* 主循环线程：事件到达时打断它的 sleep，检测延迟从"最多一个周期"降到 ~0 */

    static void startDisplayListener() {
        Thread th = new Thread(new Runnable() {
            public void run() {
                try {
                    android.os.Looper.prepare();
                    final android.os.Handler h = new android.os.Handler();
                    if (sysCtx == null) { Log.w(TAG, "没有 system Context → 回落 320ms 轮询"); android.os.Looper.loop(); return; }
                    Object dm = sysCtx.getClass().getMethod("getSystemService", Class.class)
                                       .invoke(sysCtx, Class.forName("android.hardware.display.DisplayManager"));
                    Class<?> dl = Class.forName("android.hardware.display.DisplayManager$DisplayListener");
                    Object proxy = java.lang.reflect.Proxy.newProxyInstance(dl.getClassLoader(),
                        new Class<?>[]{dl}, new java.lang.reflect.InvocationHandler() {
                            public Object invoke(Object p, java.lang.reflect.Method m, Object[] a2) {
                                if ("onDisplayChanged".equals(m.getName())) {
                                    settleUntil = System.currentTimeMillis() + 500;
                                    /* 立刻叫醒主循环（否则要等它睡满一个周期才发现，最坏 40ms 里
                                     * 图层还是旧尺寸铺在新屏上 = 会被拉伸）。sleep 被打断抛异常，
                                     * 主循环 catch 掉继续，等于"立刻醒来再看一次"。 */
                                    Thread mt = mainTh;
                                    if (mt != null) mt.interrupt();
                                    Log.i(TAG, "DisplayListener: onDisplayChanged（事件到达，开 500ms 观察窗）");
                                }
                                return null;
                            }
                        });
                    dm.getClass().getMethod("registerDisplayListener", dl, android.os.Handler.class)
                                .invoke(dm, proxy, h);
                    listenerOk = true;
                    Log.i(TAG, "DisplayListener 注册成功（公开 API）：转屏事件驱动，轮询退化为兜底");
                } catch (Throwable t) {
                    Log.w(TAG, "DisplayListener 注册失败 → 回落 320ms 轮询", t);
                }
                android.os.Looper.loop();
            }
        }, "disp-listener");
        th.setDaemon(true);
        th.start();
    }

    /* 真实显示状态：DisplayManagerGlobal（隐藏类，单例；包名是 android.hardware.display，
     * 不是 android.view）→ Display(0) → rotation + real size。
     * app_process 没有 Context，但这个入口是静态的，反射就够；查不到就退回启动参数（竖屏），
     * 绝不因为查不到 display 就起不来。返回 {w, h, rotation}。 */
    static final String[] DMG_NAMES = {
        "android.hardware.display.DisplayManagerGlobal",
        "android.view.DisplayManagerGlobal",
    };
    /* 反射结果缓存：queryDisplay 每 ~300ms 调一次，每次 forName + getDeclaredMethod 太亏；
     * ROM 上隐藏 API 不可用时更糟（每 300ms 一条堆栈把日志冲掉）。探一次、缓存、失败只报一次。 */
    static Object mDmg;
    static java.lang.reflect.Method mGetDisplay, mGetInfo, mGetRotation, mGetRealSize;
    static Class<?> cDisplay;
    static boolean reflectReady, reflectFailed, readWarned;

    static synchronized void initReflect() {
        if (reflectReady || reflectFailed) return;
        for (String cn : DMG_NAMES) {
            try {
                Class<?> dmg = Class.forName(cn);
                Object g = dmg.getMethod("getInstance").invoke(null);
                if (g == null) continue;
                mDmg = g;
                cDisplay = Class.forName("android.view.Display");
                /* 隐藏 API：getMethod 找不到 → 必须 getDeclaredMethod + setAccessible */
                try {
                    java.lang.reflect.Method m = dmg.getDeclaredMethod("getDisplay", int.class);
                    m.setAccessible(true);
                    mGetDisplay = m;
                } catch (Throwable e) { mGetDisplay = null; }        /* 落回 DisplayInfo 路线 */
                try {
                    java.lang.reflect.Method m = dmg.getDeclaredMethod("getDisplayInfo", int.class);
                    m.setAccessible(true);
                    mGetInfo = m;
                } catch (Throwable e) { mGetInfo = null; }
                try { mGetRotation = cDisplay.getMethod("getRotation"); } catch (Throwable e) { mGetRotation = null; }
                try {
                    mGetRealSize = cDisplay.getMethod("getRealSize", android.graphics.Point.class);
                } catch (Throwable e) { mGetRealSize = null; }
                reflectReady = true;
                Log.i(TAG, "display 反射就绪 via " + cn);
                return;
            } catch (Throwable t) { /* 换下一个候选类名 */ }
        }
        reflectFailed = true;
        Log.w(TAG, "display 反射不可用 → 沿用启动参数（不再重试、不再刷屏）");
    }

    /* 读一次显示状态；读不到返回 null（调用方保留上次值） */
    static int[] readDisplay() {
        try {
            if (mDmg != null && mGetDisplay != null && mGetRotation != null && mGetRealSize != null) {
                Object d = mGetDisplay.invoke(mDmg, 0);
                if (d != null) {
                    int rot = (Integer) mGetRotation.invoke(d);
                    android.graphics.Point p = new android.graphics.Point();
                    mGetRealSize.invoke(d, p);
                    if (p.x > 0 && p.y > 0) return new int[]{p.x, p.y, rot};
                }
            }
            if (mDmg != null && mGetInfo != null) {
                Object info = mGetInfo.invoke(mDmg, 0);
                if (info != null) {
                    Class<?> ic = info.getClass();
                    int rot = ic.getField("rotation").getInt(info);
                    int w = ic.getField("logicalWidth").getInt(info);
                    int h = ic.getField("logicalHeight").getInt(info);
                    if (w > 0 && h > 0) return new int[]{w, h, rot};
                }
            }
        } catch (Throwable t) {
            if (!readWarned) { readWarned = true; Log.w(TAG, "display 读取失败（保留上次值，后续不再重复报）", t); }
        }
        return null;
    }

    /* 返回 {w, h, rotation}：查不到就退回传入值（绝不因为查不到 display 就起不来） */
    static int[] queryDisplay(int dw, int dh, int drot) {
        if (!reflectReady && !reflectFailed) initReflect();
        if (reflectReady) {
            int[] r = readDisplay();
            if (r != null) return r;
        }
        return new int[]{dw, dh, drot};
    }

    static Surface newSurface(Object layer) throws Throwable {
        Class<?> surfCls = Class.forName("android.view.Surface");
        java.lang.reflect.Constructor<?> ctor = surfCls.getDeclaredConstructor(SCC);
        ctor.setAccessible(true);
        return (Surface) ctor.newInstance(layer);
    }

    static Object txnNew() throws Throwable {
        return TXN.getDeclaredConstructor().newInstance();
    }
    static void txnCall(Object t, String name, Class<?>[] ps, Object... a) throws Throwable {
        java.lang.reflect.Method m = TXN.getDeclaredMethod(name, ps);
        m.setAccessible(true);
        m.invoke(t, a);
    }
    static Object makeLayer(String name, int w, int h) throws Throwable {
        Class<?> bld = Class.forName("android.view.SurfaceControl$Builder");
        Object b = bld.getDeclaredConstructor().newInstance();
        bld.getMethod("setName", String.class).invoke(b, name);
        bld.getMethod("setBufferSize", int.class, int.class).invoke(b, w, h);
        bld.getMethod("setFormat", int.class).invoke(b, 1); /* TRANSLUCENT */
        Object sc = bld.getMethod("build").invoke(b);
        Object t = txnNew();
        txnCall(t, "setLayerStack", new Class<?>[]{SCC, int.class}, sc, 0);
        txnCall(t, "setLayer", new Class<?>[]{SCC, int.class}, sc, 2099990000);
        txnCall(t, "setPosition", new Class<?>[]{SCC, float.class, float.class},
                sc, 0.0f, 0.0f);
        txnCall(t, "setAlpha", new Class<?>[]{SCC, float.class}, sc, 1.0f);
        txnCall(t, "setTrustedOverlay", new Class<?>[]{SCC, boolean.class}, sc, true);
        txnCall(t, "show", new Class<?>[]{SCC}, sc);
        TXN.getMethod("apply").invoke(t);
        return sc;
    }

    public static void main(String[] args) {
        int w = 1440, h = 3168;
        try {
            if (args.length > 0) w = Integer.parseInt(args[0]);
            if (args.length > 1) h = Integer.parseInt(args[1]);
        } catch (Exception e) { Log.e(TAG, "args", e); }
        if (w < 2 || h < 2) { w = 1440; h = 3168; }
        Log.i(TAG, "start " + w + "x" + h);
        try {
            System.load("/data/local/tmp/vtouch-ui/libtestimgui.so");
        } catch (Throwable t) { Log.e(TAG, "load so", t); return; }
        if (nativeInit(w, h) != 0) { Log.e(TAG, "nativeInit failed"); System.exit(3); }
        /* system Context（**必须主线程取**：ActivityThread.systemMain() 放非主线程会让整个进程静默消失；
         * 且主线程要先 prepareMainLooper()，否则它内部 new Handler 抛 "Can't create handler ..."）。 */
        try {
            android.os.Looper.prepareMainLooper();
            Class<?> atCls = Class.forName("android.app.ActivityThread");
            Object at = atCls.getMethod("systemMain").invoke(null);
            sysCtx = atCls.getMethod("getSystemContext").invoke(at);
            Log.i(TAG, "system Context 获取成功=" + (sysCtx != null));
        } catch (Throwable t) { Log.w(TAG, "取 system Context 失败 → 回落轮询", t); }

        Object layer = null;
        int[] disp = queryDisplay(w, h, 0);
        try {
            SCC = Class.forName("android.view.SurfaceControl");
            TXN = Class.forName("android.view.SurfaceControl$Transaction");
            startDisplayListener();   /* 事件驱动优先；注册失败自动回落轮询 */
            layer = makeLayer("vtouch-ui", disp[0], disp[1]);
            nativeOnDisplay(disp[0], disp[1], disp[2]);
            nativeOnSurface(0, newSurface(layer));
            Log.i(TAG, "layer up " + disp[0] + "x" + disp[1] + " rot=" + disp[2]);
        } catch (Throwable t) { Log.e(TAG, "layer", t); System.exit(2); }
        /* 主循环三件事：
         *  ① 转屏遮挡的收尾：转屏时先把图层 alpha 归 0（旋转是非等比缩放，准备期间旧尺寸帧铺新屏
         *     必然被拉伸），等 native 报"新 surface 首帧已提交"再恢复 —— 用真实信号，不靠定时猜。
         *  ② 图层可见性：轮询 native 的「图层要不要显示」（「关闭 UI」→ alpha=0 + setVisibility(false)）。
         *  ③ 显示方向/尺寸：**事件驱动**（公开 API DisplayManager.registerDisplayListener）+ 500ms
         *     观察窗（回调常早于状态更新，直接读会拿到旧值 → 白做一次换绑、还漏掉这次旋转）；
         *     注册失败回落 320ms 轮询，注册成功也留 2s 一次的漏事件保险。
         * 变化处理顺序：先遮挡 → 再改 buffer / 换新 Surface → native 首帧上屏 → 恢复。 */
        mainTh = Thread.currentThread();
        boolean vis = true;          /* 逻辑可见性（native 说的要不要显示） */
        boolean guard = false;       /* 转屏遮挡中：此期间不碰 alpha（由遮挡逻辑管） */
        long guardT0 = 0;
        int tick = 0;
        long visWarn = 0, dispWarn = 0;   /* 各失败路径的限频时刻 */
        for (;;) {
            /* 平时 40ms（可见性判定跟手够了）；**转屏期间收紧到 5ms** ——
             * 这两段等待（观察窗内查值、遮挡期内等首帧）直接决定"屏幕上看不到面板"的时长：
             * 原来各要等最多一个 40ms 周期，收紧后各 ≤5ms。 */
            boolean tight = guard || System.currentTimeMillis() < settleUntil;
            try { Thread.sleep(tight ? 5 : 40); } catch (Throwable t) {}
            /* ① 转屏遮挡收尾：越早恢复越好（此刻屏幕是隐的） */
            if (guard) {
                boolean done = nativeTakeSwapDone() != 0;
                if (done || System.currentTimeMillis() - guardT0 > 500) {
                    try {
                        Object tt = txnNew();
                        txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layer, vis ? 1.0f : 0.0f);
                        TXN.getMethod("apply").invoke(tt);
                        Log.i(TAG, "转屏遮挡结束（" + (done ? "首帧已上屏" : "500ms 兜底")
                                  + "，用时 " + (System.currentTimeMillis() - guardT0) + "ms）");
                        guard = false;
                    } catch (Throwable t) {
                        long now0 = System.currentTimeMillis();
                        if (now0 - visWarn > 3000) { visWarn = now0; Log.w(TAG, "转屏恢复 alpha 失败（下轮重试）", t); }
                    }
                }
            }
            /* ② 图层可见性（关闭 UI / 恢复）：遮挡期间不碰 alpha；失败只重试自己。 */
            if (!guard)
            try {
                boolean want = nativeWantLayerVisible() != 0;
                if (want != vis) {
                    boolean applied = true;
                    try {
                        Object tt = txnNew();
                        try {
                            txnCall(tt, "setVisibility", new Class<?>[]{SCC, boolean.class}, layer, want);
                        } catch (Throwable e) {
                            Log.w(TAG, "setVisibility 不可用，退回 alpha");   /* 隐藏 API 变了也能关掉 */
                        }
                        txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layer, want ? 1.0f : 0.0f);
                        TXN.getMethod("apply").invoke(tt);
                    } catch (Throwable e) {
                        /* 切换失败：**不提交 vis**（下一轮重试），但日志限频 —— 以前这里每 40ms
                         * 抛一条堆栈把 logcat 刷满，真正的问题反而被淹掉。 */
                        applied = false;
                        long now0 = System.currentTimeMillis();
                        if (now0 - visWarn > 3000) {
                            visWarn = now0;
                            Log.w(TAG, "图层可见性切换失败（保留旧值，下轮重试）", e);
                        }
                    }
                    if (applied) {
                        vis = want;
                        Log.i(TAG, "layer visible=" + want);
                    }
                }
            } catch (Throwable t) { Log.e(TAG, "visibility poll", t); }
            /* ③ 显示方向/尺寸：观察窗内每轮查；否则按兜底周期（注册成功 2s / 失败 320ms）。 */
            boolean settling = System.currentTimeMillis() < settleUntil;
            int period = (listenerOk && !settling) ? 50 : 8;
            if (settling || (tick++ % period) == 0) {
                try {
                    int[] d2 = queryDisplay(disp[0], disp[1], disp[2]);
                    if (d2[0] != disp[0] || d2[1] != disp[1] || d2[2] != disp[2]) {
                        /* 先遮挡（这一帧起屏幕上看不到面板）→ 再改尺寸 / 换新 Surface → 等首帧上屏恢复。
                         * 诊断开关 VTOUCH_UI_NOGUARD=1：**不遮挡**，把平时只有 ~10ms 的错位状态
                         * 持续成整段换绑时间（~300ms），这样 9fps 的录屏也能拍下来看它到底什么样。 */
                        guard = true; guardT0 = System.currentTimeMillis();
                        Object tt = txnNew();
                        if (!"1".equals(System.getenv("VTOUCH_UI_NOGUARD")))
                            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layer, 0.0f);
                        txnCall(tt, "setBufferSize", new Class<?>[]{SCC, int.class, int.class},
                                layer, d2[0], d2[1]);
                        txnCall(tt, "setPosition", new Class<?>[]{SCC, float.class, float.class},
                                layer, 0.0f, 0.0f);
                        TXN.getMethod("apply").invoke(tt);
                        nativeOnDisplay(d2[0], d2[1], d2[2]);
                        nativeOnSurface(0, newSurface(layer));
                        Log.i(TAG, "display " + d2[0] + "x" + d2[1] + " rot=" + d2[2]
                                  + "（图层已遮挡，等新 surface 首帧上屏）");
                        disp = d2;   /* 全部成功才提交：中途抛错就停在旧值，下一轮重试 */
                    }
                } catch (Throwable t) {
                    long now1 = System.currentTimeMillis();
                    if (now1 - dispWarn > 3000) {
                        dispWarn = now1;
                        Log.w(TAG, "显示变化处理失败（保留旧状态，下轮重试）", t);
                    }
                }
            }
        }
    }
}
