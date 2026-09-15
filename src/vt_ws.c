/* vt_ws.c（§10 WebSocket 协议） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"

static unsigned char ws_in[WS_IN_MAX];
static size_t ws_in_len;

/* 给主循环用的两个小接口：输入缓冲里还有没处理完的帧 / 清空它（新客户端接入时）。
 * 缓冲本身留在本模块（外面不需要知道它长什么样）。 */
int ws_has_pending(void) { return ws_in_len > 0; }
/**
 * (vtouch-doc: ws_input_reset)
 * @brief 复位 WS 输入缓冲（新客户端接入前清掉上一个客户端的残包）。
 */
void ws_input_reset(void) { ws_in_len = 0; }
/**
 * (vtouch-doc: rol32)
 * @brief 32 位循环左移（SHA-1 内部用）。
 * @param   x        值
 * @param   n        位数
 * @return  左移结果。
 */

uint32_t rol32(uint32_t x, unsigned n) { return (x << n) | (x >> (32U - n)); }
/**
 * (vtouch-doc: be32)
 * @brief 读 4 字节大端整数（SHA-1 内部用）。
 * @param   p        字节指针
 * @return  大端解读结果。
 */

uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
/**
 * (vtouch-doc: sha1_block)
 * @brief 处理一个 64 字节块（SHA-1 内部）。
 * @param   s        上下文
 * @param   p        块起始
 */

void sha1_block(struct sha1 *s, const unsigned char *p)
{
    uint32_t w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
    for (i = 16; i < 80; ++i) w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; ++i) {
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5a827999U; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1U; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcU; }
        else { f = b ^ c ^ d; k = 0xca62c1d6U; }
        t = rol32(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
/**
 * (vtouch-doc: sha1_init)
 * @brief SHA-1 初始化。
 * @param   s        上下文
 */

void sha1_init(struct sha1 *s)
{
    s->h[0] = 0x67452301U; s->h[1] = 0xefcdab89U; s->h[2] = 0x98badcfeU;
    s->h[3] = 0x10325476U; s->h[4] = 0xc3d2e1f0U; s->bits = 0; s->used = 0;
}
/**
 * (vtouch-doc: sha1_update)
 * @brief SHA-1 追加数据。
 * @param   s        上下文
 * @param   p        数据
 * @param   n        长度
 */

void sha1_update(struct sha1 *s, const unsigned char *p, size_t n)
{
    s->bits += (uint64_t)n * 8;
    while (n) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->block + s->used, p, take); s->used += take; p += take; n -= take;
        if (s->used == 64) { sha1_block(s, s->block); s->used = 0; }
    }
}
/**
 * (vtouch-doc: sha1_final)
 * @brief SHA-1 收尾，输出 20 字节摘要（WS 握手用）。
 * @param   s        上下文
 * @param   out      20 字节输出
 */

void sha1_final(struct sha1 *s, unsigned char out[20])
{
    unsigned char pad[128];
    size_t n, i;
    uint64_t bits = s->bits;
    memset(pad, 0, sizeof pad); pad[0] = 0x80;
    n = (s->used < 56) ? (56 - s->used) : (120 - s->used);
    sha1_update(s, pad, n);
    for (i = 0; i < 8; ++i) pad[i] = (unsigned char)(bits >> (56 - i * 8));
    sha1_update(s, pad, 8);
    for (i = 0; i < 5; ++i) {
        out[i*4] = (unsigned char)(s->h[i] >> 24); out[i*4+1] = (unsigned char)(s->h[i] >> 16);
        out[i*4+2] = (unsigned char)(s->h[i] >> 8); out[i*4+3] = (unsigned char)s->h[i];
    }
}
/**
 * (vtouch-doc: base64)
 * @brief 标准 Base64 编码。
 * @param   in       输入
 * @param   n        输入长度
 * @param   out      输出缓冲
 * @param   cap      缓冲容量
 * @return  写入的字节数（含结尾 \0）；-1 缓冲不够。
 */

