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
        Object layer = null;
        int[] disp = queryDisplay(w, h, 0);
        try {
            SCC = Class.forName("android.view.SurfaceControl");
            TXN = Class.forName("android.view.SurfaceControl$Transaction");
            layer = makeLayer("vtouch-ui", disp[0], disp[1]);
            nativeOnDisplay(disp[0], disp[1], disp[2]);
            nativeOnSurface(0, newSurface(layer));
            Log.i(TAG, "layer up " + disp[0] + "x" + disp[1] + " rot=" + disp[2]);
        } catch (Throwable t) { Log.e(TAG, "layer", t); System.exit(2); }
        /* 主循环两件事：
         *  ① 轮询 native 的「图层要不要显示」——「关闭 UI」时把图层藏掉（alpha=0 +
         *     setVisibility(false)），屏幕零占用；恢复时一起还原。
         *  ② 轮询 display 方向/尺寸（每 ~300ms 一次）——变了就按新尺寸重建 buffer 并换一个新
         *     Surface 给 native（native 侧销毁旧 EGL surface、按新尺寸重排面板、换算坐标）。
         * 用轮询而不是回调：app_process 没有 Looper 泵消息，这两个延迟对人操作都够。 */
        boolean vis = true;
        int tick = 0;
        long visWarn = 0, dispWarn = 0;   /* 两条失败路径各自的限频时刻 */
        for (;;) {
            /* 40ms：可见性判定要跟手（点「关闭 UI」屏幕要立刻干净）；显示状态另按 tick 计数降频 */
            try { Thread.sleep(40); } catch (Throwable t) {}
            /* ① 图层可见性（关闭 UI / 恢复）：只影响合成，失败也只重试自己，不牵连 ②。 */
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
            /* ② 显示方向/尺寸（≈320ms 一次）：buffer + 新 Surface 一起提交，失败同样只重试自己。 */
            if ((tick++ % 8) == 0) {
                try {
                    int[] d2 = queryDisplay(disp[0], disp[1], disp[2]);
                    if (d2[0] != disp[0] || d2[1] != disp[1] || d2[2] != disp[2]) {
                        Object tt = txnNew();
                        txnCall(tt, "setBufferSize", new Class<?>[]{SCC, int.class, int.class},
                                layer, d2[0], d2[1]);
                        txnCall(tt, "setPosition", new Class<?>[]{SCC, float.class, float.class},
                                layer, 0.0f, 0.0f);
                        TXN.getMethod("apply").invoke(tt);
                        nativeOnDisplay(d2[0], d2[1], d2[2]);
                        nativeOnSurface(0, newSurface(layer));
                        Log.i(TAG, "display " + d2[0] + "x" + d2[1] + " rot=" + d2[2]);
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
