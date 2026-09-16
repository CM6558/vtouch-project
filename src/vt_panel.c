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
#include <sys/wait.h>

extern char **environ;

#define VT_PANEL_DIR_DEFAULT "/data/local/tmp/vtouch-ui"
#define VT_PANEL_APP_PROCESS "/system/bin/app_process"
#define VT_PANEL_MAX_RESTART 3        /* 1 分钟窗口内最多重启次数 */
#define VT_PANEL_RESTART_WIN 60

static pid_t S_pid = -1;
static int   S_shm_fd = -1;
static int   S_restarts;
static long  S_win_start;
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
    if (!dir || !*dir) dir = VT_PANEL_DIR_DEFAULT;
    snprintf(S_dir, sizeof S_dir, "%s", dir);
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
    if (S_pid <= 0) { if (stall_ticks > 300) vt_panel_restart(); return; }
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