int base64(const unsigned char *in, size_t n, char *out, size_t cap)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    unsigned v;
    if (cap < ((n + 2) / 3) * 4 + 1) return -1;
    while (i < n) {
        unsigned char b0 = in[i++], b1 = 0, b2 = 0;
        int nb = 1;
        if (i < n) { b1 = in[i++]; nb = 2; }
        if (i < n) { b2 = in[i++]; nb = 3; }
        v = ((unsigned)b0 << 16) | ((unsigned)b1 << 8) | b2;
        out[o++] = tab[(v >> 18) & 63]; out[o++] = tab[(v >> 12) & 63];
        out[o++] = (nb > 1) ? tab[(v >> 6) & 63] : '=';
        out[o++] = (nb > 2) ? tab[v & 63] : '=';
    }
    out[o] = 0;
    return (int)o;
}
/**
 * (vtouch-doc: header_value)
 * @brief 从 HTTP 请求头里取某个头的值（头名大小写不敏感）。
 * @param   req      请求原文
 * @param   name     头名
 * @param   out      输出
 * @param   cap      缓冲容量
 * @return  0 找到；-1 没有或缓冲不够。
 */

int header_value(const char *req, const char *name, char *out, size_t cap)
{
    const char *p = req, *e, *c;
    size_t nl = strlen(name), n;
    while (*p) {
        e = strstr(p, "\r\n"); if (!e) break;
        c = memchr(p, ':', (size_t)(e - p));
        if (c && (size_t)(c - p) == nl && strncasecmp(p, name, nl) == 0) {
            p = c + 1; while (p < e && (*p == ' ' || *p == '\t')) ++p;
            n = (size_t)(e - p); while (n && (p[n-1] == ' ' || p[n-1] == '\t')) --n;
            if (n == 0 || n + 1 > cap) return -1;
            memcpy(out, p, n); out[n] = 0; return 0;
        }
        p = e + 2;
    }
    return -1;
}
/**
 * (vtouch-doc: has_token)
 * @brief 在请求头值里按逗号分词找 token（大小写不敏感，用于 Connection: Upgrade）。
 * @param   s        头值
 * @param   token    要找的 token
 * @return  1 有；0 没有。
 */

int has_token(const char *s, const char *token)
{
    size_t n = strlen(token);
    const char *p = s;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        if (strncasecmp(p, token, n) == 0 && (p[n] == 0 || p[n] == ',' || p[n] == ' ' || p[n] == '\t')) return 1;
        while (*p && *p != ',') ++p;
    }
    return 0;
}
/**
 * (vtouch-doc: write_full)
 * @brief 把 len 字节写满（EINTR、短写自动续写）。
 * @param   fd       目标 fd
 * @param   buf      数据
 * @param   len      长度
 * @return  0 成功；-1 出错。
 * @note    握手与上行同步写用它；下行的响应/事件走出站队列，不走这里。
 */

/* 立刻可写才发：socket 当刻不可写就失败，由调用方踢掉这个客户端。
 * 为什么不等（哪怕 20ms）：这条路径跑在触摸线程上，等客户端 = 用户感到「点一下先顿一下」。 */
