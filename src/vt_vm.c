/* vt_vm.c —— VTBC v1 虚拟机（T3.1 骨架 + T3.2 宿主钩子）。见 vt_vm.h 与 docs/VTBC_V1.md。
 * 语义对齐：除零 → 错；atan2(a,b) 返回度、(-180,180]（-180 规范为 180）、(0,0)=0；
 * sin/cos 按度；数字转串 ≤6 位小数去尾零（与编辑器编译/迷你解释器同口径）。
 * 动作/展示/视觉经宿主钩子落地（核心侧 = vt_ops.c 抽取的现执行器函数；未挂宿主 = 占位：
 * 按栈效应消费、视觉推 0）。多拍动作（点按/滑动）由 VM 排序、跨 tick 续跑（见 vm_act_step）。 */
#include "vt_vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VT_EXPR_PI 3.14159265358979323846

/* ---------- 小端读 ---------- */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static double rd64(const uint8_t *p) {
    uint64_t v = 0;
    int i;
    double d;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    memcpy(&d, &v, 8);
    return d;
}

/* ---------- 值 ---------- */
static vt_val vnum(double d) { vt_val v; v.t = 1; v.n = d; v.sp = NULL; v.sl = 0; return v; }
static vt_val vempty(void) { vt_val v; v.t = 0; v.n = 0; v.sp = NULL; v.sl = 0; return v; }
static vt_val vstr(const char *p, uint32_t l) { vt_val v; v.t = 2; v.n = 0; v.sp = p; v.sl = l; return v; }

const char *vt_val_str(const vt_val *v, uint32_t *len) {
    if (v->t != 2) { if (len) *len = 0; return NULL; }
    if (len) *len = v->sl;
    return v->sp;
}

/* 数字 → 串（≤6 位小数、去尾零） */
static void fmt_num(double d, char *buf, size_t cap) {
    size_t n;
    if (d == rint(d) && fabs(d) < 1e15) {
        snprintf(buf, cap, "%lld", (long long)d);
        return;
    }
    snprintf(buf, cap, "%.6f", d);
    n = strlen(buf);
    while (n > 0 && buf[n - 1] == '0') buf[--n] = '\0';
    if (n > 0 && buf[n - 1] == '.') buf[--n] = '\0';
}

/* ---------- 行号 / 失败 ---------- */
uint32_t vt_vm_line(const vt_vm *vm, uint32_t pc) {
    uint32_t i, line = 0;
    for (i = 0; i < vm->n_lines; i++) {
        uint32_t off = rd32(vm->lines + (size_t)i * 8);
        if (off > pc) break;
        line = rd32(vm->lines + (size_t)i * 8 + 4);
    }
    return line;
}

static void vt_vm_fail(vt_vm *vm, const char *msg, uint32_t pc) {
    snprintf(vm->err, sizeof vm->err, "%s", msg);
    vm->err_pc = pc;
    vm->err_line = vt_vm_line(vm, pc);
}

#define VMFAIL(m) do { vt_vm_fail(vm, (m), vm->pc); return VT_VM_ERR; } while (0)

/* ---------- 指令长度（载入期可解码性走查） ---------- */
static uint32_t vm_op_len(uint8_t op) {
    switch (op) {
        case VT_OP_KN: case VT_OP_KS: case VT_OP_LV: case VT_OP_SV:
        case VT_OP_LT: case VT_OP_ST:
        case VT_OP_JMP: case VT_OP_JF: case VT_OP_JT: case VT_OP_CJMP:
        case VT_OP_HL: case VT_OP_SWON: case VT_OP_INREGION: case VT_OP_FINDDOT:
            return 5;
        case VT_OP_FINDIMG: case VT_OP_FINDCOLOR:
            return 9;
        case VT_OP_CALL:
            return 6;
        case VT_OP_CMP: case VT_OP_TERM:
            return 2;
        case VT_OP_ADD: case VT_OP_SUB: case VT_OP_MUL: case VT_OP_DIV:
        case VT_OP_MOD: case VT_OP_NEG: case VT_OP_NOT: case VT_OP_POP:
        case VT_OP_RET: case VT_OP_RETE:
        case VT_OP_TAP: case VT_OP_SWIPE: case VT_OP_WAIT: case VT_OP_DOWN: case VT_OP_UP:
        case VT_OP_TOAST: case VT_OP_BOX: case VT_OP_NOW:
        case VT_OP_LEN: case VT_OP_STRN: case VT_OP_NUM: case VT_OP_RAND:
        case VT_OP_FLOOR: case VT_OP_CEIL: case VT_OP_ROUND: case VT_OP_ABS:
        case VT_OP_MIN: case VT_OP_MAX: case VT_OP_SQRT: case VT_OP_SIN:
        case VT_OP_COS: case VT_OP_ATAN2:
            return 1;
        default:
            return 0;
    }
}

/* ---------- 验证器（T3.4） ----------
 * 载入尾部调用：全量静态校验——指令流可解码到底 + 指令数 ≤ VT_VM_MAX_INSTS + 代码 ≤ 32KB；
 * 跳转/函数入口落指令边界（目标 == code_len 视为终止点，合法）；栈深数据流（种子 = 主程序
 * pc0 与全部函数入口，深度 0；逐指令推进、合并点深度必须一致；下溢 / 超限拒绝）；引用界
 * （常量种类/表/函数索引/实参数/槽绝对址——全局 < slotCount、局部按所属帧）；RET/RETE 仅允许
 * 在函数体内。任一不过 → err = `程序非法: <由>`。 */

static int ver_fail(vt_vm *vm, const char *why) {
    snprintf(vm->err, sizeof vm->err, "程序非法: %s", why);
    return -1;
}

/* pc 所属帧的局部槽数（主程序 / 入口递增划分出的函数体）。 */
static uint32_t ver_frame_locals(const vt_vm *vm, uint32_t pc) {
    uint32_t k;
    if (vm->n_fns == 0 || pc < vm->fns[0].entry) return vm->main_locals;
    for (k = vm->n_fns - 1; k > 0; k--) {
        if (pc >= vm->fns[k].entry) return vm->fns[k].locals;
    }
    return vm->fns[0].locals;
}

static int ver_in_fn(const vt_vm *vm, uint32_t pc) {
    return vm->n_fns > 0 && pc >= vm->fns[0].entry;
}

