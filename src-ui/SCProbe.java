import android.util.Log;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

/* SCProbe — 全反射探测（只 import android.util.Log，android-24 stub 可编）。
 * 运行：CLASSPATH=.../scprobe.dex app_process /system/bin SCProbe（应用侧 root 起）。
 * 只读 + 建一个 8x8 测试图层（立即销毁），不碰触摸/grab。
 */
public class SCProbe {
    static final String TAG = "SCProbe";
    static void out(String s) { Log.i(TAG, s); }

    static void invokeTxn(Class<?> txn, Object t, String name, Class<?>[] ps, Object... a) throws Throwable {
        Method m = txn.getDeclaredMethod(name, ps);
        m.setAccessible(true);
        m.invoke(t, a);
    }

    public static void main(String[] args) {
        try {
            Class<?> scc = Class.forName("android.view.SurfaceControl");
            Class<?> txn = Class.forName("android.view.SurfaceControl$Transaction");
            Class<?> surf = Class.forName("android.view.Surface");
            /* P1: Builder 建 8x8 层 */
            Object sc = null;
            try {
                Class<?> bld = Class.forName("android.view.SurfaceControl$Builder");
                Object b = bld.getDeclaredConstructor().newInstance();
                bld.getMethod("setName", String.class).invoke(b, "vt-probe");
                bld.getMethod("setBufferSize", int.class, int.class).invoke(b, 8, 8);
                bld.getMethod("setFormat", int.class).invoke(b, 1); /* TRANSLUCENT */
                sc = bld.getMethod("build").invoke(b);
                out("P1 build ok valid=" + scc.getMethod("isValid").invoke(sc));
            } catch (Throwable t) { out("P1 FAIL " + t); }
            /* P2: Transaction 声明方法（签名全打） */
            StringBuilder sb = new StringBuilder("P2");
            for (Method m : txn.getDeclaredMethods()) {
                String n = m.getName();
                if (n.startsWith("set") || n.equals("reparent") || n.equals("apply")
                        || n.equals("show") || n.equals("hide") || n.equals("remove")
                        || n.equals("merge")) {
                    sb.append(" [").append(n);
                    for (Class<?> p : m.getParameterTypes()) sb.append(" ").append(p.getSimpleName());
                    sb.append("]");
                }
            }
            out(sb.toString());
            /* P3: SurfaceControl 声明方法 */
            StringBuilder sb2 = new StringBuilder("P3");
            for (Method m : scc.getDeclaredMethods()) {
                String n = m.getName();
                if (n.contains("Display") || n.contains("Transaction") || n.equals("isValid")
                        || n.equals("getHandle")) {
                    sb2.append(" [").append(n);
                    for (Class<?> p : m.getParameterTypes()) sb2.append(" ").append(p.getSimpleName());
                    sb2.append("]");
                }
            }
            out(sb2.toString());
            /* P4: Surface 构造/拷贝（签名全打） */
            StringBuilder sb3 = new StringBuilder("P4");
            for (Constructor<?> c : surf.getDeclaredConstructors()) {
                sb3.append(" ctor[");
                for (Class<?> p : c.getParameterTypes()) sb3.append(" ").append(p.getSimpleName());
                sb3.append("]");
            }
            for (Method m : surf.getDeclaredMethods()) {
                String n = m.getName();
                if (n.equals("copyFrom") || n.equals("transferFrom") || n.equals("isValid")
                        || n.equals("readFromParcel") || n.equals("writeToParcel")) {
                    sb3.append(" [").append(n);
                    for (Class<?> p : m.getParameterTypes()) sb3.append(" ").append(p.getSimpleName());
                    sb3.append("]");
                }
            }
            out(sb3.toString());
            /* P6: TrustedOverlay 相关签名 */
            StringBuilder sb4 = new StringBuilder("P6");
            for (Method m : txn.getDeclaredMethods()) {
                if (m.getName().contains("rusted")) {
                    sb4.append(" [").append(m.getName());
                    for (Class<?> p : m.getParameterTypes()) sb4.append(" ").append(p.getSimpleName());
                    sb4.append("]");
                }
            }
            out(sb4.toString());
            /* P5: 实挂 display（64x64 红色不透明，停留 6 秒供截屏验证后摘除） */
            if (sc != null) {
                try {
                    Object t = txn.getDeclaredConstructor().newInstance();
                    invokeTxn(txn, t, "setLayerStack",
                            new Class<?>[]{scc, int.class}, sc, 0);
                    invokeTxn(txn, t, "setLayer",
                            new Class<?>[]{scc, int.class}, sc, 2100000000);
                    invokeTxn(txn, t, "setPosition",
                            new Class<?>[]{scc, float.class, float.class}, sc, 100.0f, 100.0f);
                    invokeTxn(txn, t, "setBufferSize",
                            new Class<?>[]{scc, int.class, int.class}, sc, 64, 64);
                    Method setColor = txn.getDeclaredMethod("setColor", scc, float[].class);
                    setColor.setAccessible(true);
                    setColor.invoke(t, sc, new float[]{1.0f, 0.0f, 0.0f});
                    invokeTxn(txn, t, "show", new Class<?>[]{scc}, sc);
                    invokeTxn(txn, t, "setAlpha",
                            new Class<?>[]{scc, float.class}, sc, 1.0f);
                    txn.getMethod("apply").invoke(t);
                    out("P5 layer SHOWN red 64x64 at 100,100 for 6s");
                    try { Thread.sleep(6000); } catch (Throwable tt) {}
                    Object t2 = txn.getDeclaredConstructor().newInstance();
                    invokeTxn(txn, t2, "remove", new Class<?>[]{scc}, sc);
                    txn.getMethod("apply").invoke(t2);
                    out("P5 layer removed");
                } catch (Throwable t) { out("P5 FAIL " + t); }
            }
            out("DONE");
        } catch (Throwable t) { out("FATAL " + t); }
    }
}