int write_full(int fd, const void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        struct pollfd pw = { fd, POLLOUT, 0 };
        ssize_t n;
        if (!(poll(&pw, 1, 0) > 0 && (pw.revents & POLLOUT))) { errno = EAGAIN; return -1; }
        n = send(fd, (const char *)buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}
/**
 * (vtouch-doc: websocket_handshake)
 * @brief 读 HTTP 请求、校验 Upgrade 与 Sec-WebSocket-Key，回 101。
 * @param   fd       已 accept 的连接
 * @return  0 成功；-1 不是合法 WS 请求。
 * @note    握手期用带超时的阻塞读（最多被拖 300ms）。
 */

/* 握手：socket 上已设 SO_RCVTIMEO（300ms），所以慢客户端最多拖这么久；
 * 校验 5 个头 + 回 101 + Sec-WebSocket-Accept。 */
int websocket_handshake(int fd)
{
    char req[HTTP_MAX], key[128], upgrade[64], connection[128], version[32], accept[64];
    unsigned char digest[20];
    struct sha1 s;
    size_t used = 0;
    ssize_t n;
    const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    while (used + 1 < sizeof req) {
        n = recv(fd, req + used, 1, 0);
        if (n <= 0) return -1;                       /* 超时/断开都算失败 */
        used += (size_t)n; req[used] = 0;
        if (used >= 4 && req[used - 4] == 13 && req[used - 3] == 10 &&
            req[used - 2] == 13 && req[used - 1] == 10) break;   /* CRLFCRLF（用数值避开转义） */
    }
    if (used + 1 >= sizeof req || strncmp(req, "GET ", 4) != 0 ||
        header_value(req, "Sec-WebSocket-Key", key, sizeof key) < 0 ||
        header_value(req, "Upgrade", upgrade, sizeof upgrade) < 0 ||
        header_value(req, "Connection", connection, sizeof connection) < 0 ||
        header_value(req, "Sec-WebSocket-Version", version, sizeof version) < 0 ||
        strcasecmp(upgrade, "websocket") != 0 || !has_token(connection, "Upgrade") ||
        strcmp(version, "13") != 0 || strlen(key) != 24) return -1;
    sha1_init(&s);
    sha1_update(&s, (const unsigned char *)key, strlen(key));
    sha1_update(&s, (const unsigned char *)guid, strlen(guid));
    sha1_final(&s, digest);
    if (base64(digest, 20, accept, sizeof accept) < 0) return -1;
    {
        char response[512];
        int len = snprintf(response, sizeof response,
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
        return (len > 0 && (size_t)len < sizeof response &&
                write_full(fd, response, (size_t)len) == 0) ? 0 : -1;
    }
}
/**
 * (vtouch-doc: ws_send)
 * @brief 直接发一个 WS 帧（控制帧：close / pong 用）。
 * @param   fd       连接
 * @param   opcode   操作码
 * @param   p        正文
 * @param   n        正文长度
 * @return  0 成功；-1 失败。
 * @note    只给控制帧用；文本帧请走 outq_push_text。
 */

int ws_send(int fd, unsigned opcode, const unsigned char *p, size_t n)
{
    unsigned char h[10];
    size_t hn;
    if (n > MAX_PAYLOAD || (opcode >= 8 && n > 125)) return -1;
    h[0] = (unsigned char)(0x80 | (opcode & 15));
    if (n < 126) { h[1] = (unsigned char)n; hn = 2; }
    else { h[1] = 126; h[2] = (unsigned char)(n >> 8); h[3] = (unsigned char)n; hn = 4; }
    return write_full(fd, h, hn) || write_full(fd, p, n);
}
/**
 * (vtouch-doc: drop_client)
 * @brief 丢弃当前客户端：关连接 + 抬掉它的虚拟触点 + 清订阅位 + 清出站队列。
 * @note    残留的订阅与残包不许串给下一个客户端（§4.6）。
 */

/* 丢掉当前客户端：关连接 + 抬掉它的虚拟触点（只 close 会把虚拟手指永久粘在设备上） */
void drop_client(void)
{
    if (g.client_fd >= 0) {
        close(g.client_fd);
        g.client_fd = -1;
        fprintf(stderr, "vtouchd: ws client dropped\n");
    }
    ws_in_len = 0;
    g.sub_mask = 0;          /* §4.6：断连/被踢 → 订阅清零（下一个客户端要自己重新 sub） */
    outq_reset();          /* §4.6：断连/被踢 → 出站队列销毁（残包不许串给下一个客户端） */
    owner_reset();
}
/**
 * (vtouch-doc: ws_peek_frame)
 * @brief 试着从接收缓冲里解析出一个完整帧的表头。
 * @param   frame_len 输出整帧长度
 * @param   opcode   输出操作码
 * @param   payload_off 输出正文偏移
 * @return  1 解析到；0 数据不够；-1 协议错。
 */

/* 输入缓冲：半包不消费，留到下一轮 poll 继续拼（以前逐字段 recv，跨 TCP 段就误判断线） */
int ws_peek_frame(size_t *frame_len, unsigned *opcode, size_t *payload_off)
{
    unsigned len7, fin, masked, op;
    size_t off = 2, i;
    uint64_t len;
    if (ws_in_len < 2) return 0;
    fin = ws_in[0] >> 7; op = ws_in[0] & 15;
    masked = ws_in[1] >> 7; len7 = ws_in[1] & 127;
    if (!masked || !fin || (op != 1 && op != 8 && op != 9 && op != 10)) return -1;
    len = len7;
    if (len7 == 126) {
        if (ws_in_len < 4) return 0;
        len = ((uint64_t)ws_in[2] << 8) | ws_in[3];
        off = 4;
    } else if (len7 == 127) {
        if (ws_in_len < 10) return 0;
        len = 0;
        for (i = 0; i < 8; i++) len = (len << 8) | ws_in[2 + i];
        off = 10;
    }
    if (len > MAX_PAYLOAD) return -2;
    if (op >= 8 && len > 125) return -2;
    *opcode = op;
    *payload_off = off + 4;
    *frame_len = off + 4 + (size_t)len;
    return (ws_in_len < *frame_len) ? 0 : 1;
}
/**
 * (vtouch-doc: ws_next_frame)
 * @brief 取出一个完整帧的正文（必要时继续收）。
 * @param   payload  输出正文
 * @param   plen     输出长度
 * @param   opcode   输出操作码
 * @return  0 取到；1 暂时没有数据（EAGAIN）；-1 连接关闭或协议错。
 * @note    半包不消费，留到下一轮 poll 继续拼。
 */

int ws_next_frame(unsigned char *payload, size_t *plen, unsigned *opcode)
{
    for (;;) {
        size_t frame_len = 0, poff = 0, i, len;
        unsigned op = 0;
        int r = ws_peek_frame(&frame_len, &op, &poff);
        if (r < 0) return r;
        if (r == 0) {
            ssize_t n;
            if (ws_in_len >= sizeof ws_in) { ws_in_len = 0; return -1; }
            do { n = recv(g.client_fd, ws_in + ws_in_len, sizeof ws_in - ws_in_len, 0); }
            while (n < 0 && errno == EINTR && !g.stop_flag);
            if (n > 0) { ws_in_len += (size_t)n; continue; }
            if (n == 0) return -1;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
            return -1;
        }
        len = frame_len - poff;
        {
            const unsigned char *mask = ws_in + poff - 4;
            for (i = 0; i < len; i++) payload[i] = (unsigned char)(ws_in[poff + i] ^ mask[i & 3]);
        }
        memmove(ws_in, ws_in + frame_len, ws_in_len - frame_len);
        ws_in_len -= frame_len;
        *plen = len;
        *opcode = op;
        return 0;
    }
}
/**
 * (vtouch-doc: client_frame)
 * @brief 处理客户端可读事件：一轮最多 32 个帧，解帧 → handle_line → 响应入出站队列。
 * @return  0 保持连接；-1 断开（协议错或连接关闭）。
 */

/* 单轮最多处理 32 帧：一个 TCP 段里挤多条命令不会被「下一轮 poll」饿死，
 * 也不会让一整批命令长时间占住 poll 循环。返回 0 = 保持连接，-1 = 断开。 */
int client_frame(void)
{
    unsigned char payload[MAX_PAYLOAD];
    char line[MAX_LINE], resp[MAX_LINE];
    int k;
    for (k = 0; k < 32; k++) {
        size_t len = 0, i;
        unsigned opcode = 0;
        int r = ws_next_frame(payload, &len, &opcode);
        if (r == 1) return 0;
        if (r < 0) {
            static const unsigned char c_proto[2] = { 0x03, 0xea };   /* 1002 */
            static const unsigned char c_big[2]   = { 0x03, 0xf1 };   /* 1009 */
            ws_send(g.client_fd, 8, r == -2 ? c_big : c_proto, 2);
            ws_in_len = 0;
            return -1;
        }
        if (opcode == 8) { ws_send(g.client_fd, 8, payload, len); return -1; }
        if (opcode == 9) {   /* ping → pong，可写才发 */
            struct pollfd pw = { g.client_fd, POLLOUT, 0 };
            if (poll(&pw, 1, 0) > 0 && (pw.revents & POLLOUT) && ws_send(g.client_fd, 10, payload, len) < 0) return -1;
            continue;
        }
        if (opcode == 10) continue;
        for (i = 0; i < len; i++) if (payload[i] == 10 || payload[i] == 13) payload[i] = ' ';
        if (len >= sizeof line) return -1;
        memcpy(line, payload, len); line[len] = 0;
        handle_line(line, resp, sizeof resp);
        /* §4.5：响应进发送队列，主线程只在主循环里刷 —— socket 慢不再卡住注入热路径 */
        outq_push_text(resp, strlen(resp));
    }
    return 0;
}
/**
 * (vtouch-doc: make_listen)
 * @brief 建监听 socket，只绑 127.0.0.1（回环），不对外暴露。
 * @return  fd；-1 失败（调用方以退出码 6 退出）。
 */

int make_listen(void)
{
    int fd, opt = 1;
    struct sockaddr_in a;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g.ws_port);
    if (inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) != 1 ||      /* 只绑回环 */
        bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
        close(fd); return -1;
    }
    return fd;
}
/**
 * (vtouch-doc: cmd_meta)
 * @brief 命令族：ping / res / reset（不碰触点的元命令）。
 * @param   t        命令词
 * @param   stp      strtok_r 状态
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @return  1 不是本族命令（交给下一族）；0 / -1 = 已处理（-1 时 resp 是错误响应）。
 */