/* （pop, push）栈效应；0 = 已填。CALL/RET/RETE/TERM 由调用点特判。 */
static int vm_op_stack(uint8_t op, int *pops, int *pushes) {
    *pops = 0;
    *pushes = 0;
    switch (op) {
        case VT_OP_KN: case VT_OP_KS: case VT_OP_LV: case VT_OP_NOW:
            *pushes = 1;
            return 0;
        case VT_OP_SV: case VT_OP_POP: case VT_OP_WAIT: case VT_OP_JF: case VT_OP_JT: case VT_OP_HL:
            *pops = 1;
            return 0;
        case VT_OP_LT: case VT_OP_LEN: case VT_OP_STRN: case VT_OP_NUM:
        case VT_OP_FLOOR: case VT_OP_CEIL: case VT_OP_ROUND: case VT_OP_ABS:
        case VT_OP_SQRT: case VT_OP_SIN: case VT_OP_COS: case VT_OP_NEG: case VT_OP_NOT:
            *pops = 1;
            *pushes = 1;
            return 0;
        case VT_OP_ST: case VT_OP_DOWN: case VT_OP_TOAST:
            *pops = 2;
            return 0;
        case VT_OP_ADD: case VT_OP_SUB: case VT_OP_MUL: case VT_OP_DIV: case VT_OP_MOD:
        case VT_OP_CMP: case VT_OP_MIN: case VT_OP_MAX: case VT_OP_ATAN2: case VT_OP_RAND:
        case VT_OP_FINDDOT: case VT_OP_INREGION:
            *pops = 2;
            *pushes = 1;
            return 0;
        case VT_OP_TAP:
            *pops = 3;
            return 0;
        case VT_OP_FINDIMG:
            *pops = 3;
            *pushes = 1;
            return 0;
        case VT_OP_SWIPE:
            *pops = 7;
            return 0;
        case VT_OP_BOX:
            *pops = 5;
            return 0;
        case VT_OP_FINDCOLOR: case VT_OP_SWON:
            *pushes = 1;
            return 0;
        case VT_OP_JMP: case VT_OP_CJMP: case VT_OP_UP:
            return 0;
        case VT_OP_CALL: case VT_OP_RET: case VT_OP_RETE: case VT_OP_TERM:
            return 0;
        default:
            return -1;
    }
}

static int vm_verify(vt_vm *vm) {
    uint32_t i, k, s, n_insts = 0, wtop = 0, wcap;
    uint8_t *bound = NULL;
    int16_t *dep = NULL;
    uint32_t *work = NULL;
    int rc = -1;
    size_t dcap = (vm->code_len ? vm->code_len : 1);

    if (vm->code_len > 32768u) return ver_fail(vm, "代码超长");

    bound = calloc((size_t)(vm->code_len + 7) / 8 + 1, 1);
    dep = malloc(sizeof(int16_t) * dcap);
    if (!bound || !dep) { ver_fail(vm, "内存"); goto out; }
    memset(dep, 0xFF, sizeof(int16_t) * dcap);

    /* 走查：解码 + 计数 + 边界集 */
    for (i = 0; i < vm->code_len; ) {
        uint32_t l = vm_op_len(vm->code[i]);
        if (l == 0) { ver_fail(vm, "未知指令"); goto out; }
        if (l > vm->code_len - i) { ver_fail(vm, "指令截断"); goto out; }
        if (n_insts >= 8192u) { ver_fail(vm, "指令数超限"); goto out; }
        n_insts++;
        bound[i >> 3] |= (uint8_t)(1u << (i & 7));
        i += l;
    }

    /* 函数入口：递增 + 落边界（entry == code_len 视为终止点，合法） */
    for (k = 0; k < vm->n_fns; k++) {
        uint32_t e = vm->fns[k].entry;
        if (k > 0 && e < vm->fns[k - 1].entry) { ver_fail(vm, "函数入口乱序"); goto out; }
        if (e < vm->code_len && !(bound[e >> 3] & (1u << (e & 7)))) {
            ver_fail(vm, "函数入口非指令边界");
            goto out;
        }
    }

    wcap = 2u * n_insts + 8u;
    work = malloc(sizeof(uint32_t) * wcap);
    if (!work) { ver_fail(vm, "内存"); goto out; }

    /* 种子：主程序 pc0 + 全部函数入口（深度 0；入口若已被主流程访问，深度须同为 0） */
    if (vm->code_len > 0) {
        dep[0] = 0;
        work[wtop++] = 0;
    }
    for (k = 0; k < vm->n_fns; k++) {
        uint32_t e = vm->fns[k].entry;
        if (e >= vm->code_len) continue;
        if (dep[e] < 0) {
            dep[e] = 0;
            work[wtop++] = e;
        } else if (dep[e] != 0) {
            ver_fail(vm, "栈深不一致");
            goto out;
        }
    }

    /* 数据流推进 */
    while (wtop > 0) {
        uint32_t pc = work[--wtop];
        uint8_t op = vm->code[pc];
        uint32_t l = vm_op_len(op);
        int d = (int)dep[pc];
        int pops = 0, pushes = 0, nd;
        uint32_t off = pc + 1;

        /* 操作数 / 引用界（仅可达指令） */
        switch (op) {
            case VT_OP_KN: {
                uint32_t ci = rd32(vm->code + off);
                if (ci >= vm->nconsts || vm->cpool[ci].t != 1) { ver_fail(vm, "常量引用非法"); goto out; }
                break;
            }
            case VT_OP_KS: {
                uint32_t ci = rd32(vm->code + off);
                if (ci >= vm->nconsts || vm->cpool[ci].t != 2) { ver_fail(vm, "常量引用非法"); goto out; }
                break;
            }
            case VT_OP_LV:
            case VT_OP_SV: {
                uint32_t a = rd32(vm->code + off);
                if (a >= vm->slot_count && a - vm->slot_count >= ver_frame_locals(vm, pc)) {
                    ver_fail(vm, "槽引用越界");
                    goto out;
                }
                break;
            }
            case VT_OP_LT:
            case VT_OP_ST: {
                uint32_t ti = rd32(vm->code + off);
                if (ti >= vm->n_tables) { ver_fail(vm, "表引用越界"); goto out; }
                break;
            }
            case VT_OP_JMP:
            case VT_OP_JF:
            case VT_OP_JT:
            case VT_OP_CJMP: {
                uint32_t t = rd32(vm->code + off);
                if (t > vm->code_len) { ver_fail(vm, "跳转越界"); goto out; }
                break;
            }
            case VT_OP_CMP:
                if (vm->code[off] > VT_C_GE) { ver_fail(vm, "比较子码非法"); goto out; }
                break;
            case VT_OP_TERM:
                if (vm->code[off] > 1) { ver_fail(vm, "收工模式非法"); goto out; }
                break;
            case VT_OP_HL: {
                uint32_t ci = rd32(vm->code + off);
                if (ci >= vm->nconsts || vm->cpool[ci].t != 2) { ver_fail(vm, "区域常量非法"); goto out; }
                break;
            }
            case VT_OP_FINDIMG: {
                uint32_t ci = rd32(vm->code + off);
                uint32_t ri = rd32(vm->code + off + 4);
                if (ci >= vm->nconsts || vm->cpool[ci].t != 2) { ver_fail(vm, "模板常量非法"); goto out; }
                if (ri != 0xFFFFFFFFu && (ri >= vm->nconsts || vm->cpool[ri].t != 2)) {
                    ver_fail(vm, "区域常量非法");
                    goto out;
                }
                break;
            }
            case VT_OP_FINDCOLOR: {
                uint32_t ci = rd32(vm->code + off);
                uint32_t ri = rd32(vm->code + off + 4);
                if (ci >= vm->nconsts || vm->cpool[ci].t != 2) { ver_fail(vm, "点集常量非法"); goto out; }
                if (ri != 0xFFFFFFFFu && (ri >= vm->nconsts || vm->cpool[ri].t != 2)) {
                    ver_fail(vm, "区域常量非法");
                    goto out;
                }
                break;
            }
            case VT_OP_FINDDOT: {
                uint32_t ri = rd32(vm->code + off);
                if (ri != 0xFFFFFFFFu && (ri >= vm->nconsts || vm->cpool[ri].t != 2)) {
                    ver_fail(vm, "区域常量非法");
                    goto out;
                }
                break;
            }
            case VT_OP_INREGION: {
                uint32_t ri = rd32(vm->code + off);
                if (ri >= vm->nconsts || vm->cpool[ri].t != 2) { ver_fail(vm, "区域常量非法"); goto out; }
                break;
            }
            case VT_OP_SWON: {
                uint32_t ci = rd32(vm->code + off);
                if (ci >= vm->nconsts || vm->cpool[ci].t != 2) { ver_fail(vm, "开关名常量非法"); goto out; }
                break;
            }
            case VT_OP_CALL: {
                uint32_t fi = rd32(vm->code + off);
                if (fi >= vm->n_fns) { ver_fail(vm, "函数引用越界"); goto out; }
                if (vm->code[off + 4] != vm->fns[fi].nargs) { ver_fail(vm, "实参数不符"); goto out; }
                break;
            }
            case VT_OP_RET:
            case VT_OP_RETE:
                if (!ver_in_fn(vm, pc)) { ver_fail(vm, "返回不在函数内"); goto out; }
                break;
            default:
                break;
        }

        /* 栈效应 */
        if (op == VT_OP_CALL) {
            uint32_t fi = rd32(vm->code + off);
            pops = (int)vm->fns[fi].nargs;
            pushes = 1;
        } else if (op == VT_OP_RET) {
            pops = 1;
        } else if (op == VT_OP_RETE || op == VT_OP_TERM) {
            pops = 0;
        } else if (vm_op_stack(op, &pops, &pushes) != 0) {
            ver_fail(vm, "未知指令");
            goto out;
        }
        if (d < pops) { ver_fail(vm, "栈下溢"); goto out; }
        nd = d - pops + pushes;
        if (nd > VT_VM_MAX_STACK) { ver_fail(vm, "栈深超限"); goto out; }

        /* 终止 / 后继推进（跳转目标与落尾都须是边界；t == code_len = 终止合法） */
        if (op == VT_OP_RET || op == VT_OP_RETE || op == VT_OP_TERM) continue;
        {
            uint32_t succ[2];
            int nsucc = 0;
            if (op == VT_OP_JMP || op == VT_OP_CJMP) {
                succ[nsucc++] = rd32(vm->code + off);
            } else if (op == VT_OP_JF || op == VT_OP_JT) {
                succ[nsucc++] = rd32(vm->code + off);
                succ[nsucc++] = pc + l;
            } else {
                succ[nsucc++] = pc + l;
            }
            for (s = 0; s < (uint32_t)nsucc; s++) {
                uint32_t t = succ[s];
                if (t == vm->code_len) continue;
                if (t > vm->code_len) { ver_fail(vm, "跳转越界"); goto out; }
                if (!(bound[t >> 3] & (1u << (t & 7)))) { ver_fail(vm, "跳转目标非指令边界"); goto out; }
                if (dep[t] < 0) {
                    if (wtop >= wcap) { ver_fail(vm, "验证溢出"); goto out; }
                    dep[t] = (int16_t)nd;
                    work[wtop++] = t;
                } else if ((int)dep[t] != nd) {
                    ver_fail(vm, "栈深不一致");
                    goto out;
                }
            }
        }
    }
    rc = 0;
out:
    free(bound);
    free(dep);
    free(work);
    return rc;
}

