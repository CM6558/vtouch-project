/* Test the real executor capture state machine with in-memory frame headers. No device IO. */
#include "vt_internal.h"
#include "../src/vt_ops.c"
#include <assert.h>

static struct vt_state state;
struct vt_state *g_ptr = &state;
static struct vt_shm_frame_hdr header;
static unsigned char pixels[32 * 32 * 4];
static uint64_t fake_ns = 1000000000ull;

uint64_t now_ns(void) { return fake_ns; }
struct vt_shm_frame_hdr *vt_shm_frame(void) { return &header; }
uint8_t *vt_shm_frame_buf(int idx) { assert(idx == 0); return pixels; }
uint32_t vt_shm_ui_hb(void) { return 1; }
int vt_shm_panel_rot(void) { return 0; }
void vt_shm_tick(void) {}
void vt_shm_edit_apply(void) {}
int vt_shm_stop_req(void) { return 0; }
void physical_events(void) { assert(!"synchronous path unexpectedly called"); }

static void complete(void)
{
    header.flags = VT_FRAME_F_VALID;
    header.width = 32; header.height = 32; header.stride = 128;
    header.buf_idx = 0; header.rotation = 0; header.seq = 2; header.ts_ns = fake_ns;
    header.req_seq = header.req_pending;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g.logical_width = 32; g.logical_height = 32;
    pthread_mutex_init(&g.region_lock, NULL);
    struct op_vis_job job = {0};
    int x, y, attempts;
    uint64_t ms;
    const char *err;
    int rc = vis_exec_find(1, "", "", 0, 0, 0, &x, &y, &ms, &attempts, &err, &job);
    assert(rc == 1 && job.req && job.tries == 1);
    uint32_t req = job.req;
    rc = vis_exec_find(1, "", "", 0, 0, 0, &x, &y, &ms, &attempts, &err, &job);
    assert(rc == 1 && job.req == req && job.tries == 1);
    complete();
    rc = vis_exec_find(1, "", "", 0, 0, 0, &x, &y, &ms, &attempts, &err, &job);
    assert(rc == 0 && x == 0 && y == 0 && attempts == 1 && !job.active);

    memset(pixels, 255, sizeof pixels); S_vis_have = 0;
    rc = vis_exec_find(1, "", "", 0, 0, 100, &x, &y, &ms, &attempts, &err, &job);
    assert(rc == 1);
    complete();
    rc = vis_exec_find(1, "", "", 0, 0, 100, &x, &y, &ms, &attempts, &err, &job);
    assert(rc == 1 && job.req == 0 && job.active);
    fake_ns += 10000000ull;
    rc = vis_exec_find(1, "", "", 0, 0, 100, &x, &y, &ms, &attempts, &err, &job);
    assert(rc == 1 && job.req != req);
    fake_ns += 100000000ull; complete();
    rc = vis_exec_find(1, "", "", 0, 0, 100, &x, &y, &ms, &attempts, &err, &job);
    assert(rc == -1 && attempts == 2 && !job.active);

    S_vis_have = 0;
    rc = vis_exec_find(1, "", "", 0, 0, 0, &x, &y, &ms, &attempts, &err, &job);
    assert(rc == 1);
    fake_ns += 1100000000ull;
    rc = vis_exec_find(1, "", "", 0, 0, 0, &x, &y, &ms, &attempts, &err, &job);
    assert(rc == -2 && err && !job.active);
    S_vm_vis.active = 1; S_vm_vis.req = 9; op_vis_cancel();
    assert(!S_vm_vis.active && !S_vm_vis.req && !S_vis_have);
    puts("vision_async_check: PASS (request resume / hit / persistent miss / capture timeout / cancel)");
    return 0;
}
