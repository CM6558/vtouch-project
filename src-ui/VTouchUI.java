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
    static native void nativeDestroy();

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
        if (nativeInit(w, h) != 0) { Log.e(TAG, "nativeInit failed"); return; }
        try {
            SCC = Class.forName("android.view.SurfaceControl");
            TXN = Class.forName("android.view.SurfaceControl$Transaction");
            Object layer = makeLayer("vtouch-ui", w, h);
            Class<?> surfCls = Class.forName("android.view.Surface");
            java.lang.reflect.Constructor<?> ctor = surfCls.getDeclaredConstructor(SCC);
            ctor.setAccessible(true);
            nativeOnSurface(0, (Surface) ctor.newInstance(layer));
            Log.i(TAG, "layer up");
        } catch (Throwable t) { Log.e(TAG, "layer", t); System.exit(2); }
        for (;;) {
            try { Thread.sleep(10000); } catch (Throwable t) {}
        }
    }
}