/* ---------- 载入 ---------- */
static int load_fail(vt_vm *vm, const char *m) {
    snprintf(vm->err, sizeof vm->err, "VTBC: %s", m);
    return -1;
}

int vt_vm_load(vt_vm *vm, const uint8_t *bc, uint32_t len) {
    uint32_t p, i, cells;
    memset(vm, 0, sizeof *vm);
    vm->bc = bc;
    vm->bc_len = len;
    if (!bc || len < 16) return load_fail(vm, "过短");
    if (memcmp(bc, "VTBC", 4) != 0) return load_fail(vm, "魔数不符");
    if (rd16(bc + 4) != 1) return load_fail(vm, "版本不符");
    if (rd16(bc + 6) != 0) return load_fail(vm, "flags 非 0");
    if (rd32(bc + 12) != len) return load_fail(vm, "长度不符");

    p = 16;
    if (p + 4 > len) return load_fail(vm, "代码节越界");
    vm->code_len = rd32(bc + p); p += 4;
    if (vm->code_len > len - p) return load_fail(vm, "代码节越界");
    vm->code = bc + p; p += vm->code_len;

    /* （指令流/语义全量走查在尾部的 vt_vm_verify：解码、边界、栈深、引用界） */

    if (p + 4 > len) return load_fail(vm, "常量节越界");
    vm->nconsts = rd32(bc + p); p += 4;
    if (vm->nconsts > VT_VM_MAX_CONSTS) return load_fail(vm, "常量过多");
    for (i = 0; i < vm->nconsts; i++) {
        uint8_t kind;
        if (p + 1 > len) return load_fail(vm, "常量节越界");
        kind = bc[p++];
        if (kind == 0) {
            if (p + 8 > len) return load_fail(vm, "常量节越界");
            vm->cpool[i] = vnum(rd64(bc + p)); p += 8;
        } else if (kind == 1) {
            uint32_t l;
            if (p + 4 > len) return load_fail(vm, "常量节越界");
            l = rd32(bc + p); p += 4;
            if (l > len - p) return load_fail(vm, "常量串越界");
            vm->cpool[i] = vstr((const char *)(bc + p), l); p += l;
        } else {
            return load_fail(vm, "常量 kind 非法");
        }
    }

    if (p + 8 > len) return load_fail(vm, "槽节越界");
    vm->slot_count = rd32(bc + p); p += 4;
    if (vm->slot_count < 7 || vm->slot_count > VT_VM_MAX_GLOBALS) return load_fail(vm, "槽数非法");
    vm->main_locals = rd32(bc + p); p += 4;
    if (vm->main_locals > VT_VM_MAX_LOCALS) return load_fail(vm, "主局部过多");

    if (p + 4 > len) return load_fail(vm, "表节越界");
    vm->n_tables = rd32(bc + p); p += 4;
    if (vm->n_tables > VT_VM_MAX_TABLES) return load_fail(vm, "表过多");
    cells = 0;
    for (i = 0; i < vm->n_tables; i++) {
        uint32_t l;
        if (p + 4 > len) return load_fail(vm, "表节越界");
        l = rd32(bc + p); p += 4;
        if (l < 1 || l > 4096) return load_fail(vm, "表长度非法");
        cells += l;
        if (cells > VT_VM_MAX_TABLE_CELLS) return load_fail(vm, "表超限");
        vm->table_len[i] = l;
    }

    if (p + 4 > len) return load_fail(vm, "函数节越界");
    vm->n_fns = rd32(bc + p); p += 4;
    if (vm->n_fns > VT_VM_MAX_FNS) return load_fail(vm, "函数过多");
    for (i = 0; i < vm->n_fns; i++) {
        if (p + 9 > len) return load_fail(vm, "函数节越界");
        vm->fns[i].entry = rd32(bc + p); p += 4;
        vm->fns[i].locals = rd32(bc + p); p += 4;
        vm->fns[i].nargs = bc[p++];
        if (vm->fns[i].entry > vm->code_len) return load_fail(vm, "函数入口越界");
        if (vm->fns[i].locals < vm->fns[i].nargs || vm->fns[i].locals > VT_VM_MAX_LOCALS)
            return load_fail(vm, "函数局部非法");
    }

    if (p + 4 > len) return load_fail(vm, "行号节越界");
    vm->n_lines = rd32(bc + p); p += 4;
    if ((uint64_t)vm->n_lines * 8 > (uint64_t)(len - p)) return load_fail(vm, "行号节越界");
    vm->lines = bc + p; p += vm->n_lines * 8;
    for (i = 0; i < vm->n_lines; i++) {
        if (rd32(vm->lines + (size_t)i * 8) > vm->code_len) return load_fail(vm, "行号偏移越界");
    }

    if (p + 4 > len) return load_fail(vm, "名字节越界");
    vm->n_names = rd32(bc + p); p += 4;
    if ((uint64_t)vm->n_names * 9 > (uint64_t)(len - p)) return load_fail(vm, "名字节越界");
    vm->names = bc + p; p += vm->n_names * 9;
    for (i = 0; i < vm->n_names; i++) {
        const uint8_t *e = vm->names + (size_t)i * 9;
        uint32_t nc = rd32(e + 5);
        if (e[0] > 2 || nc >= vm->nconsts || vm->cpool[nc].t != 2)
            return load_fail(vm, "名字项非法");
    }

    if (p != len) return load_fail(vm, "尾部多余字节");
    if (vm_verify(vm) != 0) return -1;   /* err 已置（程序非法: <由>） */
    vt_vm_reset(vm);
    return 0;
}

