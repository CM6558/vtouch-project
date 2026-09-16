/* rotprobe.c —— 旋转策略 B 探针的 native 部分（纯 GLES2，不引 ImGui，保持探针最小）。
 *
 * 验证目标：**旋转时不重建 Surface / EGLSurface，只改 buffer 几何**能不能正常出画。
 *   · Java 侧：同一图层 setBufferSize + setPosition（事务），不再 newSurface
 *   · native 侧（本文件）：收到 nativeOnDisplay 后**不销毁 EGLSurface**，只调
 *     ANativeWindow_setBuffersGeometry(win, w, h, RGBA_8888)，然后继续用同一个 EGLSurface 画
 *   · 若这条链可行，日志会稳定输出 geom/win/egl 三个尺寸一致；若不行，会在 swap 或 query 上暴露错误
 *
 * 画面元素（全部按当前 buffer 像素坐标画，所以"几何是否跟上旋转"一眼可判）：
 *   满屏边框 + 四角 L 标 + 正圆 + 正方形 + 顶部方向锚（条数 = rotation 索引 + 1）
 *   · 正圆画成椭圆 = buffer 几何没跟上（拉伸）
 *   · 角标被切掉 = 尺寸/裁切不对
 *   · 锚不朝上 = 90° 错位
 */
#define _GNU_SOURCE
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include <jni.h>

#define LOGT "RotProbe"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOGT, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOGT, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOGT, __VA_ARGS__)

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;   /* 换绑/几何变化立刻唤醒渲染线程 */
static ANativeWindow *g_win;          /* 当前 window */
static ANativeWindow *g_win_new;      /* Java 送来的新 Surface 对应的 window（模式 C） */
static volatile int g_swap_win;       /* 待换绑：EGL 归渲染线程管，不在 JNI 线程动 */
static int g_sw, g_sh;                /* 当前 EGLSurface 真实尺寸：画坐标必须用它，
                                       * 不能用 ANativeWindow_getWidth/Height（那是图层 default 几何，
                                       * 换绑/换几何后与真实缓冲区不一致 → 实测会把场景画成错位椭圆） */
static volatile int g_running = 1, g_frame;
static volatile int g_dw = 1440, g_dh = 3168, g_drot;
static volatile int g_geom_pending;   /* 需要 ANativeWindow_setBuffersGeometry */
static int g_swap_fail, g_logged_geom = -1, g_mode;   /* 0=B 1=A 2=C（重建）3=E（固定竖屏图层+绘制旋转）*/
static volatile int g_after_swap;     /* 换绑后待打点：首帧真正上屏时记一行（量换绑延迟） */
/* ---- 模式 D：双槽（双图层 + 双 EGLSurface + 单 GL 上下文）---- */
static ANativeWindow *g_win2[2];
static EGLSurface     g_surf2[2];
static int  g_slot_cur;               /* 当前可见槽 */
static volatile int g_prep_slot = -1;  /* 待准备（不可见）的槽 */
static volatile int g_prep_w, g_prep_h;
static int g_slotw[2], g_sloth[2];    /* 各槽目标尺寸（surface 建的时候用） */
static int g_cw, g_ch;                /* 内容画布尺寸 = 用户当前朝向的逻辑尺寸（rot 0/2: w×h；rot 1/3: h×w）*/

/* 策略 E：内容坐标(px) → 缓冲区坐标(px)。缓冲区永远是竖屏尺寸，旋转只体现在这里。
 * 四个方向都是「旋转 + 平移」，无缩放 —— 所以圆永远是圆。 */
static void c2b(float cx, float cy, float *bx, float *by)
{
    switch (g_drot) {
        case 1:  *bx = cy;                *by = (float)g_sh - cx; break;   /* 内容旋转 90°CCW */
        case 3:  *bx = (float)g_sw - cy;  *by = cx;              break;   /* 内容旋转 90°CW  */
        case 2:  *bx = (float)g_sw - cx;  *by = (float)g_sh - cy; break;  /* 180° */
        default: *bx = cx;                *by = cy;              break;
    }
}
static volatile int g_diag;           /* 置 1 = 下一帧打一次全尺寸诊断 */
static jmethodID g_mid_hidden_frame;
static volatile int g_swap_done;      /* 换绑后首帧已上屏（供 Java 恢复图层 alpha） */
static JavaVM *g_jvm;                 /* native → Java 回调：首帧上屏即通知，省掉轮询延迟 */
static jclass  g_cls;
static jmethodID g_mid_first_frame;

