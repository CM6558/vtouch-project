import android.util.Log;
import android.view.Surface;
import java.lang.reflect.Method;

/* RotProbeMain —— 旋转策略 B 探针的 app_process 入口（与 VTouchUI 同构，差别只有一处）。
 *
 * 策略 B：**保持同一个 SurfaceControl 图层 / 同一个 Surface / 同一个 EGLSurface**，
 *         旋转时只改 buffer 几何（server 侧 setBufferSize + client 侧 ANativeWindow_setBuffersGeometry），
 *         绝不新建 Surface、绝不换绑。
 *   → 相比现状（每次旋转新建 Surface + 换 EGLSurface）能省掉销毁/重建的闪顿与失败面。
 *
 * 屏幕上画的是"能不能一眼判死"的东西：满屏边框 + 四角 L 标 + 正圆 + 正方形 + 顶部方向锚
 *   · 正圆是圆 → buffer 几何对了（拉伸会变椭圆）
 *   · 四角 L 标都在 + 边框贴合 → 没被 crop/scale
 *   · 顶部锚条数 = rotation 索引，且"上"是真的上 → 没有 90° 错位
 *
 * 用法: CLASSPATH=<dir>/classes.dex app_process /system/bin --nice-name=rotprobe RotProbeMain <w> <h>
 */
public class RotProbeMain {
    static final String TAG = "RotProbe";
    static Class<?> SCC, TXN;

    static native int  nativeStart(int w, int h, int mode);   /* mode: 0=策略B 1=策略A */
    static native void nativeOnSurface(Surface surface);
    static native void nativeOnDisplay(int w, int h, int rot);
    static native int  nativeTakeSwapDone();   /* 换绑后首帧已上屏（一次性） */
    static native void nativePrepareSlot(int slot, int w, int h);   /* 模式 D：在隐藏槽上按新尺寸准备 */
    static native void nativeSurfaceSlot(int slot, Surface s);      /* 模式 D：把 surface 交给指定槽 */

    static final String[] DMG_NAMES = {
        "android.hardware.display.DisplayManagerGlobal",
        "android.view.DisplayManagerGlobal",
    };
    static Object mDmg;
    static Method mGetDisplay, mGetInfo, mGetRotation, mGetRealSize;
    static Class<?> cDisplay;
    static boolean reflectReady, reflectFailed, readWarned;
    static volatile boolean dispDirty;          /* DisplayListener 置位：有方向/尺寸变化 */
    static volatile long settleUntil;           /* 事件后的稳定观察窗截止时刻（回调常早于真实状态更新） */
    static volatile boolean listenerOk;
    static Object swapWaitLayer;                 /* 等首帧期间被临时隐藏的图层 */
    static Object[] layers = new Object[2];      /* 模式 D：双图层 */
    static int curIdx;                           /* 模式 D：当前可见层下标 */
    static boolean modeD;
    static long swapWaitT0;         /* 注册成功（失败则只走轮询兜底） */
    static Object sysCtx;                       /* 主线程取到的 system Context */

    static synchronized void initReflect() {
        if (reflectReady || reflectFailed) return;
        for (String cn : DMG_NAMES) {
            try {
                Class<?> dmg = Class.forName(cn);
                Object g = dmg.getMethod("getInstance").invoke(null);
                if (g == null) continue;
                mDmg = g;
                cDisplay = Class.forName("android.view.Display");
                try {
                    Method m = dmg.getDeclaredMethod("getDisplay", int.class);
                    m.setAccessible(true); mGetDisplay = m;
                } catch (Throwable e) { mGetDisplay = null; }
                try {
                    Method m = dmg.getDeclaredMethod("getDisplayInfo", int.class);
                    m.setAccessible(true); mGetInfo = m;
                } catch (Throwable e) { mGetInfo = null; }
                try { mGetRotation = cDisplay.getMethod("getRotation"); } catch (Throwable e) { mGetRotation = null; }
                try { mGetRealSize = cDisplay.getMethod("getRealSize", android.graphics.Point.class); }
                catch (Throwable e) { mGetRealSize = null; }
                reflectReady = true;
                Log.i(TAG, "display 反射就绪 via " + cn);
                return;
            } catch (Throwable t) { }
        }
        reflectFailed = true;
        Log.w(TAG, "display 反射不可用 → 沿用启动参数");
    }

