/* vt_ws.c（§10 WebSocket 协议） —— 模块地图见 vt_internal.h；私有状态就近放 static，共享状态走 g。 */
#include "vt_internal.h"
#include <stdlib.h>          /* strtol：解析 sub 的槽号列表 */

static unsigned char ws_in[WS_IN_MAX];
static size_t ws_in_len;

/**
 * (vtouch-doc: ws_has_pending)
 * @brief WS 输入缓冲里是否还有没解析完的半包数据（主循环据此继续挂 POLLIN）。
 * @return  1 有；0 没有。
 * @note    半包不消费：解析不出完整帧就留着，等下一轮 poll 再拼。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   给主循环用的两个小接口：输入缓冲里还有没处理完的帧 / 清空它（新客户端接入时）。
 *   缓冲本身留在本模块（外面不需要知道它长什么样）。
 */
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
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   立刻可写才发：socket 当刻不可写就失败，由调用方踢掉这个客户端。
 *   为什么不等（哪怕 20ms）：这条路径跑在触摸线程上，等客户端 = 用户感到「点一下先顿一下」。
 */
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
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   握手：socket 上已设 SO_RCVTIMEO（300ms），所以慢客户端最多拖这么久；
 *   校验 5 个头 + 回 101 + Sec-WebSocket-Accept。
 */
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
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   丢掉当前客户端：关连接 + 抬掉它的虚拟触点（只 close 会把虚拟手指永久粘在设备上）
 */