static void notify_java_first_frame(void)
{
    JNIEnv *env = NULL;
    if (!g_jvm || !g_cls || !g_mid_first_frame) return;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) return;
        (*env)->CallStaticVoidMethod(env, g_cls, g_mid_first_frame);
        (*g_jvm)->DetachCurrentThread(g_jvm);
        return;
    }
    (*env)->CallStaticVoidMethod(env, g_cls, g_mid_first_frame);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLConfig  g_cfg;
static GLuint g_prog, g_vbo;
static int g_gl_ready;                /* program/VBO 是否已就绪（必须在有 surface current 之后建）*/
static GLint  g_uColor, g_aPos;

static const char *VS =
    "attribute vec2 aPos; uniform vec4 uColor; varying vec4 vColor;\n"
    "void main(){ vColor = uColor; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *FS =
    "precision mediump float; varying vec4 vColor;\n"
    "void main(){ gl_FragColor = vColor; }\n";

static GLuint mk_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static int gl_init(void)
{
    GLuint vs = mk_shader(GL_VERTEX_SHADER, VS), fs = mk_shader(GL_FRAGMENT_SHADER, FS);
    GLint ok = 0;
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs); glAttachShader(g_prog, fs); glLinkProgram(g_prog);
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { LOGE("program link 失败"); return -1; }
    g_aPos = glGetAttribLocation(g_prog, "aPos");
    g_uColor = glGetUniformLocation(g_prog, "uColor");
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);      /* 必须绑：否则 glBufferData/glVertexAttribPointer 都作用在缓冲区 0 上 */
    glUseProgram(g_prog);                      /* 必须选：否则没有任何 program 生效，只出清屏色 */
    glEnableVertexAttribArray((GLuint)g_aPos);
    glVertexAttribPointer((GLuint)g_aPos, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    {
        GLenum e = glGetError();
        if (e) LOGE("gl_init 后 glGetError=0x%x（着色器/VBO 状态有问题）", e);
        else LOGI("gl_init ok（program/VBO/attrib 就绪）");
    }
    return 0;
}

/* 内容像素坐标（原点左上）→ 先按 rot 旋进缓冲区，再转 NDC。
 * bw/bh 忽略（保留签名以免改动所有调用点）：缓冲区尺寸用 g_sw/g_sh。 */
static void px2ndc(float x, float y, float bw, float bh, float *ox, float *oy)
{
    float bx, by;
    (void)bw; (void)bh;
    c2b(x, y, &bx, &by);
    *ox = (bx / (float)g_sw) * 2.0f - 1.0f;
    *oy = 1.0f - (by / (float)g_sh) * 2.0f;
}

static void draw_verts(const float *v, int n, GLenum mode, float r, float g, float b)
{
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * 2 * sizeof(float)), v, GL_STREAM_DRAW);
    glUniform4f(g_uColor, r, g, b, 1.0f);
    glDrawArrays(mode, 0, n);
}

static void quad(ANativeWindow *w, float x0, float y0, float x1, float y1, float r, float g, float b)
{
    float bw = (float)g_sw, bh = (float)g_sh;
    (void)w;
    float ax, ay, bx, by, v[12];
    if (bw < 1 || bh < 1) return;
    px2ndc(x0, y0, bw, bh, &ax, &ay);
    px2ndc(x1, y1, bw, bh, &bx, &by);
    v[0]=ax; v[1]=ay;  v[2]=bx; v[3]=ay;  v[4]=ax; v[5]=by;
    v[6]=ax; v[7]=by;  v[8]=bx; v[9]=ay;  v[10]=bx; v[11]=by;
    draw_verts(v, 6, GL_TRIANGLES, r, g, b);
}