/* ---------- 复位 ---------- */
void vt_vm_reset(vt_vm *vm) {
    uint32_t i, off = 0;
    memset(vm->g, 0, sizeof vm->g);
    for (i = 0; i < vm->n_tables; i++) {
        uint32_t j, l = vm->table_len[i];
        vm->tables[i] = vm->tab_store + off;
        for (j = 0; j < l; j++) vm->tab_store[off + j] = vnum(0);
        off += l;
    }
    vm->sp = 0;
    vm->fdepth = 0;
    vm->pc = 0;
    vm->lbase = 0;
    vm->cur_nlocals = vm->main_locals;
    vm->cjmp_count = 0;
    vm->now_ms = 0;
    vm->now_incr = 1;   /* 批模式假时钟；T3.5 接真实时钟后置 0 */
    vm->sheap_used = 0;
    vm->err[0] = '\0';
    vm->err_pc = 0;
    vm->err_line = 0;
    vm->term_mode = 0;
    vm->act_op = 0;
    vm->act_phase = 0;
    vm->act_k = 0;
    vm->act_n = 0;
    vm->act_pc = 0;
}

/* ---------- 栈 / 槽 ---------- */
static int push(vt_vm *vm, vt_val v) {
    if (vm->sp >= VT_VM_MAX_STACK) return -1;
    vm->stack[vm->sp++] = v;
    return 0;
}
static vt_val popv(vt_vm *vm) {
    if (vm->sp <= 0) return vempty();
    return vm->stack[--vm->sp];
}
static vt_val *slot_ref(vt_vm *vm, uint32_t a) {
    if (a < vm->slot_count) return &vm->g[a];
    a -= vm->slot_count;
    if (a >= vm->cur_nlocals || vm->lbase + a >= VT_VM_MAX_LOCALS) return NULL;
    return &vm->locals[vm->lbase + a];
}

/* ---------- 值运算 ---------- */
static int num_of(vt_vm *vm, vt_val v, double *out) {
    if (v.t == 1) { *out = v.n; return 0; }
    vt_vm_fail(vm, v.t == 2 ? "类型错" : "结果无值", vm->pc);
    return -1;
}
static int truth_of(vt_vm *vm, vt_val v) {
    double d;
    if (num_of(vm, v, &d) < 0) return -1;
    return d != 0;
}
static int str_of(vt_vm *vm, vt_val v, char *tmp, size_t cap, const char **p, uint32_t *len) {
    if (v.t == 2) { *p = v.sp; *len = v.sl; return 0; }
    if (v.t == 1) {
        fmt_num(v.n, tmp, cap);
        *p = tmp; *len = (uint32_t)strlen(tmp);
        return 0;
    }
    vt_vm_fail(vm, "结果无值", vm->pc);
    return -1;
}

static int vm_add(vt_vm *vm, vt_val a, vt_val b, vt_val *out) {
    if (a.t == 2 || b.t == 2) {
        char ta[64], tb[64], *dst;
        const char *pa, *pb;
        uint32_t la, lb;
        if (str_of(vm, a, ta, sizeof ta, &pa, &la) < 0) return -1;
        if (str_of(vm, b, tb, sizeof tb, &pb, &lb) < 0) return -1;
        if ((uint64_t)vm->sheap_used + la + lb > VT_VM_SHEAP_CAP) {
            vt_vm_fail(vm, "字符串池满", vm->pc);
            return -1;
        }
        dst = vm->sheap + vm->sheap_used;
        memcpy(dst, pa, la);
        memcpy(dst + la, pb, lb);
        vm->sheap_used += la + lb;
        *out = vstr(dst, la + lb);
        return 0;
    }
    {
        double x, y;
        if (num_of(vm, a, &x) < 0 || num_of(vm, b, &y) < 0) return -1;
        *out = vnum(x + y);
        return 0;
    }
}