/* ---- §10.2 命令族：每族一个函数，只认自己的命令，handle_line 只做分派 ----
 * 返回约定：1 = 不是我的命令（交给下一族）；0 / -1 = 我处理了（resp 已写好，-1 表示是错误响应）。
 * 这次拆分只动组织、不动语义：响应文本、错误词、检查顺序、状态改动都逐字保留（见 build/_equiv_drive.py 的比对）。 */

/* ping / res / reset：不碰触点的元命令 */
int cmd_meta(char *t, char **stp, char *resp, size_t cap)
{
    (void)stp;                                          /* 元命令不带参数（签名与其它族保持一致，便于分派） */
    if (!strcmp(t, "ping")) { snprintf(resp, cap, "pong"); return 0; }
    if (!strcmp(t, "res")) {
        snprintf(resp, cap, "res %d %d raw %d %d %d %d", g.logical_width, g.logical_height,
                 g.axmin[0], g.axmax[0], g.axmin[1], g.axmax[1]);
        return 0;
    }
    if (!strcmp(t, "reset")) {
        if (g.frame_open) { snprintf(resp, cap, "err frame"); return -1; }
        owner_reset(); snprintf(resp, cap, "ok"); return 0;
    }
    return 1;
}
/**
 * (vtouch-doc: cmd_point_once)
 * @brief 命令族：up / down / move —— 单点命令，每个命令提交一帧。
 * @param   t        命令词
 * @param   stp      strtok_r 状态
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @return  1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。
 */