static const int CIR[96][2] = {
    { 10000,     0}, {  9979,   654}, {  9914,  1305}, {  9808,  1951}, {  9659,  2588}, {  9469,  3214},
    {  9239,  3827}, {  8969,  4423}, {  8660,  5000}, {  8315,  5556}, {  7934,  6088}, {  7518,  6593},
    {  7071,  7071}, {  6593,  7518}, {  6088,  7934}, {  5556,  8315}, {  5000,  8660}, {  4423,  8969},
    {  3827,  9239}, {  3214,  9469}, {  2588,  9659}, {  1951,  9808}, {  1305,  9914}, {   654,  9979},
    {     0, 10000}, {  -654,  9979}, { -1305,  9914}, { -1951,  9808}, { -2588,  9659}, { -3214,  9469},
    { -3827,  9239}, { -4423,  8969}, { -5000,  8660}, { -5556,  8315}, { -6088,  7934}, { -6593,  7518},
    { -7071,  7071}, { -7518,  6593}, { -7934,  6088}, { -8315,  5556}, { -8660,  5000}, { -8969,  4423},
    { -9239,  3827}, { -9469,  3214}, { -9659,  2588}, { -9808,  1951}, { -9914,  1305}, { -9979,   654},
    {-10000,     0}, { -9979,  -654}, { -9914, -1305}, { -9808, -1951}, { -9659, -2588}, { -9469, -3214},
    { -9239, -3827}, { -8969, -4423}, { -8660, -5000}, { -8315, -5556}, { -7934, -6088}, { -7518, -6593},
    { -7071, -7071}, { -6593, -7518}, { -6088, -7934}, { -5556, -8315}, { -5000, -8660}, { -4423, -8969},
    { -3827, -9239}, { -3214, -9469}, { -2588, -9659}, { -1951, -9808}, { -1305, -9914}, {  -654, -9979},
    {     0,-10000}, {   654, -9979}, {  1305, -9914}, {  1951, -9808}, {  2588, -9659}, {  3214, -9469},
    {  3827, -9239}, {  4423, -8969}, {  5000, -8660}, {  5556, -8315}, {  6088, -7934}, {  6593, -7518},
    {  7071, -7071}, {  7518, -6593}, {  7934, -6088}, {  8315, -5556}, {  8660, -5000}, {  8969, -4423},
    {  9239, -3827}, {  9469, -3214}, {  9659, -2588}, {  9808, -1951}, {  9914, -1305}, {  9979,  -654},
};

/* 单位圆表（×10000）：-O2 会把成对 sinf/cosf 融成 sincosf/sincos，而 bionic(API24) 不导出这两个符号
 * → dlopen 直接失败。探针里干脆不碰 libm。 */
static void circle(ANativeWindow *w, float cx, float cy, float rad, float r, float g, float b)
{
    enum { SEG = 96 };
    float bw = (float)g_sw, bh = (float)g_sh;
    (void)w;
    float v[(SEG + 2) * 2];
    int i;
    if (bw < 1 || bh < 1) return;
    px2ndc(cx, cy, bw, bh, &v[0], &v[1]);
    for (i = 0; i < SEG; i++) {
        px2ndc(cx + rad * CIR[i][0] / 10000.0f, cy + rad * CIR[i][1] / 10000.0f, bw, bh,
               &v[(i + 1) * 2], &v[(i + 1) * 2 + 1]);
    }
    v[(SEG + 1) * 2] = v[2]; v[(SEG + 1) * 2 + 1] = v[3];   /* 闭合 */
    draw_verts(v, SEG + 2, GL_TRIANGLE_FAN, r, g, b);
}

