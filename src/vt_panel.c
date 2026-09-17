/* vt_panel.c —— 核心拉起面板子进程（VT_UI 构建才参与编译）。
 *
 * 职责边界（与 docs/UI_INTEGRATION.md §5 一致）：
 *   · 核心先把引擎完整拉起来（vtouch_init 全部成功），**然后**才起面板 —— 以核心为准；
 *   · 面板是核心的**子进程**，继承共享内存 fd（固定 VT_SHM_FD 号），窗口/绘制全在它自己那边；
 *   · 核心随时能停面板（stop 用 SIGTERM→SIGKILL 兜底）；面板崩了不影响注入，按策略重启（默认 1 分钟最多 3 次）。
 *
 * 踩过的坑（都写在这里，别再犯）：
 *   · fork 之后、exec 之前**只能调 async-signal-safe 函数** → 所有字符串（argv/envp）都提前拼好；
 *   · exec app_process **必须带完整 environ**（只传几个精选变量时 ART 起不来，子进程连 main 都进不去、静默退 0）；
 *   · 核心自己的 fd 一律 FD_CLOEXEC —— 否则面板会继承带 EVIOCGRAB 的 input_fd，
 *     核心退出后 grab 不释放、**物理触摸回不来**（这是硬性检查项）；
 *   · 子进程要额外 close 掉 3..1023 里除共享内存 fd 外的所有 fd；
 *   · **父进程给 memfd 设了 CLOEXEC（防泄漏），子进程必须显式清掉目标 fd 的 CLOEXEC** ——
 *     当 memfd 恰好就是 VT_SHM_FD 时不会走 dup2，而 dup2 是唯一会清 CLOEXEC 的操作，
 *     结果就是 fd 在 exec 时被关掉、随后被 ART 复用成别的 fd（实测被复用成 socket，
 *     面板拿不到共享内存）。这个坑真机踩过，见 scripts/ondev-ui-smoke.sh 的判据 2。
 */
#include "vt_internal.h"
#ifdef VT_UI
#ifndef VT_UI_PANEL

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ===== B 方案：面板三件套**嵌进核心**，启动时自解包 =====
 *
 * 设备上因此只需要**一个文件**（vtouchd_ui）：不用再推 classes.dex / libtestimgui.so /
 * libc++_shared.so，也就不存在"面板是旧的那一版"这类不一致 —— 版本永远来自同一个二进制。
 *
 * 打包方式（scripts/build.sh 的 ui 目标）：用 llvm-objcopy -I binary 把三个文件变成 .rodata
 * 里的符号对（`_binary_<名>_start/_end`），链接进核心；这里的表把它们写给面板目录。
 * 未链接时（-DVT_UI_NO_EMBED 或没走 ui 目标）符号为 NULL → 自动退回"用设备上已有的面板文件"。
 */
#ifndef VT_UI_NO_EMBED
extern const unsigned char _binary_classes_dex_start[] __attribute__((weak));
extern const unsigned char _binary_classes_dex_end[] __attribute__((weak));
extern const unsigned char _binary_libtestimgui_so_start[] __attribute__((weak));
extern const unsigned char _binary_libtestimgui_so_end[] __attribute__((weak));
extern const unsigned char _binary_libcxx_shared_so_start[] __attribute__((weak));
extern const unsigned char _binary_libcxx_shared_so_end[] __attribute__((weak));

static const struct {
    const unsigned char *s, *e;
    const char *name;
} S_emb[] = {
    { _binary_classes_dex_start,      _binary_classes_dex_end,      "classes.dex" },
    { _binary_libtestimgui_so_start,  _binary_libtestimgui_so_end,  "libtestimgui.so" },
    { _binary_libcxx_shared_so_start, _binary_libcxx_shared_so_end, "libc++_shared.so" },
};

