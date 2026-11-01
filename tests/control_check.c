#include "vt_internal.h"
#include "vt_control.h"
#include "vt_binding.h"
#include <sys/un.h>
#include <sys/stat.h>
#include <assert.h>

static struct vt_state state;
struct vt_state *g_ptr = &state;
static struct vt_shm_header header;
static uint64_t clock_ns = 1000000000ull;
static unsigned aborts;
uint64_t now_ns(void) { return clock_ns; }
struct vt_shm_header *vt_shm_app_header(void) { return &header; }
void vt_ops_abort(const char *why) { assert(why); aborts++; g.op_run_state = 0; }

static int connect_client(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0); assert(fd >= 0);
    struct sockaddr_un addr = {0}; addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    assert(connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    return fd;
}

static void response(int fd, char *out, size_t size)
{
    for (int i = 0; i < 10; i++) {
        vt_control_tick();
        ssize_t n = recv(fd, out, size - 1, MSG_DONTWAIT);
        if (n > 0) { out[n] = 0; assert(strstr(out, "\nend=1\n")); return; }
    }
    assert(!"missing response");
}

static void request(const char *path, const char *build, const char *id, const char *verb, char *out, size_t size)
{
    int fd = connect_client(path); char line[192];
    int n = snprintf(line, sizeof line, "VTAPP 1 %s %s %s\n", build, id, verb);
    assert(write(fd, line, (size_t)n) == n); response(fd, out, size); close(fd);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char dir[] = "/data/data/com.termux/files/usr/tmp/control-check-XXXXXX";
#if !defined(__ANDROID__)
    snprintf(dir, sizeof dir, "/tmp/control-check-XXXXXX");
#endif
    assert(mkdtemp(dir)); char path[108]; snprintf(path, sizeof path, "%s/control.sock", dir);
    assert(vt_control_open(path) == 0);
    struct stat st; assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    char out[2048];
    header.ui_hb = 1; snprintf(header.app_reload_result, sizeof header.app_reload_result, "loaded");
    request(path, "different", "status1", "status", out, sizeof out);
    assert(strstr(out, "state=ok") && strstr(out, "request_id=status1") && strstr(out, "build_id=test-paired"));
    request(path, "different", "bad1", "shutdown", out, sizeof out);
    assert(strstr(out, "reason=build_mismatch") && !g.stop_flag);
    g.op_run_state = 1;
    request(path, "test-paired", "stop1", "stop_script", out, sizeof out);
    assert(strstr(out, "state=ok") && aborts == 1 && !g.op_run_state);

    int reload = connect_client(path);
    const char line[] = "VTAPP 1 test-paired reload1 reload\n";
    assert(write(reload, line, sizeof line - 1) == sizeof line - 1);
    vt_control_tick(); assert(header.app_reload_req == 1 && header.app_reload_done == 0);
    request(path, "test-paired", "status2", "status", out, sizeof out); /* waiting slot cannot starve another client */
    assert(strstr(out, "request_id=status2"));
    request(path, "test-paired", "reload2", "reload", out, sizeof out);
    assert(strstr(out, "reason=reload_busy"));
    header.app_ops_hash = 0x12345678; header.app_reload_rc = 0;
    snprintf(header.app_scheme, sizeof header.app_scheme, "sample");
    __atomic_store_n(&header.app_reload_done, 1, __ATOMIC_RELEASE);
    response(reload, out, sizeof out); close(reload);
    assert(strstr(out, "request_id=reload1") && strstr(out, "ops_hash=12345678") && strstr(out, "scheme=sample"));
    request(path, "test-paired", "reload1", "reload", out, sizeof out);
    assert(strstr(out, "state=ok") && header.app_reload_req == 1); /* replay doesn't reload twice */

    clock_ns += 3100000000ull;
    request(path, "test-paired", "reload3", "reload", out, sizeof out);
    assert(strstr(out, "reason=panel_unavailable"));
    int malformed = connect_client(path);
    const char bad_version[] = "VTAPP 2 test-paired version1 status\n";
    assert(write(malformed, bad_version, sizeof bad_version - 1) == sizeof bad_version - 1);
    response(malformed, out, sizeof out); close(malformed);
    assert(strstr(out, "reason=protocol_mismatch"));
    int slow = connect_client(path);
    assert(write(slow, "VTAPP ", 6) == 6); vt_control_tick();
    request(path, "different", "parallel", "status", out, sizeof out);
    assert(strstr(out, "request_id=parallel")); close(slow); vt_control_tick();
    header.ui_hb++;
    int pending = connect_client(path);
    const char pending_line[] = "VTAPP 1 test-paired timeout1 reload\n";
    assert(write(pending, pending_line, sizeof pending_line - 1) == sizeof pending_line - 1);
    vt_control_tick();
    assert(header.app_reload_req == 2);
    clock_ns += 9100000000ull;
    response(pending, out, sizeof out); close(pending);
    assert(strstr(out, "reason=request_timeout") && strstr(out, "request_id=timeout1"));
    header.app_reload_rc = 0;
    __atomic_store_n(&header.app_reload_done, 2, __ATOMIC_RELEASE);
    header.ui_hb++;
    request(path, "test-paired", "quit1", "shutdown", out, sizeof out);
    assert(strstr(out, "state=ok") && g.stop_flag);
    vt_control_close(); assert(access(path, F_OK) != 0); rmdir(dir);
    puts("control_check: PASS (private socket / version gate / request IDs / concurrent status / async reload / replay / shutdown)");
    return 0;
}
