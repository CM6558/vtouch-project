/* probe_jni.c —— 探针子进程（"UI 侧"）的 native 部分：映射父进程的 memfd、双向读写、验只读保护。
 * 子进程由父进程 exec app_process 拉起，那块 memfd 在 fd 3 上继承过来（无路径、无权限问题）。
 */
#define _GNU_SOURCE
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "probe_shm.h"

static struct probe_shm *sh_rw;      /* 与核心同权限的映射：探针要写自己的字段 */
static volatile uint8_t *ro_win;     /* 只读映射：写它必须 SIGSEGV */

static void cmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "C: ");
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

JNIEXPORT jint JNICALL Java_ProbeMain_nativeAttach(JNIEnv *env, jclass cls, jint fd)
{
    void *p;
    (void)env; (void)cls;
    p = mmap(NULL, PROBE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { cmsg("mmap RW 失败 fd=%d", (int)fd); return -1; }
    sh_rw = (struct probe_shm *)p;
    if (sh_rw->magic != PROBE_MAGIC) { cmsg("magic 不符: 0x%x（不是同一块内存？）", sh_rw->magic); return -2; }
    if (sh_rw->ver != PROBE_VER || sh_rw->size != PROBE_SIZE) { cmsg("版本/尺寸不符 ver=%u size=%u", sh_rw->ver, sh_rw->size); return -3; }
    /* 只读别名：验证"UI 写核心状态区会被 MMU 挡住" */
    p = mmap(NULL, PROBE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { cmsg("mmap RO 失败"); return -4; }
    ro_win = (volatile uint8_t *)p;
    sh_rw->ui_pid = (int32_t)getpid();
    cmsg("attach ok: magic=0x%x size=%u mypid=%d ro_window[0..3]=%02x %02x %02x %02x",
         sh_rw->magic, sh_rw->size, (int)getpid(), ro_win[0], ro_win[1], ro_win[2], ro_win[3]);
    return 0;
}

/* 每 100ms 调一次：写自己的心跳/计数，返回核心心跳（Java 侧据此判断核心是否还活着） */
JNIEXPORT jlong JNICALL Java_ProbeMain_nativeTick(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (!sh_rw) return -1;
    sh_rw->ui_hb++;
    sh_rw->ui_cnt++;
    return (jlong)sh_rw->core_hb;
}

JNIEXPORT jlong JNICALL Java_ProbeMain_nativeCoreCnt(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return sh_rw ? (jlong)sh_rw->core_cnt : -1;
}

/* 数据方向：UI 想停引擎就是写一个字段（不需要命令通道） */
JNIEXPORT void JNICALL Java_ProbeMain_nativeStopReq(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (sh_rw) sh_rw->stop_req = 1;
    cmsg("已写 stop_req=1（数据方式请求核心停）");
}

/* 走只读映射写一个字节 —— 期望是 SIGSEGV（证明"以核心为准"是硬件保证，不是靠自觉） */
JNIEXPORT void JNICALL Java_ProbeMain_nativeTryRoWrite(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    cmsg("准备写只读窗口 %p …", (const void *)ro_win);
    ro_win[0] = 0xFF;
    cmsg("！！没崩：只读保护失效");
}
