/* vt_vm.h —— VTBC v1 虚拟机（T3.1 骨架）。
 * 格式：docs/VTBC_V1.md；语义：docs/VT_SCRIPT_V1_SPEC.md。
 * 纯 C、零 Android 依赖（宿主单测：build/test_vt_vm.c，跑 ~/vtouch-vm-test.sh）。
 * 让出式 tick：vt_vm_tick() 跑到「需等待 / 结束 / 错误」就交还；等待毫秒走出参
 * （WAIT 指令；T3.2/T3.5 起动作/视觉也经挂钩返回等待）。主循环集成在 T3.5。 */
#ifndef VT_VM_H
#define VT_VM_H

#include <stdint.h>
#include <stddef.h>

/* 容量上限（载入时校验；超限 → 载入失败） */
#define VT_VM_MAX_GLOBALS     512
#define VT_VM_MAX_CONSTS      512
#define VT_VM_MAX_STACK       256
#define VT_VM_MAX_LOCALS      1024   /* 全部帧合计 */
#define VT_VM_MAX_FRAMES      32
#define VT_VM_MAX_TABLES      64
#define VT_VM_MAX_TABLE_CELLS 4096
#define VT_VM_MAX_FNS         128
#define VT_VM_SHEAP_CAP       65536  /* 运行时字符串堆（拼接/转串） */
#define VT_VM_ERR_CAP         160

/* 操作码（= docs/VTBC_V1.md §3.1；与编辑器 VtOp.java 一致） */
enum {
    VT_OP_KN = 0x01, VT_OP_KS = 0x02, VT_OP_LV = 0x03, VT_OP_SV = 0x04,
    VT_OP_LT = 0x05, VT_OP_ST = 0x06,
    VT_OP_ADD = 0x10, VT_OP_SUB = 0x11, VT_OP_MUL = 0x12, VT_OP_DIV = 0x13,
    VT_OP_MOD = 0x14, VT_OP_NEG = 0x15, VT_OP_NOT = 0x16, VT_OP_CMP = 0x17,
    VT_OP_POP = 0x21,
    VT_OP_JMP = 0x30, VT_OP_JF = 0x31, VT_OP_JT = 0x32, VT_OP_CJMP = 0x33,
    VT_OP_CALL = 0x40, VT_OP_RET = 0x41, VT_OP_RETE = 0x42,
    VT_OP_TAP = 0x50, VT_OP_SWIPE = 0x51, VT_OP_WAIT = 0x52,
    VT_OP_DOWN = 0x53, VT_OP_UP = 0x54, VT_OP_TOAST = 0x55,
    VT_OP_HL = 0x56, VT_OP_BOX = 0x57,
    VT_OP_FINDIMG = 0x58, VT_OP_FINDCOLOR = 0x59, VT_OP_FINDDOT = 0x5A,
    VT_OP_INREGION = 0x5B, VT_OP_SWON = 0x5C, VT_OP_TERM = 0x5D,
    VT_OP_NOW = 0x60,
    VT_OP_LEN = 0x70, VT_OP_STRN = 0x71, VT_OP_NUM = 0x72, VT_OP_RAND = 0x73,
    VT_OP_FLOOR = 0x74, VT_OP_CEIL = 0x75, VT_OP_ROUND = 0x76, VT_OP_ABS = 0x77,
    VT_OP_MIN = 0x78, VT_OP_MAX = 0x79, VT_OP_SQRT = 0x7A, VT_OP_SIN = 0x7B,
    VT_OP_COS = 0x7C, VT_OP_ATAN2 = 0x7D
};

/* CMP 子码 */
enum { VT_C_EQ = 0, VT_C_NE = 1, VT_C_LT = 2, VT_C_GT = 3, VT_C_LE = 4, VT_C_GE = 5 };

typedef enum {
    VT_VM_DONE = 0,   /* 程序结束（term_mode：0 完成 / 1 中止） */
    VT_VM_WAIT = 1,   /* 需等待（出参 ms）后继续 tick */
    VT_VM_ERR  = 2    /* 中止：err / err_line */
} vt_vm_status;

/* 值：t 0 空 / 1 数 / 2 串（sp 指向 blob 常量或字符串堆） */
typedef struct {
    uint8_t     t;
    double      n;
    const char *sp;
    uint32_t    sl;
} vt_val;

typedef struct vt_vm vt_vm;

struct vt_vm {
    /* 载入的 blob（不拷贝；调用方保证生命周期） */
    const uint8_t *bc;
    uint32_t       bc_len;
    /* 节 */
    const uint8_t *code;    uint32_t code_len;
    vt_val         cpool[VT_VM_MAX_CONSTS]; uint32_t nconsts;
    uint32_t       slot_count;             /* 全局槽数（含预定义 7） */
    uint32_t       main_locals;
    uint32_t       n_tables;
    uint32_t       table_len[VT_VM_MAX_TABLES];
    uint32_t       n_fns;
    struct { uint32_t entry, locals; uint8_t nargs; } fns[VT_VM_MAX_FNS];
    const uint8_t *lines;   uint32_t n_lines;   /* 8 字节步长（off,line） */
    const uint8_t *names;   uint32_t n_names;   /* 9 字节步长（kind,index,nameConst） */

    /* 运行态 */
    vt_val  g[VT_VM_MAX_GLOBALS];
    vt_val  stack[VT_VM_MAX_STACK];   int32_t sp;
    vt_val  locals[VT_VM_MAX_LOCALS];
    vt_val  tab_store[VT_VM_MAX_TABLE_CELLS];
    vt_val *tables[VT_VM_MAX_TABLES];
    struct { uint32_t ret_pc, base, nlocals; } frames[VT_VM_MAX_FRAMES];
    int32_t  fdepth;
    uint32_t pc;
    uint32_t lbase, cur_nlocals;      /* 当前帧局部窗（绝对槽号 = slot_count + 帧内相对号） */
    uint32_t cjmp_count;
    double   now_ms;                  /* NOW 源；批模式每调一次 +now_incr（测试假时钟） */
    double   now_incr;
    char     sheap[VT_VM_SHEAP_CAP];  uint32_t sheap_used;
    char     err[VT_VM_ERR_CAP];
    uint32_t err_pc, err_line;
    uint8_t  term_mode;               /* 0 完成 / 1 中止 */
};

/* 载入 + 结构校验（失败返回 -1，vm->err 说明原因）。成功即做好跑前准备。 */
int  vt_vm_load(vt_vm *vm, const uint8_t *bc, uint32_t len);
/* 复位运行态（全局清空 / 表清零 / 帧清空 / pc=0 / 时钟=0）。 */
void vt_vm_reset(vt_vm *vm);
/* 执行一段，直到 等待/结束/错误。WAIT 时 *wait_ms 为需等毫秒。 */
vt_vm_status vt_vm_tick(vt_vm *vm, uint32_t *wait_ms);
/* 批模式：WAIT 不等直接续跑（测试/编译期望值用）。 */
vt_vm_status vt_vm_run(vt_vm *vm);
/* 按名字表查全局槽（测试/调试）。1 = 找到。 */
int  vt_vm_global(const vt_vm *vm, const char *name, vt_val *out);
/* pc → 行号（取 ≤pc 的最后一条）。 */
uint32_t vt_vm_line(const vt_vm *vm, uint32_t pc);
/* 串值取指针（非串返回 NULL）。 */
const char *vt_val_str(const vt_val *v, uint32_t *len);

#endif /* VT_VM_H */