    /* 事件驱动：注册 DisplayListener（隐藏接口，用动态代理避免编译期依赖），回调里只置位。
     * 注册失败不影响可用性 —— 主循环仍保留 320ms 轮询兜底。 */
    static void startDisplayListener() {
        Thread th = new Thread(new Runnable() {
            public void run() {
                try {
                    android.os.Looper.prepare();
                    final android.os.Handler h = new android.os.Handler();
                    /* 只用**公开** API：DisplayManager（API 17+）+ 主线程取到的 system Context。
                     * 注意：ActivityThread.systemMain() 必须在主线程调，否则进程直接死（踩过）。 */
                    if (sysCtx == null) { Log.w(TAG, "没有 system Context → 只走 40ms 轮询"); android.os.Looper.loop(); return; }
                    Object dm = sysCtx.getClass().getMethod("getSystemService", Class.class)
                                       .invoke(sysCtx, Class.forName("android.hardware.display.DisplayManager"));
                    Class<?> dl = Class.forName("android.hardware.display.DisplayManager$DisplayListener");
                    Object proxy = java.lang.reflect.Proxy.newProxyInstance(dl.getClassLoader(),
                        new Class<?>[]{dl}, new java.lang.reflect.InvocationHandler() {
                            public Object invoke(Object p, Method m, Object[] a2) {
                                if ("onDisplayChanged".equals(m.getName())) {
                                    dispDirty = true;
                                    /* 回调常常早于真实状态更新（实测 getRotation() 仍返回旧值 →
                                     * 会漏掉这次旋转、还白做一次换绑）。开 500ms 观察窗，窗内每 40ms 复查到稳定为止。 */
                                    settleUntil = System.currentTimeMillis() + 500;
                                    Log.i(TAG, "DisplayListener: onDisplayChanged（公开 API，事件到达，开 500ms 观察窗）");
                                }
                                return null;
                            }
                        });
                    dm.getClass().getMethod("registerDisplayListener", dl, android.os.Handler.class)
                                .invoke(dm, proxy, h);
                    listenerOk = true;
                    Log.i(TAG, "DisplayListener 注册成功（公开 API）：方向变化事件驱动，40ms 轮询仅作兜底");
                } catch (Throwable t) {
                    Log.w(TAG, "DisplayListener 注册失败 → 只走 320ms 轮询兜底", t);
                }
                android.os.Looper.loop();
            }
        }, "disp-listener");
        th.setDaemon(true);
        th.start();
    }

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
            if (!readWarned) { readWarned = true; Log.w(TAG, "display 读取失败（保留上次值）", t); }
        }
        return null;
    }

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

    static Object txnNew() throws Throwable { return TXN.getDeclaredConstructor().newInstance(); }

    static void txnCall(Object t, String name, Class<?>[] ps, Object... a) throws Throwable {
        Method m = TXN.getDeclaredMethod(name, ps);
        m.setAccessible(true);
        m.invoke(t, a);
    }

    /* 策略 A 的正解：把 w×h 的 buffer「纯旋转 + 平移」到当前屏幕上（无极缩放）。
     * 旋转绕原点 (0,0)，所以必须配一个平移把内容搬回可视区：
     *   rot=1 → 旋转 90°CCW: (x,y)→(-y,x)   ⇒ x' ∈ (-h,0]  ⇒ 平移 +h
     *   rot=3 → 旋转 90°CW : (x,y)→(y,-x)   ⇒ y' ∈ (-w,0]  ⇒ 平移 +w
     *   rot=2 → 旋转 180°  : (x,y)→(-x,-y)  ⇒ 平移 (w,h)
     * 用 setMatrix(SurfaceControl, android.graphics.Matrix, float[])（setGeometry 会把图层摆到可视区外）。 */
    static void applyRotation(Object layer, int rot, int w, int h) throws Throwable {
        Class<?> matrixCls = Class.forName("android.graphics.Matrix");
        Object m = matrixCls.getConstructor().newInstance();
        java.lang.reflect.Method setRotate = matrixCls.getMethod("setRotate", float.class);
        float[] t = new float[]{0f, 0f, 0f};
        float deg;
        if (rot == 1)      { deg = -90f; t[0] = h; }
        else if (rot == 3) { deg =  90f; t[1] = w; }
        else if (rot == 2) { deg = 180f; t[0] = w; t[1] = h; }
        else               { deg =   0f; }
        setRotate.invoke(m, deg);
        Object tt = txnNew();
        txnCall(tt, "setMatrix", new Class<?>[]{SCC, matrixCls, float[].class}, layer, m, t);
        txnCall(tt, "setPosition", new Class<?>[]{SCC, float.class, float.class}, layer, 0.0f, 0.0f);
        TXN.getMethod("apply").invoke(tt);
        Log.i(TAG, "applyRotation rot=" + rot + " deg=" + deg + " translate=(" + t[0] + "," + t[1] + ")");
    }

    /* 这台 ROM 上 Transaction 到底有哪些变换 API：先打出来，不猜 */
    static void dumpTxnMethods() {
        try {
            Method[] ms = TXN.getDeclaredMethods();
            StringBuilder sb = new StringBuilder();
            for (Method m : ms) {
                String n = m.getName();
                if (!(n.contains("atrix") || n.contains("eometry") || n.contains("osition") || n.contains("rop"))) continue;
                sb.append(n).append('(');
                Class<?>[] ps = m.getParameterTypes();
                for (int i = 0; i < ps.length; i++) sb.append(i > 0 ? "," : "").append(ps[i].getSimpleName());
                sb.append(") ");
            }
            Log.i(TAG, "Transaction 变换类方法: " + sb);
        } catch (Throwable t) { Log.w(TAG, "dumpTxnMethods 失败", t); }
    }

    static Object makeLayer(String name, int w, int h) throws Throwable {
        Class<?> bld = Class.forName("android.view.SurfaceControl$Builder");
        Object b = bld.getDeclaredConstructor().newInstance();
        bld.getMethod("setName", String.class).invoke(b, name);
        bld.getMethod("setBufferSize", int.class, int.class).invoke(b, w, h);
        bld.getMethod("setFormat", int.class).invoke(b, 1);      /* TRANSLUCENT */
        Object sc = bld.getMethod("build").invoke(b);
        Object t = txnNew();
        txnCall(t, "setLayerStack", new Class<?>[]{SCC, int.class}, sc, 0);
        txnCall(t, "setLayer", new Class<?>[]{SCC, int.class}, sc, 2099990000);
        txnCall(t, "setPosition", new Class<?>[]{SCC, float.class, float.class}, sc, 0.0f, 0.0f);
        txnCall(t, "setAlpha", new Class<?>[]{SCC, float.class}, sc, 1.0f);
        txnCall(t, "setTrustedOverlay", new Class<?>[]{SCC, boolean.class}, sc, true);
        txnCall(t, "show", new Class<?>[]{SCC}, sc);
        TXN.getMethod("apply").invoke(t);
        return sc;
    }

    /* 模式 D：native 报"隐藏槽首帧已上屏" → 一个事务里原子翻转两个图层的 alpha（无中间帧） */
    public static void onHiddenFirstFrame() {
        int hidden = 1 - curIdx;
        try {
            Object tt = txnNew();
            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[curIdx], 0.0f);   /* 旧层保持隐藏 */
            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[hidden], 1.0f);   /* 新层提上来 */
            TXN.getMethod("apply").invoke(tt);
            Log.i(TAG, "原子翻转：可见层 A/B → " + hidden + "（同一事务，无空白无拉伸，用时 "
                       + (System.currentTimeMillis() - swapWaitT0) + "ms 自事件起）");
            curIdx = hidden;
        } catch (Throwable t) { Log.w(TAG, "原子翻转失败", t); }
    }

    /* native 回调：换绑后首帧已上屏 → 立刻恢复图层可见（不走 40ms 轮询，省 ~15ms） */
    public static void onSwapFirstFrame() {
        if (swapWaitLayer == null) return;
        try {
            Object tt = txnNew();
            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, swapWaitLayer, 1.0f);
            TXN.getMethod("apply").invoke(tt);
            Log.i(TAG, "图层已恢复可见（alpha=1，native 回调，用时 " + (System.currentTimeMillis() - swapWaitT0) + "ms）");
        } catch (Throwable t) { Log.w(TAG, "回调里恢复 alpha 失败", t); }
        swapWaitLayer = null;
    }

    public static void main(String[] args) {
        int w = 1440, h = 3168;
        try {
            if (args.length > 0) w = Integer.parseInt(args[0]);
            if (args.length > 1) h = Integer.parseInt(args[1]);
        } catch (Exception e) { Log.e(TAG, "args", e); }
        if (w < 2 || h < 2) { w = 1440; h = 3168; }
        String mode = (args.length > 2) ? args[2].toUpperCase() : "C";
        boolean modeA = mode.equals("A");
        modeD = mode.equals("D");
        boolean modeE = mode.equals("E");
        Log.i(TAG, "start " + w + "x" + h + "  模式=" + mode
              + (mode.equals("C") ? "（重建 Surface：兼容性主路径）"
                 : mode.equals("A") ? "（恒定 buffer + 合成器旋转，ROM 相关）" : "（只改几何，实验）"));
        /* 主线程取 system Context（ActivityThread.systemMain() 只能主线程调；
         * 放非主线程调会让整个进程静默消失 —— 踩过）。拿到 Context 才能用公开的 DisplayManager。 */
        try {
            /* systemMain() 内部会 new Handler → 主线程必须先有 Looper（否则 RuntimeException，
             * 异常原文："Can't create handler inside thread Thread[main,5,main] that has not called Looper.prepare()"） */
            android.os.Looper.prepareMainLooper();
            Class<?> atCls = Class.forName("android.app.ActivityThread");
            Object at = atCls.getMethod("systemMain").invoke(null);
            sysCtx = atCls.getMethod("getSystemContext").invoke(at);
            Log.i(TAG, "system Context 获取成功=" + (sysCtx != null));
        } catch (Throwable t) { Log.w(TAG, "取 system Context 失败 → 回落 40ms 轮询", t); }
        try {
            System.load("/data/local/tmp/vtouch-rotprobe/librotprobe.so");
        } catch (Throwable t) { Log.e(TAG, "load so", t); return; }
        if (nativeStart(w, h, mode.equals("A") ? 1 : mode.equals("B") ? 0 : mode.equals("E") ? 3 : 2) != 0) { Log.e(TAG, "nativeStart failed"); System.exit(3); }

        Object layer = null;
        int[] disp = queryDisplay(w, h, 0);
        try {
            SCC = Class.forName("android.view.SurfaceControl");
            TXN = Class.forName("android.view.SurfaceControl$Transaction");
            startDisplayListener();   /* 事件驱动优先，失败自动回落轮询 */
            /* 模式 D 的图层在上面 if (modeD) 块里建 */
            if (modeD) {   /* 双图层：都建在当前显示尺寸上，B 先隐藏 */
                layers[0] = makeLayer("vtouch-rotprobe-A", disp[0], disp[1]);
                layers[1] = makeLayer("vtouch-rotprobe-B", disp[0], disp[1]);
                Object tt0 = txnNew();
                txnCall(tt0, "setAlpha", new Class<?>[]{SCC, float.class}, layers[1], 0.0f);
                TXN.getMethod("apply").invoke(tt0);
                nativeSurfaceSlot(0, newSurface(layers[0]));
                nativeSurfaceSlot(1, newSurface(layers[1]));
                Log.i(TAG, "模式 D：双图层就绪 A(可见) B(隐藏) " + disp[0] + "x" + disp[1]);
            }
            if (modeD) {
                layer = layers[0];                   /* 模式 D 只用双图层，不建单图层 */
            } else if (modeE) {
                /* 策略 E：按**竖屏逻辑尺寸**建一次，此后永不 resize、永不重建、不动 alpha。
                 * 旋转只改绘制变换（native 侧 px2ndc），图层尺寸始终不变。 */
                layer = makeLayer("vtouch-rotprobe-E", w, h);
                nativeOnDisplay(disp[0], disp[1], disp[2]);
                nativeOnSurface(newSurface(layer));
                Log.i(TAG, "模式 E：固定竖屏图层 " + w + "x" + h + "（永不改尺寸），旋转只改绘制变换");
            } else {
                layer = makeLayer("vtouch-rotprobe", modeA ? w : disp[0], modeA ? h : disp[1]);
                /* 模式 A：图层 buffer 恒为竖屏逻辑尺寸，永不改；模式 C：按当前显示尺寸建，旋转时 setBufferSize 重建 */
                dumpTxnMethods();
                nativeOnDisplay(disp[0], disp[1], disp[2]);
                nativeOnSurface(newSurface(layer));  /* 只在启动时给一次 Surface */
                if (modeA && disp[2] != 0) applyRotation(layer, disp[2], w, h);   /* 启动就在横屏：立刻施加旋转 */
            }
            Log.i(TAG, "layer up " + disp[0] + "x" + disp[1] + " rot=" + disp[2]);
        } catch (Throwable t) { Log.e(TAG, "layer", t); System.exit(2); }
        if (layer == null) { Log.e(TAG, "layer 未建起"); System.exit(2); }

        /* 显示变化：只改 buffer 几何（server 事务 + native 侧 setBuffersGeometry），**不新建 Surface** */
        int tick = 0;
        for (;;) {
            try { Thread.sleep(40); } catch (Throwable t) {}
            /* 事件驱动优先：DisplayListener 注册成功时只在事件到达才查（外加 2s 一次的漏事件保险）；
             * 没注册成功才退回 40ms 轮询兜底（反射 3 次调用 ~15µs/次）。 */
            /* 换绑后恢复 alpha：等 native 明确回报"首帧已上屏"，另有 500ms 保险 */
            if (swapWaitLayer != null) {
                if (nativeTakeSwapDone() != 0 || System.currentTimeMillis() - swapWaitT0 > 500) {
                    try {
                        Object tt = txnNew();
                        txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, swapWaitLayer, 1.0f);
                        TXN.getMethod("apply").invoke(tt);
                        Log.i(TAG, "图层已恢复可见（alpha=1，用时 " + (System.currentTimeMillis() - swapWaitT0) + "ms）");
                    } catch (Throwable t) { Log.w(TAG, "恢复 alpha 失败", t); }
                    swapWaitLayer = null;
                }
            }
            boolean byEvent = dispDirty;
            dispDirty = false;
            boolean settling = System.currentTimeMillis() < settleUntil;   /* 观察窗内每 40ms 复查 */
            int period = (listenerOk && !settling) ? 50 : 1;
            if (!byEvent && !settling && (tick++ % period) != 0) continue;
            try {
                int[] d2 = queryDisplay(disp[0], disp[1], disp[2]);
                if (d2[0] != disp[0] || d2[1] != disp[1] || d2[2] != disp[2]) {
                    Object tt = txnNew();
                    if (modeE) {
                        /* 只通知 native 更新 rotation（它据此旋转绘制）；图层本身一点不动 */
                        nativeOnDisplay(d2[0], d2[1], d2[2]);
                        Log.i(TAG, "display " + d2[0] + "x" + d2[1] + " rot=" + d2[2]
                                  + "  → 模式E：仅更新绘制变换（图层未动、无重建、无空白）");
                    } else if (modeD) {
                        /* 在隐藏层上准备新尺寸（可见层原样不动，全程无空白），首帧上屏后由回调原子翻转 */
                        int hidden2 = 1 - curIdx;
                        swapWaitT0 = System.currentTimeMillis();
                        txnCall(tt, "setBufferSize", new Class<?>[]{SCC, int.class, int.class}, layers[hidden2], d2[0], d2[1]);
                        txnCall(tt, "setPosition", new Class<?>[]{SCC, float.class, float.class}, layers[hidden2], 0.0f, 0.0f);
                        /* 旋转是非等比缩放（2.2x），准备期间可见层仍显示旧尺寸 = 必然被拉伸 →
                         * 先把它隐掉，等隐藏槽首帧就绪后由"原子翻转"直接把新层提上来（不出现拉伸帧）。 */
                        txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[curIdx], 0.0f);
                        TXN.getMethod("apply").invoke(tt);
                        nativeOnDisplay(d2[0], d2[1], d2[2]);   /* 让 native 侧显示信息保持真实（诊断用） */
                        nativeSurfaceSlot(hidden2, newSurface(layers[hidden2]));
                        nativePrepareSlot(hidden2, d2[0], d2[1]);
                        Log.i(TAG, "display " + d2[0] + "x" + d2[1] + " rot=" + d2[2] + " → 模式D：隐藏层 "
                                   + hidden2 + " 按新尺寸准备，可见层已隐（等原子翻转接管）");
                    } else if (modeA) {
                        applyRotation(layer, d2[2], w, h);
                        nativeOnDisplay(d2[0], d2[1], d2[2]);
                        Log.i(TAG, "display " + d2[0] + "x" + d2[1] + " rot=" + d2[2]
                                  + "  → setMatrix 纯旋转（buffer 恒 " + w + "x" + h + "、Surface 未重建）");
                    } else {
                        /* 策略 C（兼容性主路径，旧面板同款）：改图层 buffer 尺寸 + 交一个新 Surface，
                         * native 侧销毁旧 EGLSurface、换绑新 window、重建 EGLSurface（GL/ImGui 上下文保留）。 */
                        /* 拉伸的根因：显示尺寸已经变了，而我们的 buffer 还是旧尺寸（1~2 帧）。
                         * 既然没法让 buffer 与尺寸切换原子完成，就把这几帧藏起来：
                         * alpha=0 → 改尺寸 + 交新 Surface → native 首帧真上屏 → alpha=1（用回读，不靠定时猜）。 */
                        txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layer, 0.0f);
                        txnCall(tt, "setBufferSize", new Class<?>[]{SCC, int.class, int.class}, layer, d2[0], d2[1]);
                        txnCall(tt, "setPosition", new Class<?>[]{SCC, float.class, float.class}, layer, 0.0f, 0.0f);
                        TXN.getMethod("apply").invoke(tt);
                        nativeOnDisplay(d2[0], d2[1], d2[2]);
                        nativeOnSurface(newSurface(layer));
                        swapWaitLayer = layer; swapWaitT0 = System.currentTimeMillis();
                        Log.i(TAG, "display " + d2[0] + "x" + d2[1] + " rot=" + d2[2]
                                  + "  → 模式C：setBufferSize + 新 Surface（native 将换绑 EGLSurface）");
                    }
                    disp = d2;
                }
            } catch (Throwable t) { Log.w(TAG, "显示变化处理失败（保留旧状态，下轮重试）", t); }
        }
    }
}
