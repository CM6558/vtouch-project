import android.os.IBinder;
import android.os.Parcel;
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
    static native void nativeOnFlip(int slot);   /* 原子翻转完成后告诉 native 现在可见的是哪个槽 */
    static native void nativeOnDisplay(int w, int h, int rot);
    static native void nativeDestroy();
    static native int nativeWantLayerVisible();
    static native int nativeTakeSwapDone();      /* 转屏后"新 surface 首帧已提交"（读到即清零） */
    /* 视觉抓帧（T3.1）：轮询请求 / 读回帧区 / 报告失败 —— 帧头字段写入与内存序全在 JNI C 里做。 */
    static native int nativeVisPollRequest();    /* 0 = 无请求；≠0 = 待抓请求序号 */
    static native long nativeVisSubmitFrame(Object hb, int reqSeq, int rotation);  /* 读回 shm 帧区；返回耗时 ms，负 = 失败 */
    static native void nativeVisFailFrame(int reqSeq, int err);                    /* 写帧头失败标志，立即解阻核心 */
    /* 面板侧抓帧（T3.2）：模板 / 点集 / 找色吸色用；与核心帧区请求协议**完全独立的旁路**
     * （请求位与缓冲全在 JNI C 的静态区，不进共享内存、不干扰核心请求）。 */
    static native int nativeVisPanelPoll();                     /* 1 = 面板要一帧（读到即清） */
    static native int nativeVisPanelFrame(Object hb, int rotation);  /* 拷进面板侧缓冲；0 = 成功，负 = 错误码 */
    static native void nativeVisPanelFail(int err);             /* Java 侧失败（token/反射/capture） */

    /* 显示变化：事件驱动（公开 API DisplayManager.registerDisplayListener）。
     * 事件只告诉我们"去查"——实测回调常早于状态更新（getRotation() 仍返回旧值），所以配一个
     * 500ms 观察窗复查到值真变；注册失败自动回落轮询，绝不影响可用性。 */
    static Object sysCtx;
    static volatile long settleUntil = 0;
    static volatile boolean listenerOk = false;
    static volatile Thread mainTh;      /* 主循环线程：事件到达时打断它的 sleep，检测延迟从"最多一个周期"降到 ~0 */
    /* 事件回调里要用的：图层对象、最近一次已知显示状态、以及"回调已提前遮挡"的请求位 */
    static volatile Object layerRef;
    static volatile int[] dispNow = new int[]{1440, 3168, 0};
    static volatile boolean guardPending;
    /* 转屏模式：flip（默认）= 双图层原子翻转，屏幕不空；hide = 单图层遮挡换绑（旧行为）。 */
    static boolean rotFlip = true;

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
                                    /* 这一步是"位置切换看起来自然"的关键：回调**早于**状态更新（实测），
                                     * 等真值就要多等 5~10ms —— 那段时间面板还停在旧位置铺在新朝向里，
                                     * 正是用户看到的"位置闪现"。所以：
                                     *   ① 立刻把图层 alpha 归 0（此后任何错位帧都看不见）；
                                     *   ② 立刻按**预测**朝向重排面板：90° 旋转时尺寸必然交换
                                     *      (1440x3168 ↔ 3168x1440)，方向先猜 +1；
                                     *   ③ 真值到了只做校正（猜错也在遮挡里，看不见）。
                                     * 于是"屏幕转过去的那一刻"面板已经在新位置了。 */
                                    /* 双图层模式（默认）不需要在回调里遮挡/预测：可见槽继续出图，
                                     * 备用槽按新尺寸准备好后一个事务原子翻转 —— 屏幕既不空也不闪。
                                     * 单图层模式（VTOUCH_UI_ROT_MODE=hide）才在这里先遮挡。 */
                                    if (!rotFlip) {
                                        try {
                                            Object lr = layerRef;
                                            if (lr != null) {
                                                Object tt0 = txnNew();
                                                txnCall(tt0, "setAlpha", new Class<?>[]{SCC, float.class}, lr, 0.0f);
                                                TXN.getMethod("apply").invoke(tt0);
                                                guardPending = true;
                                            }
                                        } catch (Throwable t2) { /* 失败就让主循环按老路处理 */ }
                                    }
                                    /* 立刻叫醒主循环（否则要等它睡满一个周期才发现）。 */
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

    /* ---- 视觉抓帧（T3.1）：binder 拿 display token → captureDisplay → JNI 读回 shm 帧区 ----
     * 主循环每帧调 nativeVisPollRequest（廉价 JNI 读帧头）；有请求才走下面这条链。
     * 反射句柄懒初始化、失败可重试（旧 android.jar 编译 ⇒ ScreenCapture / HardwareBuffer 全反射，
     * recipe 逐字照 spec §2.1 / build/probe/VProbe7.java：事务 6 = getPhysicalDisplayIds（回复 skip 4B）→
     * 事务 7 = getPhysicalDisplayToken（skip 4B）；接口类不可反射加载，只能 binder 事务）。 */
    static boolean visReflectOk;
    static java.lang.reflect.Constructor<?> visBldCtor;
    static Class<?> visBldCls, visDcaCls;
    static java.lang.reflect.Method visCapM;
    static volatile IBinder visToken;
    static boolean visLoggedOnce;
    /* 视觉失败日志限频（T3.2 顺手项，T3.1 M-2）：首条带栈，之后最多每 3s 一行短句 ——
     * 反射 / token 失败会在每个请求上重试，不限频会把 logcat 刷满（真问题反而被淹掉）。 */
    static boolean visWarned;
    static long visWarnT;
    /* 抓帧隐藏面板（2026-10-05 用户需求）：面板在合成画面里，抓帧前把可见图层 alpha 归 0、
     * 等 SF 提交+合成更新再抓 —— 帧里不再带面板 UI / 区域。沉降 40ms（120Hz ~5 vsync）。
     * 自动保持隐藏（T7.4）：抓完不立即恢复 —— 150ms 内又来请求 → 复用隐藏态（跳过隐藏 + 40ms 沉降，
     * 支撑 ~60fps 持续搜索）；150ms 无请求 → 自动恢复（失败置 needShow 下轮补）。 */
    static final int VIS_HIDE_SETTLE_MS = 40;
    static final int VIS_KEEP_HIDDEN_MS = 150;
    static boolean visHideLogged;
    static boolean visKeepLogged;        /* 「复用保持隐藏」首次日志（限频，同 visHideLogged 口径） */
    static boolean skipShotOk;           /* 图层 skip-screenshot 已设（免隐藏抓帧）；makeLayer 探测成功置 true，
                                          * 老 ROM 无此方法 → false（回退隐藏+沉降路径，行为同 2026-10-05） */
    static void visWarn(String msg, Throwable t) {
        long now0 = System.currentTimeMillis();
        if (!visWarned) {
            visWarned = true;
            visWarnT = now0;
            if (t != null) Log.w(TAG, msg, t); else Log.w(TAG, msg);
            return;
        }
        if (now0 - visWarnT > 3000) {
            visWarnT = now0;
            Log.w(TAG, msg + "（持续失败，限频报告）");
        }
    }

    static boolean visInitReflect() {
        if (visReflectOk) return true;
        try {
            visBldCls = Class.forName("android.window.ScreenCapture$DisplayCaptureArgs$Builder");
            visBldCtor = visBldCls.getDeclaredConstructor(IBinder.class);
            visBldCtor.setAccessible(true);
            visDcaCls = Class.forName("android.window.ScreenCapture$DisplayCaptureArgs");
            visCapM = Class.forName("android.window.ScreenCapture").getMethod("captureDisplay", visDcaCls);
            visReflectOk = true;
            Log.i(TAG, "视觉抓帧反射就绪");
        } catch (Throwable t) {
            visWarn("视觉抓帧反射不可用（下次请求重试）", t);
        }
        return visReflectOk;
    }

    /* display token：懒初始化；失败不缓存（下次请求重试）。 */
    static IBinder visGetToken() {
        IBinder t = visToken;
        if (t != null) return t;
        synchronized (VTouchUI.class) {
            if (visToken != null) return visToken;
            try {
                Object sf = Class.forName("android.os.ServiceManager")
                                 .getMethod("getService", String.class).invoke(null, "SurfaceFlingerAIDL");
                if (!(sf instanceof IBinder)) { visWarn("视觉 token：SurfaceFlingerAIDL 拿不到", null); return null; }
                final String DESC = "android.gui.ISurfaceComposer";
                long pid = 0;
                Parcel d = Parcel.obtain(), r = Parcel.obtain();
                try {
                    d.writeInterfaceToken(DESC);
                    ((IBinder) sf).transact(6, d, r, 0);          /* 6 = getPhysicalDisplayIds */
                    int sz = r.dataSize();
                    r.setDataPosition(0);
                    if (sz >= 4) r.readInt();                     /* 回复头 4B 状态前缀（spec §2.1） */
                    long[] ids = r.createLongArray();
                    if ((ids == null || ids.length == 0) && sz >= 4) {
                        r.setDataPosition(0);                     /* 防御：无前缀实现 → 原样再试（探针同款） */
                        ids = r.createLongArray();
                    }
                    if (ids != null && ids.length > 0) pid = ids[0];
                } finally { d.recycle(); r.recycle(); }
                if (pid == 0) { visWarn("视觉 token：物理屏 id 拿不到", null); return null; }
                d = Parcel.obtain(); r = Parcel.obtain();
                try {
                    d.writeInterfaceToken(DESC);
                    d.writeLong(pid);
                    ((IBinder) sf).transact(7, d, r, 0);          /* 7 = getPhysicalDisplayToken(pid) */
                    int sz = r.dataSize();
                    r.setDataPosition(0);
                    if (sz >= 4) r.readInt();
                    IBinder tok = r.readStrongBinder();
                    if (tok == null && sz >= 4) {
                        r.setDataPosition(0);                     /* 防御：同 ids */
                        tok = r.readStrongBinder();
                    }
                    if (tok != null) {
                        visToken = tok;
                        Log.i(TAG, "视觉 token 就绪 pid=" + pid);
                        return tok;
                    }
                } finally { d.recycle(); r.recycle(); }
                visWarn("视觉 token：display token 拿不到", null);
            } catch (Throwable t2) {
                visWarn("视觉 token 获取失败（下次请求重试）", t2);
            }
            return null;
        }
    }

    /* 响应一次抓帧请求：captureDisplay → nativeVisSubmitFrame。失败路径统一 nativeVisFailFrame
     * （写帧头失败标志 + req_seq，让核心立即解阻，不等 1000ms 超时）；同步做（capture 4–7ms +
     * 读回可承受；挂死由核心超时兜底）。err 码表见 ui_glue.c 的 JNI 段注释。 */
    static void captureToShm(int reqSeq) {
        if (!visInitReflect()) { nativeVisFailFrame(reqSeq, -102); return; }
        IBinder token = visGetToken();
        if (token == null) { nativeVisFailFrame(reqSeq, -101); return; }
        Object hb = null;
        try {
            Object bld = visBldCtor.newInstance(token);
            Object args = visBldCls.getMethod("build").invoke(bld);
            Object shb = visCapM.invoke(null, args);
            if (shb != null) hb = shb.getClass().getMethod("getHardwareBuffer").invoke(shb);
            if (hb == null) { nativeVisFailFrame(reqSeq, -104); return; }
            long ms = nativeVisSubmitFrame(hb, reqSeq, dispNow[2]);
            if (ms < 0) {
                nativeVisFailFrame(reqSeq, (int) ms);          /* submit 失败没写 req_seq：补失败帧 */
                visWarn("vis 抓帧提交失败 ms=" + ms + " req=" + reqSeq, null);
            } else if (!visLoggedOnce) {
                visLoggedOnce = true;
                Log.i(TAG, "vis 抓帧就绪 首帧 " + ms + "ms");
            }
        } catch (Throwable t) {
            visToken = null;                                   /* token 可能失效：置空，下次请求重新拿 */
            nativeVisFailFrame(reqSeq, -103);
            visWarn("vis 抓帧失败（已报核心）", t);
        } finally {
            if (hb != null) try { hb.getClass().getMethod("close").invoke(hb); } catch (Throwable t2) { }
        }
    }

    /* 面板侧抓帧（T3.2）：模板页截帧 / 点集编辑 / 找色吸色用 —— 复用同一套反射与 token，
     * 但帧走 nativeVisPanelFrame 拷进**面板侧缓冲**（不进共享内存、不动核心帧区请求协议）。
     * 面板要帧时 nativeVisPanelPoll 读到（≤10ms）；失败路径统一 nativeVisPanelFail（面板显示错误）。 */
    static void captureToPanel() {
        if (!visInitReflect()) { nativeVisPanelFail(-102); return; }
        IBinder token = visGetToken();
        if (token == null) { nativeVisPanelFail(-101); return; }
        Object hb = null;
        try {
            Object bld = visBldCtor.newInstance(token);
            Object args = visBldCls.getMethod("build").invoke(bld);
            Object shb = visCapM.invoke(null, args);
            if (shb != null) hb = shb.getClass().getMethod("getHardwareBuffer").invoke(shb);
            if (hb == null) { nativeVisPanelFail(-104); return; }
            int rc = nativeVisPanelFrame(hb, dispNow[2]);
            if (rc != 0) {
                nativeVisPanelFail(rc);                        /* 拷贝失败：错误码交面板显示 */
                visWarn("vis 面板抓帧失败 rc=" + rc, null);
            }
        } catch (Throwable t) {
            visToken = null;                                   /* token 可能失效：置空，下次请求重新拿 */
            nativeVisPanelFail(-103);
            visWarn("vis 面板抓帧失败（已报面板）", t);
        } finally {
            if (hb != null) try { hb.getClass().getMethod("close").invoke(hb); } catch (Throwable t2) { }
        }
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
        /* 免隐藏抓帧（2026-10-05b）：图层标记 skip-screenshot —— 本层不进任何截图/抓屏（含
         * captureDisplay），抓帧不再需要把面板 alpha 归 0（消除「闪一下」）。老 ROM 无此方法
         * → skipShotOk 保持 false，抓帧回退「隐藏 + 沉降」路径。 */
        try {
            txnCall(t, "setSkipScreenshot", new Class<?>[]{SCC, boolean.class}, sc, true);
            if (!skipShotOk) { skipShotOk = true; Log.i(TAG, "vis 抓帧：图层 skip-screenshot 已设（免隐藏抓帧）"); }
        } catch (Throwable t2) {
            skipShotOk = false;
        }
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
        Object[] layers = new Object[2];   /* 双图层：layers[cur] 可见，另一个待命 */
        int cur = 0;
        rotFlip = !"hide".equals(System.getenv("VTOUCH_UI_ROT_MODE"));
        try {
            SCC = Class.forName("android.view.SurfaceControl");
            TXN = Class.forName("android.view.SurfaceControl$Transaction");
            startDisplayListener();   /* 事件驱动优先；注册失败自动回落轮询 */
            layer = makeLayer("vtouch-ui-A", disp[0], disp[1]);
            layers[0] = layer;
            if (rotFlip) {
                layers[1] = makeLayer("vtouch-ui-B", disp[0], disp[1]);
                Object tt0 = txnNew();
                txnCall(tt0, "setAlpha", new Class<?>[]{SCC, float.class}, layers[1], 0.0f);
                TXN.getMethod("apply").invoke(tt0);      /* B 起始隐藏 */
            }
            nativeOnDisplay(disp[0], disp[1], disp[2]);
            nativeOnSurface(0, newSurface(layer));
            layerRef = layer;          /* 单图层模式的事件回调里要立刻改它的 alpha */
            dispNow = disp;
            Log.i(TAG, "layer up " + disp[0] + "x" + disp[1] + " rot=" + disp[2]
                      + "  转屏模式=" + (rotFlip ? "flip（双图层原子翻转）" : "hide（遮挡换绑）"));
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
        boolean needShow = false;    /* 抓帧恢复失败：下一轮补恢复（否则面板会一直不可见） */
        boolean capHide = false;     /* 抓帧保持隐藏中（T7.4）：窗口到期前不恢复；再抓时复用隐藏态 */
        long capHideUntil = 0;       /* 保持隐藏窗口截止（墙钟 ms；到期无请求 → 自动恢复） */
        boolean guard = false;       /* 转屏遮挡中：此期间不碰 alpha（由遮挡逻辑管） */
        boolean changedSeen = false; /* 本轮遮挡期间是否真的查到了变化（没有就是伪事件，要尽快恢复） */
        long guardT0 = 0;
        int tick = 0;
        long visWarn = 0, dispWarn = 0;   /* 各失败路径的限频时刻 */
        for (;;) {
            /* 平时 10ms：视觉抓帧发现延迟 ≤10ms（满足 spec §2.2 ≤16ms / §9 全链 ≤25ms）；
             * **转屏期间收紧到 5ms 不变** —— 这两段等待（观察窗内查值、遮挡期内等首帧）直接决定
             * "屏幕上看不到面板"的时长：各等最多一个 10ms 周期，收紧后各 ≤5ms。 */
            boolean tight = guard || guardPending || System.currentTimeMillis() < settleUntil;
            try { Thread.sleep(tight ? 5 : 10); } catch (Throwable t) {}
            /* 事件回调已经先遮挡 + 按预测朝向排好位置了：接管它的遮挡状态 */
            if (guardPending) {
                guardPending = false;
                if (!guard) { guard = true; guardT0 = System.currentTimeMillis(); changedSeen = false; }
            }
            /* ① 转屏遮挡收尾：越早恢复越好（此刻屏幕是隐的） */
            if (guard) {
                boolean done = nativeTakeSwapDone() != 0;
                long gdt = System.currentTimeMillis() - guardT0;
                /* 伪事件（回调到了但方向/尺寸没变）：60ms 内没查到变化就尽快恢复，
                 * 不然每次无关的显示事件都会让面板黑 500ms。 */
                if (!changedSeen && gdt > 60) done = true;
                if (done || gdt > 500) {
                    try {
                        Object tt = txnNew();
                        if (rotFlip && changedSeen && layers[1] != null) {
                            /* **原子翻转**：一个事务里旧槽 alpha→0、新槽 alpha→1，SurfaceFlinger 一次提交，
                             * 中间不存在"两边都不可见"的帧 —— 这就是转屏零空白的来源。 */
                            int hid = 1 - cur;
                            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[cur], 0.0f);
                            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[hid], vis ? 1.0f : 0.0f);
                            TXN.getMethod("apply").invoke(tt);
                            nativeOnFlip(hid);
                            cur = hid;
                            Log.i(TAG, "原子翻转 → 可见槽 " + cur + "（用时 " + gdt + "ms，屏幕无空白）");
                        } else {
                            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[cur], vis ? 1.0f : 0.0f);
                            TXN.getMethod("apply").invoke(tt);
                            Log.i(TAG, "转屏遮挡结束（" + (done ? "首帧已上屏" : "500ms 兜底/伪事件")
                                      + "，用时 " + gdt + "ms）");
                        }
                        guard = false;
                    } catch (Throwable t) {
                        long now0 = System.currentTimeMillis();
                        if (now0 - visWarn > 3000) { visWarn = now0; Log.w(TAG, "转屏恢复 alpha 失败（下轮重试）", t); }
                    }
                }
            }
            /* ①b 抓帧恢复失败的重试（否则面板会一直不可见） */
            if (needShow && !guard && !capHide) {   /* 窗口期不抢 alpha（评审 M-3：防窗口被不断续期+帧带面板） */
                try {
                    Object tt = txnNew();
                    txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[cur], vis ? 1.0f : 0.0f);
                    TXN.getMethod("apply").invoke(tt);
                    needShow = false;
                } catch (Throwable t) { /* 留到下轮 */ }
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
                            txnCall(tt, "setVisibility", new Class<?>[]{SCC, boolean.class}, layers[cur], want);
                        } catch (Throwable e) {
                            Log.w(TAG, "setVisibility 不可用，退回 alpha");   /* 隐藏 API 变了也能关掉 */
                        }
                        txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[cur], want ? 1.0f : 0.0f);
                        if (rotFlip && layers[1] != null)   /* 待命槽恒为 0，只有翻转那一刻才上 */
                            txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[1 - cur], 0.0f);
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
                        capHide = false;   /* T7.4：可见性被 ② 改写 —— 保持隐藏窗口作废（下轮抓帧重新走隐藏 + 沉降） */
                        Log.i(TAG, "layer visible=" + want);
                    }
                }
            } catch (Throwable t) { Log.e(TAG, "visibility poll", t); }
            /* ③ 显示方向/尺寸：观察窗内每轮查；否则按兜底周期（注册成功 2s / 失败 320ms）。
             * 10ms 拍折算：200 拍 = 2s、32 拍 = 320ms（原 40ms 拍下为 50 / 8，墙钟节奏不变）。 */
            boolean settling = System.currentTimeMillis() < settleUntil;
            int period = (listenerOk && !settling) ? 200 : 32;
            if (settling || (tick++ % period) == 0) {
                try {
                    int[] d2 = queryDisplay(disp[0], disp[1], disp[2]);
                    if (d2[0] != disp[0] || d2[1] != disp[1] || d2[2] != disp[2]) {
                        changedSeen = true;
                        Object tt = txnNew();
                        int target;
                        if (rotFlip && layers[1] != null) {
                            /* 双图层（默认）：给**待命槽**按新尺寸准备 —— 可见槽继续出它自己的图，
                             * 屏幕全程有内容；待命槽画满两帧后一个事务原子翻转。 */
                            target = 1 - cur;
                            txnCall(tt, "setBufferSize", new Class<?>[]{SCC, int.class, int.class},
                                    layers[target], d2[0], d2[1]);
                            txnCall(tt, "setPosition", new Class<?>[]{SCC, float.class, float.class},
                                    layers[target], 0.0f, 0.0f);
                        } else {
                            /* 单图层（VTOUCH_UI_ROT_MODE=hide）：先遮挡再换绑，代价是那一段看不见面板。
                             * 诊断开关 VTOUCH_UI_NOGUARD=1 时不遮挡（把错位状态拉长便于录屏取证）。 */
                            target = cur;
                            if (!"1".equals(System.getenv("VTOUCH_UI_NOGUARD")))
                                txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[cur], 0.0f);
                            txnCall(tt, "setBufferSize", new Class<?>[]{SCC, int.class, int.class},
                                    layers[cur], d2[0], d2[1]);
                            txnCall(tt, "setPosition", new Class<?>[]{SCC, float.class, float.class},
                                    layers[cur], 0.0f, 0.0f);
                        }
                        TXN.getMethod("apply").invoke(tt);
                        nativeOnDisplay(d2[0], d2[1], d2[2]);
                        dispNow = d2;
                        nativeOnSurface(target, newSurface(layers[target]));
                        guard = true; guardT0 = System.currentTimeMillis();
                        Log.i(TAG, "display " + d2[0] + "x" + d2[1] + " rot=" + d2[2]
                                  + (rotFlip ? ("（待命槽 " + target + " 准备中，可见槽 " + cur + " 继续出图）")
                                             : "（图层已遮挡，等新 surface 首帧上屏）"));
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
            /* ④ 视觉抓帧（T3.1/T3.2/T7.4）：核心有请求才抓 —— 轮询是廉价 JNI 读（帧区 req_pending vs req_seq）；
             * 面板侧抓帧（模板/点集/吸色）同轮询、同同步做（capture 4–7ms + 读回，可承受；
             * 挂死由核心 1000ms 超时兜底 / 面板侧 3s 超时提示）。
             * **抓帧前隐藏面板**（2026-10-05）：面板在合成画面里，不隐藏则帧中带面板 UI 与区域；
             * 可见图层 alpha 归 0 → 沉降 → 抓。转屏遮挡（guard）期间 alpha 归遮挡逻辑管，
             * 不隐藏直接抓（面板彼时本就不可见/过渡中）。
             * **免隐藏抓帧**（2026-10-05b）：图层 setSkipScreenshot 生效（Android 13+）时完全跳过
             * 隐藏 / 沉降 / 保持窗口 —— 帧里天然没有面板与区域，用户无感（不再「闪一下」）；
             * 老 ROM 无此方法 → skipShotOk=false，回退上面的隐藏路径。
             * **自动保持隐藏**（T7.4）：抓完不立即恢复 —— 记 capHideUntil = now + 150ms；
             * 窗内又来请求 → 复用隐藏态（跳过隐藏 + 40ms 沉降，~60fps 搜索）；150ms 无请求 → 自动恢复
             * （失败置 needShow 下轮补，同旧口径）。单次查找 = 面板隐藏总时长 ~200ms（沉降 + 抓 + 窗口）。 */
            if (capHide && (guard || System.currentTimeMillis() >= capHideUntil)) {
                /* 保持隐藏窗口到期（或转屏遮挡接管 alpha）→ 恢复可见（失败置 needShow 下轮补） */
                if (guard) {
                    capHide = false;   /* 转屏遮挡接管：alpha 由 guard 收尾逻辑恢复 */
                } else {
                    try {
                        Object tt = txnNew();
                        txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[cur], vis ? 1.0f : 0.0f);
                        TXN.getMethod("apply").invoke(tt);
                        capHide = false;
                    } catch (Throwable t) {
                        capHide = false;   /* 交给 needShow 下轮重试（否则面板会一直不可见） */
                        needShow = true;
                        long now3 = System.currentTimeMillis();
                        if (now3 - visWarn > 3000) { visWarn = now3; Log.w(TAG, "vis 抓帧：面板恢复失败（下轮重试）", t); }
                    }
                }
            }
            {
                int vreq = nativeVisPollRequest();
                boolean pneed = nativeVisPanelPoll() != 0;
                if (vreq != 0 || pneed) {
                    boolean hid = false;
                    if (!guard && !skipShotOk) {   /* skip-screenshot 生效：帧天然无面板/区域，无需隐藏（不闪） */
                        if (capHide) {
                            hid = true;    /* 复用隐藏态：跳过隐藏 + 沉降（保持隐藏窗口内） */
                            if (!visKeepLogged) {
                                visKeepLogged = true;
                                Log.i(TAG, "vis 抓帧：保持隐藏窗口内再抓（跳过沉降 " + VIS_HIDE_SETTLE_MS + "ms）");
                            }
                        } else {
                            try {
                                Object tt = txnNew();
                                txnCall(tt, "setAlpha", new Class<?>[]{SCC, float.class}, layers[cur], 0.0f);
                                TXN.getMethod("apply").invoke(tt);
                                hid = true;
                                if (!visHideLogged) {
                                    visHideLogged = true;
                                    Log.i(TAG, "vis 抓帧：面板已隐藏（沉降 " + VIS_HIDE_SETTLE_MS + "ms）");
                                }
                                Thread.sleep(VIS_HIDE_SETTLE_MS);
                            } catch (Throwable t) {
                                long now2 = System.currentTimeMillis();
                                if (now2 - visWarn > 3000) { visWarn = now2; Log.w(TAG, "vis 抓帧：面板隐藏失败（照常抓帧）", t); }
                            }
                        }
                    }
                    try {
                        if (vreq != 0) captureToShm(vreq);
                        if (pneed) captureToPanel();
                    } finally {
                        if (hid) {
                            capHide = true;    /* 抓完不立即恢复：保持隐藏到窗口到期（T7.4） */
                            capHideUntil = System.currentTimeMillis() + VIS_KEEP_HIDDEN_MS;
                        }
                    }
                }
            }
        }
    }
}
