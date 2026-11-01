/* Runtime regression: no input devices, root, UI process or touch injection. */
#include "vt_vm.h"
#include "vt_vision.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static vt_vm vm;

static void w32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static size_t blob(unsigned char *dst, const unsigned char *code, unsigned size)
{
    unsigned p = 0;
    memset(dst, 0, 128);
    memcpy(dst, "VTBC", 4); dst[4] = 1; p = 16;
    w32(dst + p, size); p += 4; memcpy(dst + p, code, size); p += size;
    w32(dst + p, 0); p += 4; /* constants */
    w32(dst + p, 7); p += 4; /* global slots */
    w32(dst + p, 0); p += 4; /* main locals */
    w32(dst + p, 0); p += 4; /* tables */
    w32(dst + p, 0); p += 4; /* functions */
    w32(dst + p, 1); p += 4; w32(dst + p, 0); p += 4; w32(dst + p, 9); p += 4;
    w32(dst + p, 0); p += 4; /* names */
    w32(dst + 12, p);
    return p;
}

static uint64_t real_clock(void) { return 123456789u; }

static int pending_dot(void *ctx, vt_vm *v, int rgb, int tol, const char *region, int *hit)
{
    int *calls = ctx;
    (void)v; (void)region;
    assert(rgb == 0 && tol == 0);
    *hit = 1;
    return ++*calls < 4 ? 1 : 0;
}

static int pending_image(void *ctx, vt_vm *v, const char *tpl, const char *region,
                         int thresh, int cc, int persist, int *hit)
{
    assert(tpl && strcmp(tpl, "t") == 0 && !region && thresh == 0 && cc == 0 && persist == 0);
    return pending_dot(ctx, v, 0, 0, region, hit);
}

int main(void)
{
    unsigned char bc[128], loop[] = { VT_OP_JMP, 0, 0, 0, 0 };
    uint32_t wait = 99;
    unsigned ticks = 0;
    assert(vt_vm_load(&vm, bc, (uint32_t)0) < 0);
    size_t n = blob(bc, loop, sizeof loop);
    assert(vt_vm_load(&vm, bc, (uint32_t)n) == 0);
    assert(vt_vm_tick(&vm, &wait) == VT_VM_YIELD && wait == 0 && vm.pc == 0);
    while (vt_vm_tick(&vm, &wait) == VT_VM_YIELD) assert(++ticks < 2000);
    assert(strstr(vm.err, "连续计算超限") && vm.err_line == 9);

    unsigned char clock_code[] = {VT_OP_NOW, VT_OP_SV, 0, 0, 0, 0};
    n = blob(bc, clock_code, sizeof clock_code);
    assert(vt_vm_load(&vm, bc, (uint32_t)n) == 0);
    vm.clock_ms = real_clock;
    assert(vt_vm_run(&vm) == VT_VM_DONE && vm.glob[0].n == 123456789u);

    /* FINDDOT with repeated pending result preserves popped operands. */
    unsigned char dot[] = {VT_OP_NOW, VT_OP_NOW, VT_OP_FINDDOT, 255, 255, 255, 255,
                           VT_OP_SV, 0, 0, 0, 0};
    n = blob(bc, dot, sizeof dot);
    assert(vt_vm_load(&vm, bc, (uint32_t)n) == 0);
    vm.now_incr = 0;
    int calls = 0;
    struct vt_vm_host host = {0}; host.ctx = &calls; host.find_dot = pending_dot;
    vt_vm_set_host(&vm, &host);
    for (int i = 0; i < 3; i++) assert(vt_vm_tick(&vm, &wait) == VT_VM_YIELD && vm.sp == 2);
    assert(vt_vm_tick(&vm, &wait) == VT_VM_DONE && calls == 4 && vm.glob[0].n == 1);

    unsigned char img[] = {VT_OP_NOW, VT_OP_NOW, VT_OP_NOW, VT_OP_FINDIMG,
                          0, 0, 0, 0, 255, 255, 255, 255, VT_OP_SV, 0, 0, 0, 0};
    n = blob(bc, img, sizeof img);
    /* Insert one string constant before the slot header. */
    unsigned constant_off = 20 + sizeof img;
    memmove(bc + constant_off + 10, bc + constant_off + 4, n - constant_off - 4);
    w32(bc + constant_off, 1); bc[constant_off + 4] = 1;
    w32(bc + constant_off + 5, 1); bc[constant_off + 9] = 't';
    n += 6; w32(bc + 12, (unsigned)n);
    assert(vt_vm_load(&vm, bc, (uint32_t)n) == 0);
    calls = 0; vm.now_incr = 0; host.find_img = pending_image; vt_vm_set_host(&vm, &host);
    for (int i = 0; i < 3; i++) assert(vt_vm_tick(&vm, &wait) == VT_VM_YIELD && vm.sp == 3);
    assert(vt_vm_tick(&vm, &wait) == VT_VM_DONE && calls == 4 && vm.glob[0].n == 1);

    unsigned char bad_wait[] = {VT_OP_NOW, VT_OP_WAIT};
    n = blob(bc, bad_wait, sizeof bad_wait);
    assert(vt_vm_load(&vm, bc, (uint32_t)n) == 0);
    vm.now_ms = NAN;
    assert(vt_vm_tick(&vm, &wait) == VT_VM_ERR && strstr(vm.err, "时长越界"));

    unsigned char rgba[47 * 39 * 4];
    for (size_t i = 0; i < sizeof rgba; i++) rgba[i] = (unsigned char)(i * 7);
    int x1, y1, x2, y2;
    unsigned rgb = (unsigned)rgba[0] << 16 | (unsigned)rgba[1] << 8 | rgba[2];
    assert(vt_vis_frame_prepare_color(rgba, 47, 39, 47 * 4) == VT_VIS_OK);
    assert(vt_vis_find_color(0, 0, 47, 39, rgb, 0, &x1, &y1) == VT_VIS_OK);
    unsigned char pixel = 0;
    assert(vt_vis_find_image(0, 0, 47, 39, &pixel, 1, 1, 0, &x2, &y2) == VT_VIS_BAD);
    assert(vt_vis_frame_prepare(rgba, 47, 39, 47 * 4) == VT_VIS_OK);
    assert(vt_vis_find_color(0, 0, 47, 39, rgb, 0, &x2, &y2) == VT_VIS_OK);
    assert(x1 == x2 && y1 == y2 && x1 == 0 && y1 == 0);
    vt_vis_frame_release();
    assert(vt_vis_find_color(0, 0, 47, 39, rgb, 0, &x2, &y2) == VT_VIS_BAD);
    puts("runtime_check: PASS (compute budget / clock / async operand restore / color frame)");
    return 0;
}
