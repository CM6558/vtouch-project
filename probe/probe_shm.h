/* probe_shm.h —— 探针共享区布局（父进程与子进程 JNI 两侧编同一份，验证"同一块内存双侧可见"）。
 * 布局与最终方案的思路一致：头部带 magic/ver/size 做版本校验，数据字段各自所有权明确。
 */
#ifndef PROBE_SHM_H
#define PROBE_SHM_H

#include <stdint.h>

#define PROBE_MAGIC 0x56545042u   /* "VTPB" */
#define PROBE_VER   1u
#define PROBE_SIZE  65536         /* 64KB：够放头部 + 只读窗口等 */

struct probe_shm {
    uint32_t magic;
    uint32_t ver;
    uint32_t size;
    uint32_t pad;

    volatile uint64_t core_hb;    /* 父（核心侧）每 100ms++ —— 子靠它判断核心是否还活着 */
    volatile uint64_t ui_hb;      /* 子（UI 侧）每 100ms++ —— 父靠它判断 UI 是否还在取数 */
    volatile uint64_t core_cnt;
    volatile uint64_t ui_cnt;
    volatile int32_t  ui_pid;
    volatile int32_t  stop_req;   /* 子 → 父：请求停（数据方向，不需要命令通道） */

    volatile uint8_t  ro_window[64];  /* 只读窗口：子进程按 RO 映射，写它必须 SIGSEGV */
};

#endif