/* up / down / move：单点命令，每个命令提交一帧 */
int cmd_point_once(char *t, char **stp, char *resp, size_t cap)
{
    int slot, x, y;
    if (!strcmp(t, "up")) {
        char *ss = strtok_r(NULL, " \t", stp);
        if (g.frame_open || !ss || strtok_r(NULL, " \t", stp) ||
            parse_long(ss, 0, g.vslots - 1, &slot) ||
            set_virtual(g.virt, slot, t, g.virt[slot].x, g.virt[slot].y) || emit_frame() < 0) {
            snprintf(resp, cap, "err point"); return -1;
        }
        snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "down") || !strcmp(t, "move")) {
        char *ss = strtok_r(NULL, " \t", stp), *sx = strtok_r(NULL, " \t", stp), *sy = strtok_r(NULL, " \t", stp);
        int lx, ly;
        if (g.frame_open || !ss || !sx || !sy || strtok_r(NULL, " \t", stp) ||
            parse_long(ss, 0, g.vslots - 1, &slot) || parse_long(sx, 0, g.logical_width - 1, &lx) ||
            parse_long(sy, 0, g.logical_height - 1, &ly) ||
            logical_to_raw(lx, 0, &x) || logical_to_raw(ly, 1, &y) ||
            set_virtual(g.virt, slot, t, x, y) || emit_frame() < 0) {
            snprintf(resp, cap, "err point"); return -1;
        }
        snprintf(resp, cap, "ok"); return 0;
    }
    return 1;
}
/**
 * (vtouch-doc: cmd_frame)
 * @brief 命令族：begin_frame / point / end_frame —— 帧内多点，一次 SYN 提交。
 * @param   t        命令词
 * @param   stp      strtok_r 状态
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @return  1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。
 */