void drop_client(void)
{
    if (g.client_fd >= 0) {
        close(g.client_fd);
        g.client_fd = -1;
        fprintf(stderr, "vtouchd: ws client dropped\n");
    }
    ws_in_len = 0;
    g.sub_mask = 0;          /* §4.6：断连/被踢 → 订阅清零（下一个客户端要自己重新 sub） */
    g.sub_phys_mask = 0; g.sub_phys_ev = 0;      /* 过滤器一并复位（0/空 = 全通） */
    g.sub_region_id[0] = 0; g.sub_region_ev = 0;
    g.sub_phys_ts = g.sub_region_ts = 0;          /* 线路格式也复位（下一个客户端自己订） */
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
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   输入缓冲：半包不消费，留到下一轮 poll 继续拼（以前逐字段 recv，跨 TCP 段就误判断线）
 */
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
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   单轮最多处理 32 帧：一个 TCP 段里挤多条命令不会被「下一轮 poll」饿死，
 *   也不会让一整批命令长时间占住 poll 循环。返回 0 = 保持连接，-1 = 断开。
 */
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
        /* B3：region list 分帧时已由 cmd_region 自己逐行发过（resp[0]=0），这里别再发一个空帧；
         * 其余命令族照旧 —— 每条路径都会把完整回包写进 resp，行为零变化。 */
        if (resp[0]) outq_push_text(resp, strlen(resp));
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
/* ---- §10.2 命令族：每族一个函数，只认自己的命令，handle_line 只做分派 ----
 * 返回约定：1 = 不是我的命令（交给下一族）；0 / -1 = 我处理了（resp 已写好，-1 表示是错误响应）。
 * 这次拆分只动组织、不动语义：响应文本、错误词、检查顺序、状态改动都逐字保留（见 build/_equiv_drive.py 的比对）。 */

/**
 * (vtouch-doc: cmd_meta)
 * @brief 命令族：ping / res / reset（不碰触点的元命令）。
 * @param   t        命令词
 * @param   stp      strtok_r 状态
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @return  1 不是本族命令（交给下一族）；0 / -1 = 已处理（-1 时 resp 是错误响应）。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   ping / res / reset：不碰触点的元命令
 */
int cmd_meta(char *t, char **stp, char *resp, size_t cap)
{
    (void)stp;                                          /* 元命令不带参数（签名与其它族保持一致，便于分派） */
    if (!strcmp(t, "ping")) { snprintf(resp, cap, "pong"); return 0; }
    if (!strcmp(t, "quiet")) {                       /* quiet [0|1]：注入族回包开关（默认 0 = 回 ok，老客户端不变） */
        char *sv = strtok_r(NULL, " \t", stp);
        long v = 1;
        if (sv) { char *e = NULL; v = strtol(sv, &e, 10); if (e == sv || *e || (v != 0 && v != 1)) { snprintf(resp, cap, "err quiet"); return -1; } }
        if (strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err quiet"); return -1; }
        g.quiet = (int)v; snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "res")) {
        snprintf(resp, cap, "res %d %d raw %d %d %d %d phys %d",
                 g.logical_width, g.logical_height,
                 g.axmin[0], g.axmax[0], g.axmin[1], g.axmax[1],
                 g.phys_slots);
        return 0;
    }
    if (!strcmp(t, "reset")) {
        if (g.frame_open) { snprintf(resp, cap, "err frame"); return -1; }
        owner_reset(); snprintf(resp, cap, "ok"); return 0;
    }
    return 1;
}
/**
 * (vtouch-doc: ack_ok)
 * @brief 注入族的回包：默认 "ok"；quiet 模式留空（resp[0]=0 ⇒ client_frame 不发帧）。
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @note    单条注入命令一次 ok，在脚本侧要读一帧 + 剥前缀 + 丢弃 ⇒ 注入风暴时每秒几千行纯浪费。
 */
static void ack_ok(char *resp, size_t cap)
{
    if (g.quiet) { resp[0] = 0; return; }
    snprintf(resp, cap, "ok");
}
/**
 * (vtouch-doc: cmd_point_once)
 * @brief 命令族：up / down / move —— 单点命令，每个命令提交一帧。
 * @param   t        命令词
 * @param   stp      strtok_r 状态
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @return  1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   up / down / move：单点命令，每个命令提交一帧
 */
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
        ack_ok(resp, cap); return 0;
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
        ack_ok(resp, cap); return 0;
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
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   begin_frame / point / end_frame：帧内多点，一次 SYN 提交
 */
int cmd_frame(char *t, char **stp, char *resp, size_t cap)
{
    int slot, x, y;
    if (!strcmp(t, "begin_frame")) {
        if (g.frame_open || strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err frame"); return -1; }
        memcpy(g.staged, g.virt, sizeof g.staged);
        g.frame_open = 1; memset(g.frame_seen, 0, sizeof g.frame_seen);
        ack_ok(resp, cap); return 0;
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
        g.frame_seen[slot] = 1; ack_ok(resp, cap); return 0;
    }
    if (!strcmp(t, "end_frame")) {
        if (!g.frame_open || strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err frame"); return -1; }
        memcpy(g.virt, g.staged, sizeof g.virt);
        if (emit_frame() < 0) { g.frame_open = 0; snprintf(resp, cap, "err frame"); return -1; }
        g.frame_open = 0; ack_ok(resp, cap); return 0;
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
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   region add|del|clear|list：主线程只写表（短锁），判定全在区域线程（§4.4/§4.6）
 */
int cmd_region(char *t, char **stp, char *resp, size_t cap)
{
    char *op;
    if (strcmp(t, "region")) return 1;
    op = strtok_r(NULL, " \t", stp);
    if (op && !strcmp(op, "clear")) {
        if (strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err region"); return -1; }
        regions_clear(); snprintf(resp, cap, "ok %d", g.region_count); return 0;
    }
    if (op && !strcmp(op, "mark")) {           /* region mark <id> <0|1>：脚本置"开关样式"，面板照着高亮 */
        char *sid = strtok_r(NULL, " \t", stp), *sv = strtok_r(NULL, " \t", stp), *e = NULL;
        long v;
        int i, hit = -1;
        if (!sid || !sv || strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err region"); return -1; }
        v = strtol(sv, &e, 10);
        if (e == sv || *e || (v != 0 && v != 1)) { snprintf(resp, cap, "err region"); return -1; }
        pthread_mutex_lock(&g.region_lock);
        for (i = 0; i < g.region_count; i++)
            if (!strcmp(g.regions[i].id, sid)) { hit = i; break; }
        if (hit >= 0) g.regions[hit].mark = (int)v;
        pthread_mutex_unlock(&g.region_lock);
        if (hit < 0) { snprintf(resp, cap, "err region"); return -1; }   /* 没这个 id 就明确报错，不静默 */
        fprintf(stderr, "vtouchd: region mark %s %d\n", sid, (int)v);
#ifdef VT_UI
        /* 面板是**按需重绘**的：核心改了标记，它没有醒来的理由（脚本在别的回调里关开关时，
         * 屏幕上不会有人碰它）。所以往事件环推一条，面板收到就立刻重画一帧（≤33ms 节流）。 */
        {
            char mb[64];
            int mn = snprintf(mb, sizeof mb, "mark_ev %s %d\n", sid, (int)v);
            if (mn > 0 && (size_t)mn < sizeof mb) vt_shm_ring_push(mb, (size_t)mn);
        }
#endif
        snprintf(resp, cap, "ok"); return 0;
    }
    if (op && !strcmp(op, "list")) {
        char line[128];
        char rows[MAX_REGIONS][128];            /* 锁内只**格式化**到这里，解锁后再逐条入队（见下） */
        int i, n, nrow = 0, w, ndrop = 0;
        if (strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err region"); return -1; }
        /* ── B3 + I2：一条区域一帧，且**锁内只格式化** ────────────────────
         * 以前把整表拼进一个 resp（上限 MAX_LINE=1024）当**一个帧**发：最坏 32 条 × ~50B = 1600
         * ⇒ ~20 条以上开始丢表尾（连末行 end N 一起没了，客户端只能等到超时）。现在每条
         * 各一次入队（自己组 WS 文本帧），末行 end N 也单独一帧 —— 行格式逐字不变。
         * resp[0] = 0 表示“本族已经自己发过了”，client_frame 据此不再重发（见那里的 if）。
         *
         * 但「逐条入队」不能放在 region_lock 里：持 region_lock 抢 outq 锁会把这条 I/O 入锁路径
         * 带回来（B2 刚把 I/O 移出锁），而且区域线程的 region_ev 入队要排在 poll 线程这 N+1 次
         * push 后面。所以锁内**只格式化**每行到本地 rows[]（nrow < MAX_REGIONS 边界保护），
         * 解锁后逐条入队 —— 与 B2 同款：锁内只碰区域表，I/O 一律在锁外。
         *
         * 入队走 outq_push_text_keep（**队满就不写这一帧**，不是事件帧那条「丢最旧」）：
         * outq 只有 OUTQ_CAP=64 格、满了丢最旧（src/vt_queue.c）本身是「事件保新鲜」的有意设计，
         * 但一张**带自证末行**的表不能这么丢 —— 丢最旧会先挤掉队列里已排队的数据（很可能是客户端
         * 还没读走的事件帧），而末行 end N 照发 ⇒ 客户端拿到「少几行却自称 N 条」的半张表，被挤掉的
         * 事件帧还是静默丢的（I2 的另一半）。现在丢的是这一帧本身：表可能不完整，但客户端能靠行数与
         * end N 对账报出「收到 X 行，表里声明 N 条」；若队列到末行时仍然满（队满后再无空间可腾，本命令
         * 期间出站队列不会排空），末行自己也会被丢 ⇒ 客户端走它既有的「没见过末行 → 回包不完整（超时或
         * 被截断）」判据 —— 两条路都会报，不会静默接受半张表。且一条已排队的事件帧都不会被这次推表挤掉。
         * 被丢的行数只在**锁外**打一条 stderr 日志（不逐行打）。 */
        resp[0] = 0;
        pthread_mutex_lock(&g.region_lock);
        n = g.region_count;
        for (i = 0; i < n; i++) {              /* 锁内只读表 + 格式化到本地缓冲（不碰 outq/stderr）*/
            if (nrow >= MAX_REGIONS) break;    /* 边界保护：表最多 MAX_REGIONS 条（不该发生） */
            w = snprintf(rows[nrow], sizeof rows[0], "region %s %d %d %d %d %d %d\n", g.regions[i].id,
                         g.regions[i].type, g.regions[i].a1, g.regions[i].a2, g.regions[i].a3, g.regions[i].a4,
                         g.regions[i].enabled);
            if (w <= 0 || (size_t)w >= sizeof rows[0]) continue;   /* 单行放不下（不该发生）→ 跳过该条，不越界 */
            nrow++;
        }
        pthread_mutex_unlock(&g.region_lock);
        for (i = 0; i < nrow; i++)             /* 解锁后逐条入队（此时才碰 outq 锁）；满了就丢这一帧 */
            if (outq_push_text_keep(rows[i], strlen(rows[i])) != 0) ndrop++;
        w = snprintf(line, sizeof line, "end %d", n);
        if (w > 0 && (size_t)w < sizeof line &&                /* 末行单独一帧：内容/口径不变，同样走 keep */
            outq_push_text_keep(line, (size_t)w) != 0) ndrop++;   /* 它也可能入不了队 → 计进被丢行数 */
        if (ndrop)                             /* 一条日志，**锁外**；带被丢行数（客户端靠行数对账会报出来） */
            fprintf(stderr, "vtouchd: region list 有 %d 行因出站队列满被丢弃（客户端会报不完整）\n", ndrop);
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
 * (vtouch-doc: parse_slot_mask)
 * @brief 逗号分隔的槽号列表 → 槽位掩码（sub phys 的 <选择>）。
 * @param   s        槽号列表，如 "0" / "0,3"（原地切分）
 * @param   out      结果掩码
 * @return  0 成功；-1 语法错（非数字 / 越界 / 空）。
 * @note    用独立的 strtok_r saveptr —— 复用外层状态会把命令参数切坏。
 */
static int parse_slot_mask(char *s, unsigned *out)
{
    char *sv = NULL, *tk;
    unsigned m = 0;
    for (tk = strtok_r(s, ",", &sv); tk; tk = strtok_r(NULL, ",", &sv)) {
        char *end = NULL;
        long v = strtol(tk, &end, 10);
        if (end == tk || *end != 0 || v < 0 || v >= 32) return -1;
        m |= (1u << (unsigned)v);
    }
    if (m == 0) return -1;
    *out = m;
    return 0;
}
/**
 * (vtouch-doc: parse_ev_bits)
 * @brief 逗号分隔的事件名列表（或 *）→ SUBEV_* 位（sub 的 <事件>）。
 * @param   s        事件名列表，如 "down,up"；"*" = 全部
 * @param   out      结果位；* 存 0（= 未设 = 全通）
 * @return  0 成功；-1 语法错（含未知事件名）。
 * @note    * 与「<选择> 缺省」共用「0 = 全通」这一约定。
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   逗号分隔的事件名列表 → SUBEV_* 位；`*` 表示全部（存 0 = 未设 = 全通）。
 */
static int parse_ev_bits(char *s, unsigned *out)
{
    char *sv = NULL, *tk;
    unsigned e = 0;
    if (!strcmp(s, "*")) { *out = 0; return 0; }
    for (tk = strtok_r(s, ",", &sv); tk; tk = strtok_r(NULL, ",", &sv)) {
        unsigned b = vt_subev_bit(tk);   /* ts 是伪事件（SUBEV_TS），只影响线路格式，不参与事件过滤 */
        if (b == 0) return -1;
        e |= b;
    }
    if (e == 0) return -1;
    *out = e;
    return 0;
}
/**
 * (vtouch-doc: cmd_sub)
 * @brief 命令族：sub [phys|region|all] [<选择> [<事件>]] / unsub [phys|region]（不带选择 = 老语义全订；带选择 = 精确订阅过滤器）。
 * @param   t        命令词
 * @param   stp      strtok_r 状态
 * @param   resp     响应缓冲
 * @param   cap      缓冲容量
 * @return  1 不是本族命令；0 / -1 = 已处理（-1 时 resp 是错误响应）。
 * @note    <选择>：phys 是槽号列表（0 / 0,3 / * / -1 = 全部槽），region 是区域 id（* = 全部）。<事件>：逗号列表或 *（down,enter,move,exit,up）。给了 <选择> 而不给 <事件> 时默认「简报」——phys: down,up；region: down,up,enter,exit，**默认不含 move**；要位置流必须显式写 move（实测物理行占 97% 的量，默认开它等于白烧 CPU）。过滤器只作用于推送，不影响 stderr 日志。
 */
int cmd_sub(char *t, char **stp, char *resp, size_t cap)
{
    if (!strcmp(t, "sub")) {
        char *ch = strtok_r(NULL, " \t", stp), *sel, *evs;
        int want = SUB_REGION;                       /* 裸 sub = 区域通道（与改动前一致，老脚本行为不变） */
        unsigned m = 0, e = 0;
        if (ch) {
            if (!strcmp(ch, "region"))      want = SUB_REGION;
            else if (!strcmp(ch, "phys"))   want = SUB_PHYS;
            else if (!strcmp(ch, "all"))    want = SUB_PHYS | SUB_REGION;
            else { snprintf(resp, cap, "err sub"); return -1; }
        }
        sel = strtok_r(NULL, " \t", stp);
        evs = strtok_r(NULL, " \t", stp);
        if (strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err sub"); return -1; }
        if (!ch) {   /* 裸 sub：老语义（全槽/全区域、全事件、**带 ts 与区域 id = 老线路格式**） */
            g.sub_mask |= want;
            if (want & SUB_PHYS)   { g.sub_phys_mask = 0;   g.sub_phys_ev = 0;   g.sub_phys_ts = 1; }
            if (want & SUB_REGION) { g.sub_region_id[0] = 0; g.sub_region_ev = 0; g.sub_region_ts = 1; }
            snprintf(resp, cap, "ok"); return 0;
        }
        /* 带 <选择> ⇒ 精确设定过滤器（不再 |=，两次带参 sub 不应互相污染过滤器）。
         * 事件缺省 = 简报（phys: down,up；region: down,up,enter,exit）—— **默认不含 move**，
         * 要位置流必须显式写 move（物理行占实测 97% 的量）。 */
        if (want == SUB_PHYS) {
            if (sel && strcmp(sel, "*") && strcmp(sel, "-1") && parse_slot_mask(sel, &m) != 0) {
                snprintf(resp, cap, "err sub"); return -1;
            }
            if (evs) { if (parse_ev_bits(evs, &e) != 0) { snprintf(resp, cap, "err sub"); return -1; } }
            else e = sel ? (SUBEV_DOWN | SUBEV_UP) : 0;
            g.sub_phys_mask = m; g.sub_phys_ev = e;
            /* 时间戳**默认带上**（脚本要靠它算按压时长/送达延迟）；只有显式写 nots 才省掉。 */
            g.sub_phys_ts = (e & SUBEV_NOTS) ? 0 : 1;
            fprintf(stderr, "vtouchd: 订阅 phys 槽=%s ev=%s ts=%d 线路=phys_ev <ev> <slot> <x> <y>%s\n",
                    m ? (sel ? sel : "0") : "全部", evs ? evs : "(默认)", g.sub_phys_ts, g.sub_phys_ts ? " <t>" : "");
        } else if (want == SUB_REGION) {
            if (sel && strcmp(sel, "*")) {
                if (strlen(sel) > REGION_ID_MAX) { snprintf(resp, cap, "err sub"); return -1; }
                snprintf(g.sub_region_id, sizeof g.sub_region_id, "%s", sel);
            } else g.sub_region_id[0] = 0;
            if (evs) { if (parse_ev_bits(evs, &e) != 0) { snprintf(resp, cap, "err sub"); return -1; } }
            else e = sel ? (SUBEV_DOWN | SUBEV_ENTER | SUBEV_EXIT | SUBEV_UP) : 0;
            g.sub_region_ev = e;
            g.sub_region_ts = (e & SUBEV_NOTS) ? 0 : 1;      /* 同上：默认带时间戳 */
            /* 线路格式写进日志，省得对着抓包猜（只订一个区域 ⇒ 不再重复发 id） */
            fprintf(stderr, "vtouchd: 订阅 region id=%s ev=%s ts=%d 线路=<%s> <ev> <slot> <x> <y>%s\n",
                    g.sub_region_id[0] ? g.sub_region_id : "*", evs ? evs : "(默认)", g.sub_region_ts,
                    g.sub_region_id[0] ? "(id省略)" : "id", g.sub_region_ts ? " <t>" : "");
        } else { snprintf(resp, cap, "err sub"); return -1; }   /* sub all 不支持过滤器（语义歧义） */
        g.sub_mask |= want; snprintf(resp, cap, "ok"); return 0;
    }
    if (!strcmp(t, "unsub")) {
        char *ch = strtok_r(NULL, " \t", stp);
        if (strtok_r(NULL, " \t", stp)) { snprintf(resp, cap, "err sub"); return -1; }
        if (!ch) {                                   /* 不带参数 = 全退（老语义不变） */
            g.sub_mask = 0; g.sub_phys_mask = g.sub_phys_ev = 0;
            g.sub_region_id[0] = 0; g.sub_region_ev = 0;
        } else if (!strcmp(ch, "phys")) {
            g.sub_mask &= ~SUB_PHYS; g.sub_phys_mask = g.sub_phys_ev = 0;
        } else if (!strcmp(ch, "region")) {
            g.sub_mask &= ~SUB_REGION; g.sub_region_id[0] = 0; g.sub_region_ev = 0;
        } else { snprintf(resp, cap, "err sub"); return -1; }
        snprintf(resp, cap, "ok"); return 0;
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
 *
 * 为什么这么写（原有注释，逐字保留）：
 *   一行命令 -> 一行回包：这里只剩分派
 */
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