static void draw_scene(ANativeWindow *w)
{
    float bw = (float)g_cw, bh = (float)g_ch;   /* 按用户当前朝向布局，旋转交给 px2ndc */
    float t = 6.0f;                      /* 边框粗细 */
    float m = 120.0f;                    /* 角标长度 */
    float side = (bw < bh ? bw : bh) * 0.5f;
    int i, n = g_drot + 1;

    if (bw < 1 || bh < 1) return;
    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);   /* 深底：与桌面区分 */
    glClear(GL_COLOR_BUFFER_BIT);

    /* 边框（四角都在 = 没被裁） */
    quad(w, 0, 0, bw, t, 0.9f, 0.9f, 0.9f);
    quad(w, 0, bh - t, bw, bh, 0.9f, 0.9f, 0.9f);
    quad(w, 0, 0, t, bh, 0.9f, 0.9f, 0.9f);
    quad(w, bw - t, 0, bw, bh, 0.9f, 0.9f, 0.9f);

    /* 四角 L 标（绿） */
    quad(w, 0, 0, m, 12, 0.2f, 1.0f, 0.3f);
    quad(w, 0, 0, 12, m, 0.2f, 1.0f, 0.3f);
    quad(w, bw - m, 0, bw, 12, 0.2f, 1.0f, 0.3f);
    quad(w, bw - 12, 0, bw, m, 0.2f, 1.0f, 0.3f);
    quad(w, 0, bh - 12, m, bh, 0.2f, 1.0f, 0.3f);
    quad(w, 0, bh - m, 12, bh, 0.2f, 1.0f, 0.3f);
    quad(w, bw - m, bh - 12, bw, bh, 0.2f, 1.0f, 0.3f);
    quad(w, bw - 12, bh - m, bw, bh, 0.2f, 1.0f, 0.3f);

    /* 正圆（红）：画成椭圆 = 拉伸 */
    circle(w, bw * 0.5f, bh * 0.5f, (bw < bh ? bw : bh) * 0.35f, 1.0f, 0.25f, 0.25f);

    /* 正方形（蓝，边长取短边一半）：画成矩形 = 拉伸 */
    quad(w, (bw - side) * 0.5f, (bh - side) * 0.5f, (bw + side) * 0.5f, (bh - side) * 0.5f + 8, 0.25f, 0.5f, 1.0f);
    quad(w, (bw - side) * 0.5f, (bh + side) * 0.5f - 8, (bw + side) * 0.5f, (bh + side) * 0.5f, 0.25f, 0.5f, 1.0f);
    quad(w, (bw - side) * 0.5f, (bh - side) * 0.5f, (bw - side) * 0.5f + 8, (bh + side) * 0.5f, 0.25f, 0.5f, 1.0f);
    quad(w, (bw + side) * 0.5f - 8, (bh - side) * 0.5f, (bw + side) * 0.5f, (bh + side) * 0.5f, 0.25f, 0.5f, 1.0f);

    /* 顶部方向锚：一根横条 + 一根竖条组成 "┬"，绝对朝上；旁边 n 根小条 = rotation+1 */
    quad(w, bw * 0.5f - 150, 40, bw * 0.5f + 150, 52, 1.0f, 0.9f, 0.2f);
    quad(w, bw * 0.5f - 6, 40, bw * 0.5f + 6, 190, 1.0f, 0.9f, 0.2f);
    for (i = 0; i < n; i++) {
        float x = 40 + i * 56.0f;
        quad(w, x, 40, x + 40, 80, i == 0 ? 1.0f : 0.35f, 1.0f, i == 0 ? 1.0f : 0.35f);
    }
}

static int egl_init(ANativeWindow *w)
{
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLint n = 0;
    ANativeWindow_setBuffersGeometry(w, 0, 0, WINDOW_FORMAT_RGBA_8888);
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY) { LOGE("eglGetDisplay 失败"); return -1; }
    if (!eglInitialize(g_dpy, NULL, NULL)) { LOGE("eglInitialize 失败 0x%x", eglGetError()); return -1; }
    if (!eglChooseConfig(g_dpy, cfg_attr, &g_cfg, 1, &n) || n < 1) { LOGE("eglChooseConfig 失败"); return -1; }
    g_surf = eglCreateWindowSurface(g_dpy, g_cfg, w, NULL);
    if (g_surf == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface 失败 0x%x", eglGetError()); return -1; }
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT) { LOGE("eglCreateContext 失败 0x%x", eglGetError()); return -1; }
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) { LOGE("eglMakeCurrent 失败 0x%x", eglGetError()); return -1; }
    if (gl_init() != 0) return -1;
    eglSwapInterval(g_dpy, 1);
    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &g_sw);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &g_sh);
    LOGI("egl ready（surface=%dx%d / window default=%dx%d）", g_sw, g_sh,
         ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
    return 0;
}

/* 策略 B 的关键动作：只改几何，不换 Surface */
static void apply_geometry(ANativeWindow *w)
{
    EGLint ew = -1, eh = -1;
    if (g_mode != 1) ANativeWindow_setBuffersGeometry(w, g_dw, g_dh, WINDOW_FORMAT_RGBA_8888);   /* 模式 A 不动几何 */
    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &ew);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &eh);
    g_sw = (int)ew; g_sh = (int)eh;
    LOGI("geom set: 模式=%s disp=%dx%d rot=%d | window=%dx%d | eglsurface=%dx%d | 同一个 EGLSurface（未重建）",
         g_mode == 1 ? "A" : g_mode == 2 ? "C" : "B", g_dw, g_dh, g_drot, ANativeWindow_getWidth(w), ANativeWindow_getHeight(w), (int)ew, (int)eh);
}