/* begin_frame / point / end_frame：帧内多点，一次 SYN 提交 */
int cmd_frame(char *t, char **stp, char *resp, size_t cap)
{
    int slot, x, y;
    if (!strcmp(t, "begin_frame")) {
        if (g.frame_open || strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err frame"); return -1; }
        memcpy(g.staged, g.virt, sizeof g.staged);
        g.frame_open = 1; memset(g.frame_seen, 0, sizeof g.frame_seen);
        snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "point")) {
        char *ss = strtok_r(NULL, " \t", stp), *state = strtok_r(NULL, " \t", stp);
        char *sx = strtok_r(NULL, " \t", stp), *sy = strtok_r(NULL, " \t", stp);
        int lx, ly;
        if (!g.frame_open || !ss || !state || !sx || !sy || strtok_r(NULL, " \t", stp) ||
            parse_long(ss, 0, g.vslots - 1, &slot) || parse_long(sx, 0, g.logical_width - 1, &lx) ||
            parse_long(sy, 0, g.logical_height - 1, &ly) ||
            logical_to_raw(lx, 0, &x) || logical_to_raw(ly, 1, &y) ||
            g.frame_seen[slot] || set_virtual(g.staged, slot, state, x, y)) {
            snprintf(resp, cap, "err point"); return -1;
        }
        g.frame_seen[slot] = 1; snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "end_frame")) {
        if (!g.frame_open || strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err frame"); return -1; }
        memcpy(g.virt, g.staged, sizeof g.virt);
        if (emit_frame() < 0) { g.frame_open = 0; snprintf(resp, cap, "err frame"); return -1; }
        g.frame_open = 0; snprintf(resp, cap, "ok"); return 0;
    }
    return 1;
}
/**
 * (vtouch-doc: cmd_region)
 * @brief 命令族：region add | clear | list。
 * @param   t        命令词
 * @param   stp      strtok_r 状态
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @return  1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。
 * @note    主线程只写表（短锁），判定全在区域线程。
 */

