/* probe_native.c —— 架构可行性探针（父 = "核心侧"）。
 *
 * 验四件事，每件都能一眼判定（不碰触摸注入、不动核心、不抢 EVIOCGRAB）：
 *   ① memfd + fork/exec app_process：子进程能不能继承那块内存（不需要任何路径/权限）
 *   ② 双向共享：父写 core_*，子写 ui_*，两侧互相看得到
 *   ③ FD_CLOEXEC 卫生：带 CLOEXEC 的 fd（这里用 /dev/input/event8 只读打开，**不 grab**）
 *      不允许出现在子进程 /proc/<pid>/fd —— 这是"UI 继承带 grab 的 fd 导致触摸回不来"的防线
 *   ④ 崩溃隔离 + 心跳：kill -9 子进程父进程继续跑；父进程退出子进程靠 core_hb 停滞自杀
 *
 * 用法: probe_native <设备目录> [--try-ro-write] [--pump 秒数]
 *   设备目录需含 classes.dex 与 libprobe.so（与面板产物同构）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "probe_shm.h"

#ifndef __NR_memfd_create
#define __NR_memfd_create 279
#endif

extern char **environ;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/data/local/tmp/vtouch-probe";
    int try_ro = 0;
    int pump_s = 0;
    int i;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--try-ro-write")) try_ro = 1;
        else if (!strcmp(argv[i], "--pump")) pump_s = atoi(argv[++i]);
    }

    /* ③ 卫生测试用 fd：只读打开触摸屏，**不 EVIOCGRAB**（纯看继承行为，零副作用） */
    int devfd = open("/dev/input/event8", O_RDONLY | O_CLOEXEC);
    fprintf(stderr, "P: devfd=%d (%s) cloexec=%d\n", devfd, devfd < 0 ? strerror(errno) : "ok",
            devfd >= 0 ? !!(fcntl(devfd, F_GETFD) & FD_CLOEXEC) : -1);

    int fd = (int)syscall(__NR_memfd_create, "vtprobe", 0);
    if (fd < 0) { fprintf(stderr, "P: memfd_create 失败: %s\n", strerror(errno)); return 1; }
    if (ftruncate(fd, PROBE_SIZE) != 0) { fprintf(stderr, "P: ftruncate 失败: %s\n", strerror(errno)); return 1; }
    void *base = mmap(NULL, PROBE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { fprintf(stderr, "P: mmap 失败: %s\n", strerror(errno)); return 1; }
    struct probe_shm *sh = (struct probe_shm *)base;
    memset(sh, 0, sizeof *sh);
    sh->magic = PROBE_MAGIC;
    sh->ver = PROBE_VER;
    sh->size = PROBE_SIZE;
    for (i = 0; i < 64; i++) sh->ro_window[i] = (uint8_t)(0xA0 + i);
    sh->core_hb = 1;
    fprintf(stderr, "P: memfd=%d shm=%p magic=0x%x size=%u cloexec=%d\n",
            fd, (void *)sh, sh->magic, sh->size, !!(fcntl(fd, F_GETFD) & FD_CLOEXEC));

    /* ① fork/exec：argv/envp 必须在 fork 之前备好，fork 与 exec 之间只做 async-signal-safe 的事 */
    /* 环境必须在 fork 之前备好，并且**不能精简**：实测只给 CLASSPATH/ANDROID_ROOT/PATH 时，
     * 子进程连 main 都进不去、静默退 0（logcat 里没有 "Calling main entry"）。
     * 最终方案里核心 fork/exec 面板同理 —— 直接沿用父进程的完整 environ。 */
    char dexpath[512];
    snprintf(dexpath, sizeof dexpath, "%s/classes.dex", dir);
    setenv("CLASSPATH", dexpath, 1);
    char *cargv[8];
    int ci = 0;
    cargv[ci++] = (char *)"app_process";
    cargv[ci++] = (char *)"/system/bin";
    cargv[ci++] = (char *)"--nice-name=vtprobe";
    cargv[ci++] = (char *)"ProbeMain";
    cargv[ci++] = (char *)"3";
    if (try_ro) cargv[ci++] = (char *)"ro";
    cargv[ci] = NULL;

    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "P: fork 失败: %s\n", strerror(errno)); return 1; }
    if (pid == 0) {
        int fl;
        dup2(fd, 3);                                  /* 让子进程在固定 fd 号上拿到那块内存 */
        fl = fcntl(3, F_GETFD);
        if (fl >= 0) fcntl(3, F_SETFD, fl & ~FD_CLOEXEC);   /* 保留 3；其余 fd 全带 CLOEXEC */
        execve("/system/bin/app_process", cargv, environ);
        _exit(127);                                   /* exec 失败 */
    }
    fprintf(stderr, "P: forked child pid=%d (exec app_process ProbeMain 3 %s)\n", (int)pid, try_ro ? "ro" : "-");

    /* 父：每 100ms 写自己的心跳/计数，每秒打一条状态；子靠 core_hb 判断父是否还活着 */
    long t0 = now_ms();
    long last_report = 0;
    for (;;) {
        long el = now_ms() - t0;
        sh->core_hb++;
        sh->core_cnt++;
        usleep(100000);
        if (el - last_report >= 1000) {
            last_report = el;
            fprintf(stderr, "P: t=%ldms core_hb=%llu core_cnt=%llu | ui_pid=%d ui_hb=%llu ui_cnt=%llu | child_alive=%d\n",
                    el, (unsigned long long)sh->core_hb, (unsigned long long)sh->core_cnt,
                    (int)sh->ui_pid, (unsigned long long)sh->ui_hb, (unsigned long long)sh->ui_cnt,
                    (sh->ui_hb != 0 && (sh->core_hb - sh->ui_hb) < 50));
        }
        if (pump_s > 0 && el >= pump_s * 1000L) { fprintf(stderr, "P: pump 到点，正常退出（子进程应靠心跳停滞自杀）\n"); break; }
        if (sh->stop_req) { fprintf(stderr, "P: 收到 stop_req（子进程要求停）\n"); break; }
        { int st; if (waitpid(pid, &st, WNOHANG) == pid) fprintf(stderr, "P: 子进程已退出 status=0x%x（父继续跑）\n", st); }
    }
    if (devfd >= 0) close(devfd);
    return 0;
}