/* 模式 D 的 EGL 引导：建 display/config/context/program，但不绑定任何 window（各槽各自建 surface） */
static int egl_bootstrap(void)
{
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLint n = 0;
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY) return -1;
    if (!eglInitialize(g_dpy, NULL, NULL)) return -1;
    if (!eglChooseConfig(g_dpy, cfg_attr, &g_cfg, 1, &n) || n < 1) return -1;
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT) { LOGE("eglCreateContext 失败 0x%x", eglGetError()); return -1; }
    /* 不在无 surface 的情况下要求 current（部分驱动不支持 surfaceless）：
     * program 推迟到"某个槽的 surface 已 current"之后再建。 */
    LOGI("EGL 引导完成（display/config/context 就绪，等待槽 surface 后再建 program）");
    return 0;
}

static void *render_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int made = 0;
        /* 模式 C 的换绑：帧边界上销毁旧 EGLSurface、换 window、下一帧重建 EGLSurface（GL/ImGui 上下文保留） */
        if (g_swap_win) {
            ANativeWindow *nw;
            EGLSurface ns = EGL_NO_SURFACE;
            pthread_mutex_lock(&mu);
            nw = g_win_new; g_win_new = 0; g_swap_win = 0;
            pthread_mutex_unlock(&mu);
            /* 顺序：① 新 surface 先建好并提交首帧 → ② 才销毁旧的（重叠，不留空档） */
            if (nw && g_dpy != EGL_NO_DISPLAY) {
                ANativeWindow_setBuffersGeometry(nw, g_dw, g_dh, WINDOW_FORMAT_RGBA_8888);
                ns = eglCreateWindowSurface(g_dpy, g_cfg, nw, NULL);
                if (ns != EGL_NO_SURFACE) {
                    eglMakeCurrent(g_dpy, ns, ns, g_ctx);
                    eglQuerySurface(g_dpy, ns, EGL_WIDTH, &g_sw);
                    eglQuerySurface(g_dpy, ns, EGL_HEIGHT, &g_sh);
                    draw_scene(nw);                       /* 新尺寸首帧：先画再提交 */
                    if (eglSwapBuffers(g_dpy, ns))
                        LOGI("新 EGLSurface 首帧已提交（%dx%d）", g_sw, g_sh);
                    else
                        LOGE("新 EGLSurface 首帧提交失败 (0x%x)", eglGetError());
                } else {
                    LOGE("eglCreateWindowSurface(新) 失败 (0x%x) —— 保留旧 surface", eglGetError());
                }
            }
            if (ns != EGL_NO_SURFACE) {                   /* 新 surface 就绪，才销毁旧的 */
                eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (g_surf != EGL_NO_SURFACE) eglDestroySurface(g_dpy, g_surf);
                if (g_win) ANativeWindow_release(g_win);
                g_win = nw; g_surf = ns;
                g_after_swap = 1;
                LOGI("换绑完成：新 surface %dx%d 已接手，旧 surface 已销毁（先建后销）", g_sw, g_sh);
                notify_java_first_frame();                 /* 立刻回调 Java 恢复 alpha（不再靠轮询） */
            }
        }
        if (g_prep_slot >= 0 && g_win2[g_prep_slot]) {     /* 模式 D：准备隐藏槽（不可见期间做完整套重建） */
            int s = g_prep_slot;
            g_prep_slot = -1;
            pthread_mutex_lock(&mu);
            {
                int attempt;
                for (attempt = 0; attempt < 6 && g_surf2[s] == EGL_NO_SURFACE; attempt++) {
                    ANativeWindow_setBuffersGeometry(g_win2[s], g_prep_w, g_prep_h, WINDOW_FORMAT_RGBA_8888);
                    g_surf2[s] = eglCreateWindowSurface(g_dpy, g_cfg, g_win2[s], NULL);
                    if (g_surf2[s] == EGL_NO_SURFACE) {
                        LOGW("隐藏槽 %d 建 surface 第 %d 次失败 (0x%x)，重试", s, attempt + 1, eglGetError());
                        usleep(40000);
                    }
                }
            }
            if (g_surf2[s] != EGL_NO_SURFACE) {
                EGLint w2 = 0, h2 = 0;
                if (!eglMakeCurrent(g_dpy, g_surf2[s], g_surf2[s], g_ctx))
                    LOGE("隐藏槽 %d eglMakeCurrent 失败 0x%x", s, eglGetError());
                if (!g_gl_ready) {
                    if (gl_init() == 0) { g_gl_ready = 1; eglSwapInterval(g_dpy, 1); LOGI("gl_init ok（模式 D / 隐藏槽）"); }
                    else LOGE("gl_init 失败（隐藏槽）");
                }
                eglQuerySurface(g_dpy, g_surf2[s], EGL_WIDTH, &w2);
                eglQuerySurface(g_dpy, g_surf2[s], EGL_HEIGHT, &h2);
                g_sw = (int)w2; g_sh = (int)h2;
                draw_scene(g_win2[s]);                     /* 在不可见状态下画好新尺寸首帧 */
                if (eglSwapBuffers(g_dpy, g_surf2[s])) {
                    LOGI("隐藏槽 %d 首帧已提交（%dx%d）→ 通知 Java 原子翻转", s, g_sw, g_sh);
                    pthread_mutex_unlock(&mu);
                    if (g_jvm && g_cls && g_mid_hidden_frame) {
                        JNIEnv *e = NULL;
                        if ((*g_jvm)->GetEnv(g_jvm, (void **)&e, JNI_VERSION_1_6) != JNI_OK) {
                            (*g_jvm)->AttachCurrentThread(g_jvm, &e, NULL);
                            (*e)->CallStaticVoidMethod(e, g_cls, g_mid_hidden_frame);
                            (*g_jvm)->DetachCurrentThread(g_jvm);
                        } else {
                            (*e)->CallStaticVoidMethod(e, g_cls, g_mid_hidden_frame);
                        }
                    }
                    g_slot_cur = s;                            /* Java 已在同一事务里翻转可见性 */
                    g_diag = 1;
                    continue;
                }
                LOGE("隐藏槽 %d 首帧提交失败 (0x%x)", s, eglGetError());
            } else {
                LOGE("隐藏槽 %d eglCreateWindowSurface 失败 (0x%x)", s, eglGetError());
            }
            pthread_mutex_unlock(&mu);
        }
        pthread_mutex_lock(&mu);
        if (g_win && g_surf == EGL_NO_SURFACE) made = (egl_init(g_win) == 0);   /* 首次 / 换绑后重建 */
        pthread_mutex_unlock(&mu);
        if ((!g_win && !g_win2[0] && !g_win2[1]) || (g_surf == EGL_NO_SURFACE && !made && !g_win2[0] && !g_win2[1])) {
            pthread_mutex_lock(&mu); pthread_cond_timedwait(&cv, &mu, &(struct timespec){0, 10000000}); pthread_mutex_unlock(&mu);
            continue;
        }

        if (g_geom_pending && g_surf != EGL_NO_SURFACE) {
            apply_geometry(g_win);
            g_geom_pending = 0;
            g_logged_geom = g_drot;
        }
        if (g_surf != EGL_NO_SURFACE) {
            draw_scene(g_win);
            if (g_frame < 2) { GLenum e = glGetError(); if (e) LOGE("首帧 glGetError=0x%x（没画出图元？）", e); }
            if (!eglSwapBuffers(g_dpy, g_surf)) {
                g_swap_fail++;
                if (g_swap_fail == 1 || g_swap_fail % 50 == 0)
                    LOGE("eglSwapBuffers 失败 x%d (0x%x)", g_swap_fail, eglGetError());
            } else {
                if (g_after_swap) {
                    g_after_swap = 0;
                    g_swap_done = 1;
                    LOGI("换绑后首帧已上屏（surface=%dx%d）", g_sw, g_sh);
                }
                if (g_swap_fail) {
                    LOGI("eglSwapBuffers 恢复正常（此前连续失败 %d 次）", g_swap_fail);
                    g_swap_fail = 0;
                }
            }
        }
        if (g_surf == EGL_NO_SURFACE && (g_win2[0] || g_win2[1])) {
            int i, need;
            if (g_dpy == EGL_NO_DISPLAY) egl_bootstrap();       /* 模式 D 第一次在这里初始化 EGL */
            for (i = 0; i < 2; i++) {                            /* 给有 window 没 surface 的槽补建 */
                if (!g_win2[i] || g_surf2[i] != EGL_NO_SURFACE || g_slotw[i] <= 0) continue;
                need = (i == g_slot_cur || 1);
                if (!need) continue;
                ANativeWindow_setBuffersGeometry(g_win2[i], g_slotw[i], g_sloth[i], WINDOW_FORMAT_RGBA_8888);
                g_surf2[i] = eglCreateWindowSurface(g_dpy, g_cfg, g_win2[i], NULL);
                LOGI("槽 %d EGLSurface 建好：%dx%d (%s)", i, g_slotw[i], g_sloth[i],
                     g_surf2[i] != EGL_NO_SURFACE ? "ok" : "失败");
            }
        }
        if (g_surf2[g_slot_cur] != EGL_NO_SURFACE && g_surf == EGL_NO_SURFACE) {
            /* 模式 D：每帧画到当前可见槽（另一个槽在旋转时被准备，不可见） */
            EGLint w2 = 0, h2 = 0;
            if (!eglMakeCurrent(g_dpy, g_surf2[g_slot_cur], g_surf2[g_slot_cur], g_ctx))
                LOGE("槽 %d eglMakeCurrent 失败 0x%x", g_slot_cur, eglGetError());
            if (!g_gl_ready) {
                if (gl_init() == 0) { g_gl_ready = 1; eglSwapInterval(g_dpy, 1); LOGI("gl_init ok（模式 D，槽 surface 已 current）"); }
                else LOGE("gl_init 失败（模式 D）");
            }
            eglQuerySurface(g_dpy, g_surf2[g_slot_cur], EGL_WIDTH, &w2);
            eglQuerySurface(g_dpy, g_surf2[g_slot_cur], EGL_HEIGHT, &h2);
            g_sw = (int)w2; g_sh = (int)h2;
            if (g_frame < 3 || g_diag) {
                EGLint e0w=0,e0h=0,e1w=0,e1h=0;
                if (g_surf2[0] != EGL_NO_SURFACE) { eglQuerySurface(g_dpy,g_surf2[0],EGL_WIDTH,&e0w); eglQuerySurface(g_dpy,g_surf2[0],EGL_HEIGHT,&e0h); }
                if (g_surf2[1] != EGL_NO_SURFACE) { eglQuerySurface(g_dpy,g_surf2[1],EGL_WIDTH,&e1w); eglQuerySurface(g_dpy,g_surf2[1],EGL_HEIGHT,&e1h); }
                LOGI("DIAG frame=%d 显示=%dx%d | 绘制用=%dx%d cur_slot=%d | slot0 win=%dx%d surf=%dx%d | slot1 win=%dx%d surf=%dx%d",
                     g_frame, g_dw, g_dh, g_sw, g_sh, g_slot_cur,
                     g_win2[0]?ANativeWindow_getWidth(g_win2[0]):-1, g_win2[0]?ANativeWindow_getHeight(g_win2[0]):-1, (int)e0w, (int)e0h,
                     g_win2[1]?ANativeWindow_getWidth(g_win2[1]):-1, g_win2[1]?ANativeWindow_getHeight(g_win2[1]):-1, (int)e1w, (int)e1h);
                g_diag = 0;
            }
            draw_scene(g_win2[g_slot_cur]);
            if (!eglSwapBuffers(g_dpy, g_surf2[g_slot_cur])) {
                g_swap_fail++;
                if (g_swap_fail == 1 || g_swap_fail % 50 == 0)
                    LOGE("模式D eglSwapBuffers 失败 x%d (0x%x)", g_swap_fail, eglGetError());
            } else g_swap_fail = 0;
        }
        if ((++g_frame % 180) == 0)
            LOGI("frame=%d disp=%dx%d rot=%d swap_fail=%d cur_slot=%d", g_frame, g_dw, g_dh, g_drot, g_swap_fail, g_slot_cur);
        {   /* 条件变量等 10ms：有换绑/几何事件会被立刻唤醒（不再是纯轮询） */
            struct timespec ts; struct timeval tv;
            gettimeofday(&tv, NULL);
            ts.tv_sec = tv.tv_sec; ts.tv_nsec = (long)tv.tv_usec * 1000L + 10000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_mutex_lock(&mu);
            pthread_cond_timedwait(&cv, &mu, &ts);
            pthread_mutex_unlock(&mu);
        }
    }
    return NULL;
}