/* 32 位 FNV-1a：只用来在日志里给出"这一版面板"的指纹（对账用，不做安全用途）。 */
static uint32_t fnv1a(const unsigned char *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/**
 * (vtouch-doc: vt_embed_materialize)
 * @brief 把嵌进核心的面板文件写到面板目录（先写 .tmp 再 rename，避免半截文件被加载）。
 * @param   dir      面板目录（如 /data/local/tmp/vtouch-ui）
 * @return  0 写了至少一个文件；-1 没得写（未链接）或写失败。
 * @note    无条件覆盖：宁可每次多写 ~2.8MB（约几十毫秒），也不留"设备上是旧面板"的可能。
 */
static int vt_embed_materialize(const char *dir)
{
    size_t i;
    int wrote = 0;
    if (!S_emb[0].s) return -1;                    /* 未链接：用设备上已有的 */
    for (i = 0; i < sizeof S_emb / sizeof S_emb[0]; i++) {
        char tmp[PATH_MAX], dst[PATH_MAX];
        size_t n;
        FILE *f;
        if (!S_emb[i].s || !S_emb[i].e) continue;
        n = (size_t)(S_emb[i].e - S_emb[i].s);
        if (n == 0) continue;
        snprintf(tmp, sizeof tmp, "%s/.%s.tmp", dir, S_emb[i].name);
        snprintf(dst, sizeof dst, "%s/%s", dir, S_emb[i].name);
        f = fopen(tmp, "wb");
        if (!f) { fprintf(stderr, "vtouchd: 面板自解包写 %s 失败: %s\n", tmp, strerror(errno)); return -1; }
        if (fwrite(S_emb[i].s, 1, n, f) != n) {
            fprintf(stderr, "vtouchd: 面板自解包 %s 写不全\n", tmp);
            fclose(f); unlink(tmp); return -1;
        }
        if (fclose(f) != 0 || rename(tmp, dst) != 0) {
            fprintf(stderr, "vtouchd: 面板自解包 rename %s 失败: %s\n", dst, strerror(errno));
            unlink(tmp); return -1;
        }
        chmod(dst, 0644);
        fprintf(stderr, "vtouchd: 面板自解包 %-16s %7zu 字节 fnv=%08x\n", S_emb[i].name, n, fnv1a(S_emb[i].s, n));
        wrote = 1;
    }
    return wrote ? 0 : -1;
}
#endif /* !VT_UI_NO_EMBED */

extern char **environ;

#define VT_PANEL_DIR_DEFAULT "/data/local/tmp/vtouch-ui"
#define VT_PANEL_APP_PROCESS "/system/bin/app_process"
#define VT_PANEL_MAX_RESTART 3        /* 1 分钟窗口内最多重启次数 */
#define VT_PANEL_RESTART_WIN 60
#define VT_PANEL_ABSENT_RETRY_MS 3000  /* 面板不在时距上次尝试 ≥3 秒才重试（单调时间，见 vt_panel_watchdog） */

static pid_t S_pid = -1;
static int   S_shm_fd = -1;
static int   S_restarts;
static long  S_win_start;
static int   S_shm_ok;          /* 共享内存检查已过（重启复用的就是同一个 fd；没有 shm 时重试没有意义） */
static long  S_absent_t0;       /* 进入“面板不在”态的时刻（单调毫秒，0 = 面板在）；见 vt_panel_watchdog */
static char  S_dir[PATH_MAX];
static char  S_dex[PATH_MAX + 32];
static char  S_clspath[PATH_MAX + 32];
static char  S_ldpath[PATH_MAX + 32];
static char  S_uidirenv[PATH_MAX + 32];
static char  S_shmfd[32];
static char  S_corepid[32];
static char  S_w[16], S_h[16];

static int pack_env(char ***out)
{
    int n = 0, i;
    char **e;
    while (environ[n]) n++;
    e = (char **)malloc((size_t)(n + 6) * sizeof(char *));
    if (!e) return -1;
    for (i = 0; i < n; i++) e[i] = environ[i];
    e[n++] = S_clspath;
    e[n++] = S_ldpath;
    e[n++] = S_uidirenv;
    e[n++] = S_shmfd;
    e[n++] = S_corepid;
    e[n] = NULL;
    *out = e;
    return 0;
}

/* 核心自己的这些 fd 绝不能被面板继承：带 grab 的 input_fd 尤甚。 */
static void cloexec_all(int shm_fd)
{
    int fds[6];
    int i, n = 0;
    fds[n++] = shm_fd;
    if (g.input_fd  >= 0) fds[n++] = g.input_fd;
    if (g.u_fd      >= 0) fds[n++] = g.u_fd;
    if (g.listen_fd >= 0) fds[n++] = g.listen_fd;
    if (g.client_fd >= 0) fds[n++] = g.client_fd;
    for (i = 0; i < n; i++) {
        int fl = fcntl(fds[i], F_GETFD, 0);
        if (fl >= 0) fcntl(fds[i], F_SETFD, fl | FD_CLOEXEC);
    }
}

/**
 * (vtouch-doc: vt_panel_start)
 * @brief 拉起面板子进程（fork/exec app_process），把共享内存 fd 传到固定 fd 号。
 * @param   shm_fd   共享内存 fd（核心侧）
 * @return  0 成功；-1 失败（调用方应按「无 UI 模式继续」处理，不能因此中断注入）。
 * @note    面板目录默认 /data/local/tmp/vtouch-ui（可用环境变量 VTOUCH_UI_DIR 覆盖）；
 *          目录里缺 classes.dex 时直接返回 -1（优雅降级，不报错退出）。
 */
int vt_panel_start(int shm_fd)
{
    const char *dir = getenv("VTOUCH_UI_DIR");
    char *argv[8];
    char **env = NULL;
    struct stat st;
    pid_t pid;

    if (shm_fd < 0) return -1;
    S_shm_ok = 1;                          /* 过了这关才值得重启：重启走的就是这个 fd（见看门狗） */
    if (!dir || !*dir) dir = VT_PANEL_DIR_DEFAULT;
    snprintf(S_dir, sizeof S_dir, "%s", dir);
    mkdir(S_dir, 0755);                            /* 目录可能还不存在（B 方案：设备上只有核心一个文件） */
#ifndef VT_UI_NO_EMBED
    if (vt_embed_materialize(S_dir) != 0)
        fprintf(stderr, "vtouchd: 核心未内嵌面板 → 用 %s 里已有的文件\n", S_dir);
#endif
    snprintf(S_dex, sizeof S_dex, "%s/classes.dex", S_dir);
    if (stat(S_dex, &st) != 0) {
        fprintf(stderr, "vtouchd: 面板未就绪（缺 %s）→ 以无 UI 模式继续\n", S_dex);
        return -1;
    }
    snprintf(S_clspath, sizeof S_clspath, "CLASSPATH=%s", S_dex);
    snprintf(S_ldpath, sizeof S_ldpath, "LD_LIBRARY_PATH=%s", S_dir);
    snprintf(S_uidirenv, sizeof S_uidirenv, "VTOUCH_UI_DIR=%s", S_dir);
    snprintf(S_shmfd, sizeof S_shmfd, "VTOUCH_SHM_FD=%d", VT_SHM_FD);
    snprintf(S_corepid, sizeof S_corepid, "VTOUCH_CORE_PID=%d", (int)getpid());
    snprintf(S_w, sizeof S_w, "%d", g.logical_width);
    snprintf(S_h, sizeof S_h, "%d", g.logical_height);

    argv[0] = (char *)"app_process";
    argv[1] = (char *)"/system/bin";
    argv[2] = (char *)"--nice-name=vtouch-ui";
    argv[3] = (char *)"VTouchUI";
    argv[4] = S_w;
    argv[5] = S_h;
    argv[6] = NULL;

    if (pack_env(&env) != 0) return -1;
    cloexec_all(shm_fd);                       /* 硬性检查项，见文件头注释 */
    S_shm_fd = shm_fd;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "vtouchd: fork 面板失败: %s\n", strerror(errno));
        free(env);
        return -1;
    }
    if (pid == 0) {
        int i;
        if (shm_fd != VT_SHM_FD && dup2(shm_fd, VT_SHM_FD) < 0) _exit(126);
        /* 必须显式清 CLOEXEC：memfd == VT_SHM_FD 时没走 dup2，CLOEXEC 会让它在 exec 时被关掉。
         * fcntl 在 async-signal-safe 列表里，fork 后可以用。 */
        if (fcntl(VT_SHM_FD, F_SETFD, 0) < 0) _exit(125);
        for (i = 3; i < 1024; i++) if (i != VT_SHM_FD) close(i);
        execve(VT_PANEL_APP_PROCESS, argv, env);
        _exit(127);
    }
    free(env);
    S_pid = pid;
    fprintf(stderr, "vtouchd: 面板已启动 pid=%d（dir=%s shm_fd=%d）\n", (int)pid, S_dir, shm_fd);
    return 0;
}

