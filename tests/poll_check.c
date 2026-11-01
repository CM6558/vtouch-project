/* Use pipes/socketpair instead of input/uinput; verify a slow handshake doesn't stall the poll loop. */
#define main vtouch_entry_not_run
#include "../src/vtouchd.c"
#undef main
#include <assert.h>

static unsigned physical_calls;
void physical_events(void)
{
    char byte;
    assert(read(g.input_fd, &byte, 1) == 1);
    physical_calls++;
}

int main(void)
{
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    g.input_fd = pipefd[0]; g.u_fd = -1; g.client_fd = -1;
    g.listen_fd = make_listen(); assert(g.listen_fd >= 0);
    struct sockaddr_in addr; socklen_t len = sizeof addr;
    assert(getsockname(g.listen_fd, (struct sockaddr *)&addr, &len) == 0);
    int client = socket(AF_INET, SOCK_STREAM, 0); assert(client >= 0);
    assert(connect(client, (struct sockaddr *)&addr, sizeof addr) == 0);
    uint64_t start = now_ns();
    assert(vtouch_poll_step() == 0);
    assert(now_ns() - start < 100000000ull && g.client_fd < 0);
    assert(write(pipefd[1], "x", 1) == 1);
    start = now_ns();
    assert(vtouch_poll_step() == 0 && physical_calls == 1);
    assert(now_ns() - start < 100000000ull);
    close(client);
    assert(vtouch_poll_step() == 0);
    close(g.listen_fd); close(pipefd[0]); close(pipefd[1]);
    puts("poll_check: PASS (partial handshake keeps physical input responsive)");
    return 0;
}