JNIEXPORT jint JNICALL Java_RotProbeMain_nativeStart(JNIEnv *env, jclass cls, jint w, jint h, jint mode)
{
    pthread_t th;
    g_cls = (jclass)(*env)->NewGlobalRef(env, cls);
    g_mid_first_frame = (*env)->GetStaticMethodID(env, cls, "onSwapFirstFrame", "()V");
    if (!g_mid_first_frame) LOGE("找不到 RotProbeMain.onSwapFirstFrame()");
    g_mid_hidden_frame = (*env)->GetStaticMethodID(env, cls, "onHiddenFirstFrame", "()V");
    if (!g_mid_hidden_frame) LOGE("找不到 RotProbeMain.onHiddenFirstFrame()");
    g_dw = w; g_dh = h; g_mode = mode;
    if (pthread_create(&th, NULL, render_thread, NULL) != 0) return -1;
    pthread_detach(th);
    LOGI("nativeStart %dx%d 线程已起（等 Surface）", (int)w, (int)h);
    return 0;
}

JNIEXPORT void JNICALL Java_RotProbeMain_nativeOnSurface(JNIEnv *env, jclass cls, jobject surf)
{
    ANativeWindow *w = ANativeWindow_fromSurface(env, surf);
    (void)cls;
    pthread_mutex_lock(&mu);
    if (g_win == 0) {                       /* 首次：直接用 */
        g_win = w; pthread_cond_broadcast(&cv);
        LOGI("首次 Surface：window=%p %dx%d", (void *)w, w ? ANativeWindow_getWidth(w) : -1, w ? ANativeWindow_getHeight(w) : -1);
    } else {                                /* 模式 C：交给渲染线程在帧边界换绑 */
        if (g_win_new) ANativeWindow_release(g_win_new);
        g_win_new = w; g_swap_win = 1; pthread_cond_broadcast(&cv);
        LOGI("收到新 Surface（模式 C 换绑）：window=%p %dx%d", (void *)w,
             w ? ANativeWindow_getWidth(w) : -1, w ? ANativeWindow_getHeight(w) : -1);
    }
    pthread_mutex_unlock(&mu);
}