static void vt_panel_restart(void)
{
    struct timespec ts;
    long now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (long)ts.tv_sec;
    if (now - S_win_start > VT_PANEL_RESTART_WIN) { S_win_start = now; S_restarts = 0; }
    if (++S_restarts > VT_PANEL_MAX_RESTART) {
        fprintf(stderr, "vtouchd: 面板 %d 秒内退出 %d 次 → 不再自动重启（注入不受影响）\n",
                VT_PANEL_RESTART_WIN, S_restarts - 1);
        return;
    }
    fprintf(stderr, "vtouchd: 重启面板（第 %d 次）\n", S_restarts);
    vt_panel_start(S_shm_fd);
}

/**
 * (vtouch-doc: vt_panel_watchdog)
 * @brief 主循环每轮调：回收面板子进程、判心跳停滞、按策略重启。
 * @note   面板崩了**不影响注入**（它只是个观察者/输入面板）；重启有频率上限，避免反复拉起打爆 CPU。
 */
void vt_panel_watchdog(void)
{
    static uint32_t last_ui_hb;
    static int stall_ticks;
    uint32_t hb = vt_shm_ui_hb();
    int st;

    if (hb != last_ui_hb) { last_ui_hb = hb; stall_ticks = 0; }
    else if (++stall_ticks > 300) {            /* 约 3 秒没有面板心跳 */
        if (S_pid > 0) {
            fprintf(stderr, "vtouchd: 面板心跳停滞 3s → 杀掉它（pid=%d）并按策略重启\n", (int)S_pid);
            kill(S_pid, SIGKILL);
        }
        stall_ticks = 0;
    }
    /* “面板不在”独立成一支：stall_ticks 在上面那个 tick 里刚被清 0（进过 `> 300` 那一支就归零），
     * 再拿它判断等于恒假 —— 面板**首次**没起来（缺 classes.dex / shm fd 无效，vt_panel_start 返回 -1
     * 且 S_pid 停在 -1）时就永不重试。所以这里用只在本分支维护的 S_absent_t0（进入“面板不在”态的时刻）。
     *
     * 判据是**单调时间**，不是循环拍数：主循环的 poll 会因任何可读事件提前返回，负载下一拍远快于
     * 8ms（千拍/秒量级），「300 拍」可能在几十~几百毫秒内走完 ⇒ 1 分钟 3 次的预算被瞬间烧掉、
     * 随后整分钟不再重试（与 AGENTS.md / docs/CODE_WALKTHROUGH.md 的「每 ~3 秒重试一次」不符）。
     * 所以这里记「上次尝试重试的时刻」，距上次 ≥ VT_PANEL_ABSENT_RETRY_MS 才再试一次；
     * 面板在时把计时重置。心跳停滞的 stall_ticks 是另一套逻辑，不动。
     * 没拿到 shm 时不重试（重试只会沿用同一个坏 fd），S_shm_ok 在 vt_panel_start 过了 fd 检查后置位。 */
    if (S_pid <= 0) {
        struct timespec ts;
        long now;
        clock_gettime(CLOCK_MONOTONIC, &ts);                 /* 照抄 vt_panel_restart 的取时方式 */
        now = (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (S_absent_t0 == 0) S_absent_t0 = now;             /* 刚进入“面板不在”态 → 记起点 */
        if (now - S_absent_t0 >= VT_PANEL_ABSENT_RETRY_MS) { /* ~3s 还没面板 → 再拉一次 */
            S_absent_t0 = now;                               /* 记下这次尝试，下一次最早 +3s */
            if (S_shm_ok) vt_panel_restart();
        }
        return;
    }
    S_absent_t0 = 0;                        /* 面板在 → 重新计时（下次“面板不在”从头算 3s） */
    if (waitpid(S_pid, &st, WNOHANG) == S_pid) {
        fprintf(stderr, "vtouchd: 面板已退出 status=0x%x（核心继续跑，注入不受影响）\n", st);
        S_pid = -1;
        vt_panel_restart();
    }
}

/**
 * (vtouch-doc: vt_panel_stop)
 * @brief 停面板：SIGTERM → 最多等 800ms → SIGKILL 兜底。
 * @note    核心退出路径上必须先停面板，再放 EVIOCGRAB（保证面板不会在被抓的状态下继续画）。
 */
void vt_panel_stop(void)
{
    int i, st;
    if (S_pid <= 0) return;
    kill(S_pid, SIGTERM);
    for (i = 0; i < 40; i++) {
        if (waitpid(S_pid, &st, WNOHANG) == S_pid) {
            fprintf(stderr, "vtouchd: 面板已停止 pid=%d\n", (int)S_pid);
            S_pid = -1;
            return;
        }
        usleep(20000);
    }
    fprintf(stderr, "vtouchd: 面板不响应 SIGTERM → SIGKILL\n");
    kill(S_pid, SIGKILL);
    waitpid(S_pid, &st, 0);
    S_pid = -1;
}

#endif /* !VT_UI_PANEL */
#endif /* VT_UI */