static int vm_cmp(vt_vm *vm, uint8_t sub, vt_val a, vt_val b, vt_val *out) {
    if (sub == VT_C_EQ || sub == VT_C_NE) {
        int eq;
        if (a.t == 2 || b.t == 2) {
            char ta[64], tb[64];
            const char *pa, *pb;
            uint32_t la, lb;
            if (str_of(vm, a, ta, sizeof ta, &pa, &la) < 0) return -1;
            if (str_of(vm, b, tb, sizeof tb, &pb, &lb) < 0) return -1;
            eq = (la == lb) && (memcmp(pa, pb, la) == 0);
        } else {
            double x, y;
            if (num_of(vm, a, &x) < 0 || num_of(vm, b, &y) < 0) return -1;
            eq = (x == y);
        }
        *out = vnum((sub == VT_C_EQ) ? (eq ? 1 : 0) : (eq ? 0 : 1));
        return 0;
    }
    {
        double x, y;
        if (num_of(vm, a, &x) < 0 || num_of(vm, b, &y) < 0) return -1;
        switch (sub) {
            case VT_C_LT: *out = vnum(x < y); break;
            case VT_C_GT: *out = vnum(x > y); break;
            case VT_C_LE: *out = vnum(x <= y); break;
            default:      *out = vnum(x >= y); break;
        }
        return 0;
    }
}

static int vm_atan2_deg(vt_vm *vm, double a, double b, double *out) {
    double d = atan2(a, b) / VT_EXPR_PI * 180.0;
    if (d == -180.0) d = 180.0;   /* ±180 同向（spec (-180,180]） */
    (void)vm;
    *out = d;
    return 0;
}

/* ---------- 宿主钩子小件（T3.2） ---------- */
/* 常量串取 C 串（拷贝进 buf 补 NUL）。ci == -1（无）：must=0 → *out=NULL；must=1 → 程序非法。 */
static int vm_cstr(vt_vm *vm, uint32_t ci, char *buf, size_t cap, int must, const char **out)
{
    const vt_val *c;
    if (ci == 0xFFFFFFFFu) {
        if (must) { vt_vm_fail(vm, "程序非法", vm->pc); return -1; }
        *out = NULL;
        return 0;
    }
    if (ci >= vm->nconsts || vm->cpool[ci].t != 2 || vm->cpool[ci].sl >= cap) {
        vt_vm_fail(vm, "程序非法", vm->pc);
        return -1;
    }
    c = &vm->cpool[ci];
    memcpy(buf, c->sp, c->sl);
    buf[c->sl] = '\0';
    *out = buf;
    return 0;
}

/* 弹一个数并按 int 取（超界 → 程序非法）。 */
static int vm_pop_int(vt_vm *vm, int *out)
{
    double d;
    if (num_of(vm, popv(vm), &d) < 0) return -1;
    if (!(d >= -2147483000.0 && d <= 2147483000.0)) { vt_vm_fail(vm, "程序非法", vm->pc); return -1; }
    *out = (int)d;
    return 0;
}

/* 线性插值（与 vt_ops.c op_lerp 同式）。 */
static int vm_lerp(int a, int b, uint32_t k, uint32_t n) { return a + (b - a) * (int)k / (int)n; }

/* 续跑挂起动作（点按/滑动）。返回 0 = 完成（调用方清 act 并继续）；1 = 已产出等待（*wait_ms）；2 = 已置错。 */
static int vm_act_step(vt_vm *vm, uint32_t *wait_ms)
{
    const struct vt_vm_host *h = vm->host;
    if (vm->act_op == VT_OP_TAP) {
        if (vm->act_phase == 1) {
            vm->act_phase = 2;
            *wait_ms = (vm->act_ms > 0) ? (uint32_t)vm->act_ms : 0u;   /* ms=0 也隔一拍（对齐老执行器 up 落下一拍） */
            return 1;
        }
        if (h->touch_up(h->ctx) != 0) { vt_vm_fail(vm, "动作中止", vm->act_pc); return 2; }
        return 0;
    }
    /* 滑动：状态机 1=头部停 → 2=排采样 → 3=移动 → 4=尾部停 → 5=抬指 */
    for (;;) {
        switch (vm->act_phase) {
            case 1:                    /* 头部停（停1） */
                vm->act_phase = 2;
                *wait_ms = (uint32_t)vm->act_h1;
                return 1;
            case 2: {                  /* 排下一采样：k·ms/N −(k−1)·ms/N */
                uint32_t k1, dt, dur = (uint32_t)vm->act_ms;
                if (vm->act_k >= vm->act_n) { vm->act_phase = 4; break; }
                k1 = vm->act_k + 1;
                dt = k1 * dur / vm->act_n - vm->act_k * dur / vm->act_n;
                vm->act_k = k1;
                vm->act_phase = 3;
                if (dt > 0) { *wait_ms = dt; return 1; }
                break;
            }
            case 3: {                  /* 移动第 k 点 */
                int lx = vm_lerp(vm->act_x1, vm->act_x2, vm->act_k, vm->act_n);
                int ly = vm_lerp(vm->act_y1, vm->act_y2, vm->act_k, vm->act_n);
                if (h->touch_move(h->ctx, lx, ly) != 0) { vt_vm_fail(vm, "动作中止", vm->act_pc); return 2; }
                vm->act_phase = 2;
                break;
            }
            case 4:                    /* 尾部停（停2） */
                vm->act_phase = 5;
                if (vm->act_h2 > 0) { *wait_ms = (uint32_t)vm->act_h2; return 1; }
                break;
            default:                   /* 5：抬指 */
                if (h->touch_up(h->ctx) != 0) { vt_vm_fail(vm, "动作中止", vm->act_pc); return 2; }
                return 0;
        }
    }
}