/* 模式 D：把某个槽准备好（新尺寸 surface + 画首帧），完成后回调 Java 做原子翻转 */
JNIEXPORT void JNICALL Java_RotProbeMain_nativePrepareSlot(JNIEnv *env, jclass cls, jint slot, jint w, jint h)
{
    (void)env; (void)cls;
    g_prep_w = w; g_prep_h = h; g_prep_slot = slot; g_diag = 1;
    pthread_cond_broadcast(&cv);
    LOGI("准备隐藏槽 %d（目标 %dx%d）", (int)slot, (int)w, (int)h);
}

JNIEXPORT void JNICALL Java_RotProbeMain_nativeSurfaceSlot(JNIEnv *env, jclass cls, jint slot, jobject surf)
{
    ANativeWindow *w = ANativeWindow_fromSurface(env, surf);
    (void)cls;
    if (slot < 0 || slot > 1) return;
    pthread_mutex_lock(&mu);
    if (g_win2[slot]) ANativeWindow_release(g_win2[slot]);
    g_win2[slot] = w;
    g_slotw[slot] = w ? ANativeWindow_getWidth(w) : 0;
    g_sloth[slot] = w ? ANativeWindow_getHeight(w) : 0;
    if (g_surf2[slot] != EGL_NO_SURFACE && g_dpy != EGL_NO_DISPLAY) {
        eglDestroySurface(g_dpy, g_surf2[slot]);   /* 该槽的旧 surface 无需保留（它此刻不可见） */
        g_surf2[slot] = EGL_NO_SURFACE;
    }
    pthread_mutex_unlock(&mu);
    pthread_cond_broadcast(&cv);
    LOGI("槽 %d 收到 Surface：window=%p %dx%d", (int)slot, (void *)w,
         w ? ANativeWindow_getWidth(w) : -1, w ? ANativeWindow_getHeight(w) : -1);
}

JNIEXPORT jint JNICALL Java_RotProbeMain_nativeTakeSwapDone(JNIEnv *env, jclass cls)
{
    jint v;
    (void)env; (void)cls;
    v = g_swap_done; g_swap_done = 0;
    return v;
}

JNIEXPORT void JNICALL Java_RotProbeMain_nativeOnDisplay(JNIEnv *env, jclass cls, jint w, jint h, jint rot)
{
    (void)env; (void)cls;
    g_dw = w; g_dh = h; g_drot = rot;
    if (g_mode == 3) { g_cw = (rot == 1 || rot == 3) ? h : w; g_ch = (rot == 1 || rot == 3) ? w : h; }
    g_geom_pending = 1;
    pthread_cond_broadcast(&cv);
    LOGI("nativeOnDisplay %dx%d rot=%d → 排队改几何（不新建 Surface）", (int)w, (int)h, (int)rot);
}
