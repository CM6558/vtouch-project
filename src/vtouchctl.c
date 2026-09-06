/*
 * vtouchctl.c — vtouchd 的命令行客户端（第三方程序调用入口）
 *
 * 用法:
 *   vtouchctl tap 720 1584
 *   vtouchctl swipe 200 2000 900 2000 300
 *   vtouchctl pinch 720 1584 200 400 300
 *   vtouchctl -s /path/to.sock down 0 720 1584
 *
 * 把参数拼成一行发给守护进程，并把响应打印到 stdout。
 * 可选环境变量 VTOUCH_SOCK 指定 socket 路径。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#define DEFAULT_SOCKET "/data/local/tmp/vtouch.sock"

int main(int argc, char **argv) {
    const char *sock = getenv("VTOUCH_SOCK");
    struct sockaddr_un addr;
    char buf[4096];
    int fd, i, off = 0, n = 0;
    char one;

    if (!sock || !*sock) sock = DEFAULT_SOCKET;

    if (argc > 2 && strcmp(argv[1], "-s") == 0) {
        sock = argv[2];
        argv += 2; argc -= 2;
    }
    if (argc < 2) { fprintf(stderr, "用法: %s <命令> [参数...] | --interactive\n", argv[0]); return 2; }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect %s: %s\n", sock, strerror(errno));
        close(fd); return 1;
    }

    if (strcmp(argv[1], "--interactive") == 0 || strcmp(argv[1], "-i") == 0) {
        char line[512];
        while (fgets(line, sizeof(line), stdin)) {
            size_t len = strlen(line);
            if (len == 0 || line[len - 1] != '\n') { fprintf(stderr, "line too long\n"); break; }
            if (write(fd, line, len) != (ssize_t)len) { perror("write"); break; }
            while ((n = (int)read(fd, &one, 1)) > 0) {
                putchar(one);
                if (one == '\n') break;
            }
            fflush(stdout);
            if (n <= 0) break;
        }
        close(fd);
        return 0;
    }

    for (i = 1; i < argc; i++) {
        if (i > 1) { buf[off++] = ' '; }
        int w = snprintf(buf + off, sizeof(buf) - off, "%s", argv[i]);
        off += w;
    }
    buf[off++] = '\n';
    if (write(fd, buf, off) != off) { perror("write"); close(fd); return 1; }

    while ((n = (int)read(fd, &one, 1)) > 0) {
        if (one == '\n') break;
        putchar(one);
    }
    putchar('\n');
    close(fd);
    return 0;
}