/* ---------- 执行 ---------- */
vt_vm_status vt_vm_tick(vt_vm *vm, uint32_t *wait_ms) {
    const struct vt_vm_host *host = vm->host;
    uint32_t pc = vm->pc;
    if (wait_ms) *wait_ms = 0;
    if (vm->act_op) {                        /* 挂起动作续跑（点按/滑动，跨 tick） */
        int r = vm_act_step(vm, wait_ms);
        if (r == 2) return VT_VM_ERR;
        if (r == 1) return VT_VM_WAIT;
        vm->act_op = 0;
        pc = vm->pc;
    }
    for (;;) {
        uint8_t op;
        if (pc >= vm->code_len) {
            vm->pc = pc;
            vm->term_mode = 0;
            return VT_VM_DONE;
        }
        op = vm->code[pc];
        vm->pc = pc;
        pc++;

        switch (op) {
            case VT_OP_KN: {
                uint32_t n = rd32(vm->code + pc);
                pc += 4;
                if (n >= vm->nconsts || vm->cpool[n].t != 1) VMFAIL("程序非法");
                if (push(vm, vm->cpool[n]) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_KS: {
                uint32_t n = rd32(vm->code + pc);
                pc += 4;
                if (n >= vm->nconsts || vm->cpool[n].t != 2) VMFAIL("程序非法");
                if (push(vm, vm->cpool[n]) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_LV: {
                uint32_t a = rd32(vm->code + pc);
                vt_val *s;
                pc += 4;
                s = slot_ref(vm, a);
                if (!s) VMFAIL("程序非法");
                if (s->t == 0) VMFAIL("变量无值");
                if (push(vm, *s) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_SV: {
                uint32_t a = rd32(vm->code + pc);
                vt_val v, *s;
                pc += 4;
                v = popv(vm);
                s = slot_ref(vm, a);
                if (!s) VMFAIL("程序非法");
                *s = v;
                break;
            }
            case VT_OP_LT: {
                uint32_t ti = rd32(vm->code + pc);
                double ix;
                pc += 4;
                if (ti >= vm->n_tables) VMFAIL("程序非法");
                if (num_of(vm, popv(vm), &ix) < 0) return VT_VM_ERR;
                if (ix < 0 || ix >= (double)vm->table_len[ti]) VMFAIL("下标越界");
                if (push(vm, vm->tables[ti][(uint32_t)ix]) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_ST: {
                uint32_t ti = rd32(vm->code + pc);
                double ix;
                vt_val v;
                pc += 4;
                if (ti >= vm->n_tables) VMFAIL("程序非法");
                v = popv(vm);
                if (num_of(vm, popv(vm), &ix) < 0) return VT_VM_ERR;
                if (ix < 0 || ix >= (double)vm->table_len[ti]) VMFAIL("下标越界");
                vm->tables[ti][(uint32_t)ix] = v;
                break;
            }
            case VT_OP_ADD: {
                vt_val b = popv(vm), a = popv(vm), r;
                if (vm_add(vm, a, b, &r) < 0) return VT_VM_ERR;
                if (push(vm, r) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_SUB:
            case VT_OP_MUL:
            case VT_OP_DIV:
            case VT_OP_MOD: {
                double x, y, r;
                vt_val b = popv(vm), a = popv(vm);
                if (num_of(vm, a, &x) < 0 || num_of(vm, b, &y) < 0) return VT_VM_ERR;
                if ((op == VT_OP_DIV || op == VT_OP_MOD) && y == 0.0) VMFAIL("除零");
                if (op == VT_OP_SUB) r = x - y;
                else if (op == VT_OP_MUL) r = x * y;
                else if (op == VT_OP_DIV) r = x / y;
                else r = fmod(x, y);
                if (push(vm, vnum(r)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_NEG: {
                double x;
                if (num_of(vm, popv(vm), &x) < 0) return VT_VM_ERR;
                if (push(vm, vnum(-x)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_NOT: {
                int b = truth_of(vm, popv(vm));
                if (b < 0) return VT_VM_ERR;
                if (push(vm, vnum(b ? 0 : 1)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_CMP: {
                uint8_t sub = vm->code[pc++];
                vt_val b, a, r;
                if (sub > VT_C_GE) VMFAIL("程序非法");
                b = popv(vm);
                a = popv(vm);
                if (vm_cmp(vm, sub, a, b, &r) < 0) return VT_VM_ERR;
                if (push(vm, r) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_POP:
                (void)popv(vm);
                break;
            case VT_OP_JMP:
                pc = rd32(vm->code + pc);
                break;
            case VT_OP_JF: {
                uint32_t t = rd32(vm->code + pc);
                int b;
                pc += 4;
                b = truth_of(vm, popv(vm));
                if (b < 0) return VT_VM_ERR;
                if (!b) pc = t;
                break;
            }
            case VT_OP_JT: {
                uint32_t t = rd32(vm->code + pc);
                int b;
                pc += 4;
                b = truth_of(vm, popv(vm));
                if (b < 0) return VT_VM_ERR;
                if (b) pc = t;
                break;
            }
            case VT_OP_CJMP: {
                uint32_t t = rd32(vm->code + pc);
                if (++vm->cjmp_count > 200) VMFAIL("跳转超限");
                pc = t;
                break;
            }
            case VT_OP_CALL: {
                uint32_t fi = rd32(vm->code + pc);
                uint8_t na = vm->code[pc + 4];
                uint32_t newbase, i;
                pc += 5;
                if (fi >= vm->n_fns) VMFAIL("程序非法");
                if (na != vm->fns[fi].nargs) VMFAIL("程序非法");
                if (vm->fdepth >= VT_VM_MAX_FRAMES) VMFAIL("调用超深");
                newbase = vm->lbase + vm->cur_nlocals;
                if (newbase + vm->fns[fi].locals > VT_VM_MAX_LOCALS) VMFAIL("调用超深");
                vm->frames[vm->fdepth].ret_pc = pc;
                vm->frames[vm->fdepth].base = vm->lbase;
                vm->frames[vm->fdepth].nlocals = vm->cur_nlocals;
                vm->fdepth++;
                for (i = 0; i < vm->fns[fi].locals; i++) vm->locals[newbase + i] = vempty();
                for (i = na; i > 0; i--) vm->locals[newbase + (i - 1)] = popv(vm);
                vm->lbase = newbase;
                vm->cur_nlocals = vm->fns[fi].locals;
                pc = vm->fns[fi].entry;
                break;
            }
            case VT_OP_RET:
            case VT_OP_RETE: {
                vt_val rv = (op == VT_OP_RET) ? popv(vm) : vempty();
                if (vm->fdepth <= 0) VMFAIL("程序非法");
                vm->fdepth--;
                vm->lbase = vm->frames[vm->fdepth].base;
                vm->cur_nlocals = vm->frames[vm->fdepth].nlocals;
                pc = vm->frames[vm->fdepth].ret_pc;
                if (push(vm, rv) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_TERM: {
                uint8_t m = vm->code[pc++];
                vm->term_mode = m;
                vm->pc = pc;
                return VT_VM_DONE;
            }
            case VT_OP_WAIT: {
                double ms;
                if (num_of(vm, popv(vm), &ms) < 0) return VT_VM_ERR;
                vm->pc = pc;
                if (wait_ms) *wait_ms = (ms < 0) ? 0 : (uint32_t)ms;
                return VT_VM_WAIT;
            }
            case VT_OP_NOW: {
                double d = vm->now_ms;
                vm->now_ms += vm->now_incr;
                if (push(vm, vnum(d)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_LEN: {
                char tmp[64];
                const char *p;
                uint32_t l;
                if (str_of(vm, popv(vm), tmp, sizeof tmp, &p, &l) < 0) return VT_VM_ERR;
                if (push(vm, vnum((double)l)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_STRN: {
                vt_val v = popv(vm);
                if (v.t == 2) {
                    if (push(vm, v) < 0) VMFAIL("程序非法");
                    break;
                }
                if (v.t != 1) VMFAIL("结果无值");
                {
                    char tmp[64];
                    uint32_t l;
                    vt_val r;
                    fmt_num(v.n, tmp, sizeof tmp);
                    l = (uint32_t)strlen(tmp);
                    if ((uint64_t)vm->sheap_used + l > VT_VM_SHEAP_CAP) VMFAIL("字符串池满");
                    memcpy(vm->sheap + vm->sheap_used, tmp, l);
                    r = vstr(vm->sheap + vm->sheap_used, l);
                    vm->sheap_used += l;
                    if (push(vm, r) < 0) VMFAIL("程序非法");
                }
                break;
            }
            case VT_OP_NUM: {
                vt_val v = popv(vm);
                if (v.t == 1) {
                    if (push(vm, v) < 0) VMFAIL("程序非法");
                    break;
                }
                if (v.t != 2) VMFAIL("结果无值");
                {
                    char tmp[80];
                    char *end;
                    double d;
                    if (v.sl >= sizeof tmp) VMFAIL("类型错");
                    memcpy(tmp, v.sp, v.sl);
                    tmp[v.sl] = '\0';
                    d = strtod(tmp, &end);
                    if (end == tmp || *end != '\0') VMFAIL("类型错");
                    if (push(vm, vnum(d)) < 0) VMFAIL("程序非法");
                }
                break;
            }
            case VT_OP_FLOOR:
            case VT_OP_CEIL:
            case VT_OP_ROUND:
            case VT_OP_ABS:
            case VT_OP_SQRT:
            case VT_OP_SIN:
            case VT_OP_COS: {
                double x, r;
                if (num_of(vm, popv(vm), &x) < 0) return VT_VM_ERR;
                if (op == VT_OP_FLOOR) r = floor(x);
                else if (op == VT_OP_CEIL) r = ceil(x);
                else if (op == VT_OP_ROUND) r = floor(x + 0.5);
                else if (op == VT_OP_ABS) r = fabs(x);
                else if (op == VT_OP_SQRT) r = sqrt(x);
                else if (op == VT_OP_SIN) r = sin(x * (VT_EXPR_PI / 180.0));
                else r = cos(x * (VT_EXPR_PI / 180.0));
                if (push(vm, vnum(r)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_MIN:
            case VT_OP_MAX:
            case VT_OP_ATAN2: {
                double x, y, r;
                vt_val b = popv(vm), a = popv(vm);
                if (num_of(vm, a, &x) < 0 || num_of(vm, b, &y) < 0) return VT_VM_ERR;
                if (op == VT_OP_MIN) r = (x < y) ? x : y;
                else if (op == VT_OP_MAX) r = (x > y) ? x : y;
                else (void)vm_atan2_deg(vm, x, y, &r);
                if (push(vm, vnum(r)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_RAND: {
                double a, b, r;
                if (num_of(vm, popv(vm), &b) < 0 || num_of(vm, popv(vm), &a) < 0) return VT_VM_ERR;
                if (!(a >= -1e15 && a <= 1e15) || !(b >= -1e15 && b <= 1e15)) VMFAIL("程序非法");
                {
                    double lo = (a < b) ? a : b, hi = (a < b) ? b : a;
                    long l = (long)ceil(lo), hh = (long)floor(hi);
                    if (hh < l) r = (double)l;
                    else r = (double)(l + (long)(rand() % (unsigned long)(hh - l + 1)));
                }
                if (push(vm, vnum(r)) < 0) VMFAIL("程序非法");
                break;
            }
            /* ---- 动作/展示/视觉：宿主钩子（无宿主 = 占位按栈效应消费） ---- */
            case VT_OP_TAP: {
                int x, y, ms;
                if (vm_pop_int(vm, &ms) < 0 || vm_pop_int(vm, &y) < 0 || vm_pop_int(vm, &x) < 0)
                    return VT_VM_ERR;
                if (!host || !host->touch_down) break;
                if (host->touch_down(host->ctx, x, y) != 0) VMFAIL("动作中止");
                vm->act_op = VT_OP_TAP;
                vm->act_phase = 1;
                vm->act_ms = ms;
                vm->act_pc = vm->pc;
                vm->pc = pc;
                {
                    int r = vm_act_step(vm, wait_ms);
                    if (r == 2) return VT_VM_ERR;
                    if (r == 1) return VT_VM_WAIT;
                    vm->act_op = 0;
                }
                break;
            }
            case VT_OP_SWIPE: {
                int ms, h1, h2, y1, x1, y2, x2;
                if (vm_pop_int(vm, &h2) < 0 || vm_pop_int(vm, &h1) < 0 || vm_pop_int(vm, &ms) < 0 ||
                    vm_pop_int(vm, &y2) < 0 || vm_pop_int(vm, &x2) < 0 ||
                    vm_pop_int(vm, &y1) < 0 || vm_pop_int(vm, &x1) < 0)
                    return VT_VM_ERR;
                if (!host || !host->touch_down) break;
                if (ms < 0) VMFAIL("程序非法");
                if (host->touch_down(host->ctx, x1, y1) != 0) VMFAIL("动作中止");
                vm->act_op = VT_OP_SWIPE;
                vm->act_x1 = x1; vm->act_y1 = y1; vm->act_x2 = x2; vm->act_y2 = y2;
                vm->act_ms = ms; vm->act_h1 = h1; vm->act_h2 = h2;
                vm->act_n = (uint32_t)ms / 10u;              /* 老执行器：N = max(2, ms/10) */
                if (vm->act_n < 2) vm->act_n = 2;
                vm->act_k = 0;
                vm->act_phase = (h1 > 0) ? 1 : 2;
                vm->act_pc = vm->pc;
                vm->pc = pc;
                {
                    int r = vm_act_step(vm, wait_ms);
                    if (r == 2) return VT_VM_ERR;
                    if (r == 1) return VT_VM_WAIT;
                    vm->act_op = 0;
                }
                break;
            }
            case VT_OP_DOWN: {
                int x, y;
                if (vm_pop_int(vm, &y) < 0 || vm_pop_int(vm, &x) < 0) return VT_VM_ERR;
                if (host && host->hold_down && host->hold_down(host->ctx, x, y) != 0) VMFAIL("动作中止");
                break;
            }
            case VT_OP_UP:
                if (host && host->hold_up && host->hold_up(host->ctx) != 0) VMFAIL("动作中止");
                break;
            case VT_OP_TOAST: {
                int ms;
                char buf[256];
                {
                    char tmp[64];
                    const char *p;
                    uint32_t l;
                    vt_val v;
                    if (vm_pop_int(vm, &ms) < 0) return VT_VM_ERR;   /* 栈顶 = ms（编译器先压文本后压时长） */
                    v = popv(vm);
                    if (str_of(vm, v, tmp, sizeof tmp, &p, &l) < 0) return VT_VM_ERR;
                    if (l >= sizeof buf) l = (uint32_t)sizeof buf - 1;
                    memcpy(buf, p, l);
                    buf[l] = '\0';
                }
                if (host && host->toast && host->toast(host->ctx, buf, ms) != 0) VMFAIL("动作中止");
                break;
            }
            case VT_OP_HL: {
                uint32_t ci = rd32(vm->code + pc);
                int ms;
                char rb[64];
                const char *ref;
                pc += 4;
                if (vm_pop_int(vm, &ms) < 0) return VT_VM_ERR;
                if (vm_cstr(vm, ci, rb, sizeof rb, 1, &ref) < 0) return VT_VM_ERR;
                if (host && host->hl_region && host->hl_region(host->ctx, ref, ms) != 0) VMFAIL("动作中止");
                break;
            }
            case VT_OP_BOX: {
                int ms, y2, x2, y1, x1;
                if (vm_pop_int(vm, &ms) < 0 || vm_pop_int(vm, &y2) < 0 || vm_pop_int(vm, &x2) < 0 ||
                    vm_pop_int(vm, &y1) < 0 || vm_pop_int(vm, &x1) < 0)
                    return VT_VM_ERR;
                if (host && host->hl_rect && host->hl_rect(host->ctx, x1, y1, x2, y2, ms) != 0) VMFAIL("动作中止");
                break;
            }
            case VT_OP_FINDIMG: {
                uint32_t ci = rd32(vm->code + pc), ri = rd32(vm->code + pc + 4);
                int persist, cc, th, hit = 0;
                char tb[64], rb[64];
                const char *tpl, *reg;
                pc += 8;
                if (vm_pop_int(vm, &persist) < 0 || vm_pop_int(vm, &cc) < 0 || vm_pop_int(vm, &th) < 0)
                    return VT_VM_ERR;
                if (vm_cstr(vm, ci, tb, sizeof tb, 1, &tpl) < 0) return VT_VM_ERR;
                if (vm_cstr(vm, ri, rb, sizeof rb, 0, &reg) < 0) return VT_VM_ERR;
                if (host && host->find_img &&
                    host->find_img(host->ctx, vm, tpl, reg, th, cc, persist, &hit) != 0)
                    VMFAIL("动作中止");
                if (push(vm, vnum(hit ? 1 : 0)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_FINDCOLOR: {
                uint32_t ci = rd32(vm->code + pc), ri = rd32(vm->code + pc + 4);
                char sb[64], rb[64];
                const char *set, *reg;
                int hit = 0;
                pc += 8;
                if (vm_cstr(vm, ci, sb, sizeof sb, 1, &set) < 0) return VT_VM_ERR;
                if (vm_cstr(vm, ri, rb, sizeof rb, 0, &reg) < 0) return VT_VM_ERR;
                if (host && host->find_color &&
                    host->find_color(host->ctx, vm, set, reg, &hit) != 0)
                    VMFAIL("动作中止");
                if (push(vm, vnum(hit ? 1 : 0)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_FINDDOT: {
                uint32_t ri = rd32(vm->code + pc);
                char rb[64];
                const char *reg;
                int color, tol, hit = 0;
                pc += 4;
                if (vm_pop_int(vm, &tol) < 0 || vm_pop_int(vm, &color) < 0) return VT_VM_ERR;
                if (vm_cstr(vm, ri, rb, sizeof rb, 0, &reg) < 0) return VT_VM_ERR;
                if (host && host->find_dot &&
                    host->find_dot(host->ctx, vm, color, tol, reg, &hit) != 0)
                    VMFAIL("动作中止");
                if (push(vm, vnum(hit ? 1 : 0)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_INREGION: {
                uint32_t ri = rd32(vm->code + pc);
                char rb[64];
                const char *reg;
                int x, y, hit = 0;
                pc += 4;
                if (vm_pop_int(vm, &y) < 0 || vm_pop_int(vm, &x) < 0) return VT_VM_ERR;
                if (vm_cstr(vm, ri, rb, sizeof rb, 1, &reg) < 0) return VT_VM_ERR;
                if (host && host->in_region &&
                    host->in_region(host->ctx, x, y, reg, &hit) != 0)
                    VMFAIL("动作中止");
                if (push(vm, vnum(hit ? 1 : 0)) < 0) VMFAIL("程序非法");
                break;
            }
            case VT_OP_SWON: {
                uint32_t ci = rd32(vm->code + pc);
                char nb[64];
                const char *nm;
                int hit = 0;
                pc += 4;
                if (vm_cstr(vm, ci, nb, sizeof nb, 1, &nm) < 0) return VT_VM_ERR;
                if (host && host->sw_on && host->sw_on(host->ctx, nm, &hit) != 0) VMFAIL("动作中止");
                if (push(vm, vnum(hit ? 1 : 0)) < 0) VMFAIL("程序非法");
                break;
            }
            default:
                VMFAIL("程序非法");
        }
    }
}

vt_vm_status vt_vm_run(vt_vm *vm) {
    for (;;) {
        uint32_t w = 0;
        vt_vm_status st = vt_vm_tick(vm, &w);
        if (st != VT_VM_WAIT) return st;
    }
}

/* ---------- 查询 ---------- */
void vt_vm_set_host(vt_vm *vm, const struct vt_vm_host *host) {
    vm->host = host;
}

void vt_vm_store_num(vt_vm *vm, uint32_t slot, double v) {
    if (slot < vm->slot_count) vm->g[slot] = vnum(v);
}

int vt_vm_slot_num(const vt_vm *vm, uint32_t slot, double *out) {
    if (slot >= vm->slot_count || vm->g[slot].t != 1) return 0;
    if (out) *out = vm->g[slot].n;
    return 1;
}

int vt_vm_global(const vt_vm *vm, const char *name, vt_val *out) {
    uint32_t i, nlen = (uint32_t)strlen(name);
    for (i = 0; i < vm->n_names; i++) {
        const uint8_t *e = vm->names + (size_t)i * 9;
        uint32_t idx, nc;
        if (e[0] != 0) continue;
        idx = rd32(e + 1);
        nc = rd32(e + 5);
        if (nc < vm->nconsts && vm->cpool[nc].t == 2 && vm->cpool[nc].sl == nlen &&
            memcmp(vm->cpool[nc].sp, name, nlen) == 0 && idx < vm->slot_count) {
            *out = vm->g[idx];
            return 1;
        }
    }
    return 0;
}
