#include "vt_internal.h"
#include <assert.h>
#include <sched.h>

struct vt_state g;
static struct vtq q;
static unsigned done;
static unsigned reads;

static void *producer(void *unused)
{
    (void)unused;
    for (unsigned i = 1; i <= 1000000; i++) {
        struct vt_ev ev = {0, VT_MOVE, (int)i, -(int)i, i};
        vtq_push(&q, &ev);
    }
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *consumer(void *unused)
{
    struct vt_ev ev;
    (void)unused;
    for (;;) {
        if (vtq_pop(&q, &ev)) {
            assert(ev.x == -ev.y && ev.ts == (uint64_t)ev.x);
            ++reads;
        } else if (__atomic_load_n(&done, __ATOMIC_ACQUIRE)) break;
        else sched_yield();
    }
    return NULL;
}

static const char request[] = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";

static void handshake(int authorized)
{
    int fds[2]; char data[1024];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    websocket_handshake(-1);
    uint64_t start = now_ns();
    assert(websocket_handshake(fds[0]) == 1);
    assert(now_ns() - start < 100000000ull);
    assert(write(fds[1], request, 16) == 16);
    assert(websocket_handshake(fds[0]) == 1);
    assert(write(fds[1], request + 16, strlen(request) - 16) == (ssize_t)strlen(request) - 16);
    if (authorized) {
        const char auth[] = "Authorization: Bearer test-secret\r\n\r\n";
        assert(write(fds[1], auth, sizeof auth - 1) == (ssize_t)sizeof auth - 1);
    } else assert(write(fds[1], "\r\n", 2) == 2);
    int rc = websocket_handshake(fds[0]);
    if (getenv("VTOUCH_WS_TOKEN") && !authorized) assert(rc < 0);
    else {
        assert(rc == 0);
        ssize_t n = recv(fds[1], data, sizeof data - 1, MSG_DONTWAIT); assert(n > 0); data[n] = 0;
        assert(strstr(data, "101 Switching Protocols"));
    }
    close(fds[0]); close(fds[1]); websocket_handshake(-1);
}

static void handshake_timeout(void)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    websocket_handshake(-1);
    assert(websocket_handshake(fds[0]) == 1);
    usleep(1050000);
    assert(websocket_handshake(fds[0]) == -1);
    close(fds[0]); close(fds[1]); websocket_handshake(-1);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    pthread_t p, c;
    assert(pthread_create(&p, NULL, producer, NULL) == 0);
    assert(pthread_create(&c, NULL, consumer, NULL) == 0);
    pthread_join(p, NULL); pthread_join(c, NULL);
    assert(reads > 0);
    unsetenv("VTOUCH_WS_TOKEN"); handshake(0);
    setenv("VTOUCH_WS_TOKEN", "test-secret", 1); handshake(0); handshake(1);
    handshake_timeout();
    puts("queue_ws_check: PASS (million events / partial handshake / optional auth)");
    return 0;
}