/* region add|del|clear|list：主线程只写表（短锁），判定全在区域线程（§4.4/§4.6） */
int cmd_region(char *t, char **stp, char *resp, size_t cap)
{
    char *op;
    if (strcmp(t, "region")) return 1;
    op = strtok_r(NULL, " \t", stp);
    if (op && !strcmp(op, "clear")) {
        if (strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err region"); return -1; }
        regions_clear(); snprintf(resp, cap, "ok %d", g.region_count); return 0;
    }
    if (op && !strcmp(op, "list")) {
        size_t used = 0;
        int i, n;
        if (strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err region"); return -1; }
        pthread_mutex_lock(&g.region_lock);
        n = g.region_count;
        for (i = 0; i < g.region_count && used + 1 < cap; i++) {
            int w = snprintf(resp + used, cap - used, "region %s %d %d %d %d %d %d\n", g.regions[i].id,
                             g.regions[i].type, g.regions[i].a1, g.regions[i].a2, g.regions[i].a3, g.regions[i].a4,
                             g.regions[i].enabled);
            if (w <= 0 || (size_t)w >= cap - used) break;   /* 放不下就截断：客户端以末行 end 兜底 */
            used += (size_t)w;
        }
        pthread_mutex_unlock(&g.region_lock);
        snprintf(resp + used, cap - used, "end %d", n);
        return 0;
    }
    if (op && !strcmp(op, "add")) {
        char *sid = strtok_r(NULL, " \t", stp), *stype = strtok_r(NULL, " \t", stp);
        char *sa1 = strtok_r(NULL, " \t", stp), *sa2 = strtok_r(NULL, " \t", stp);
        char *sa3 = strtok_r(NULL, " \t", stp), *sa4 = strtok_r(NULL, " \t", stp);
        char *sen = strtok_r(NULL, " \t", stp);
        int type, a1, a2, a3, a4, en;
        /* 这里只做「词数 + 数值范围」检查；id 去重/上限、几何合法性（超出逻辑尺寸等）交给 region_add */
        if (!sid || !*sid || strlen(sid) > REGION_ID_MAX || !stype || !sa1 || !sa2 || !sa3 || !sa4 || !sen ||
            strtok_r(NULL, " \t", stp) ||
            parse_long(stype, 0, 1, &type) || parse_long(sa1, 0, 100000, &a1) || parse_long(sa2, 0, 100000, &a2) ||
            parse_long(sa3, 0, 100000, &a3) || parse_long(sa4, 0, 100000, &a4) || parse_long(sen, 0, 1, &en) ||
            region_add(sid, type, a1, a2, a3, a4, en) != 0) {
            snprintf(resp, cap, "err region"); return -1;
        }
        snprintf(resp, cap, "ok %d", g.region_count); return 0;
    }
    snprintf(resp, cap, "err region"); return -1;
}
/**
 * (vtouch-doc: cmd_sub)
 * @brief 命令族：sub [phys|region|all] / unsub（裸 sub = 两个通道都订）。
 * @param   t        命令词
 * @param   stp      strtok_r 状态
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @return  1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。
 */

/* sub [g.phys|region|all] / unsub：裸 sub = 全订（老脚本语义不变，§4.6） */
int cmd_sub(char *t, char **stp, char *resp, size_t cap)
{
    if (!strcmp(t, "sub")) {
        char *ch = strtok_r(NULL, " \t", stp);
        int want = SUB_PHYS | SUB_REGION;
        if (ch) {
            if (!strcmp(ch, "g.phys")) want = SUB_PHYS;
            else if (!strcmp(ch, "region")) want = SUB_REGION;
            else if (!strcmp(ch, "all")) want = SUB_PHYS | SUB_REGION;
            else { snprintf(resp, cap, "err sub"); return -1; }
        }
        if (strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err sub"); return -1; }
        g.sub_mask = want; snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "unsub")) {
        if (strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err sub"); return -1; }
        g.sub_mask = 0; snprintf(resp, cap, "ok"); return 0;
    }
    return 1;
}
/**
 * (vtouch-doc: handle_line)
 * @brief 一行命令 → 一行回包：按命令族分派（每族一个 cmd_* 函数）。
 * @param   line     命令文本（原地改）
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @return  0 有响应；-1 错误响应。
 * @note    响应文本拼进 resp，由调用方（client_frame）入出站队列。
 */

/* 一行命令 -> 一行回包：这里只剩分派 */
int handle_line(char *line, char *resp, size_t cap)
{
    char *t, *st;
    int r;
    line[strcspn(line, "\r\n")] = 0;
    t = strtok_r(line, " \t", &st);
    if (!t) { snprintf(resp, cap, "err empty"); return -1; }
    if ((r = cmd_meta(t, &st, resp, cap)) != 1) return r;
    if ((r = cmd_point_once(t, &st, resp, cap)) != 1) return r;
    if ((r = cmd_frame(t, &st, resp, cap)) != 1) return r;
    if ((r = cmd_region(t, &st, resp, cap)) != 1) return r;
    if ((r = cmd_sub(t, &st, resp, cap)) != 1) return r;
    snprintf(resp, cap, "err unknown"); return -1;
}

