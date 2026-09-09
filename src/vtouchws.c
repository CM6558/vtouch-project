/*
 * vtouchws.c - dependency-free loopback WebSocket bridge for vtouchd.
 *
 * Listens exclusively on 127.0.0.1:27183.  A WebSocket text message is
 * treated as one or more newline-terminated vtouchd requests.  Each request
 * is sent to /data/local/tmp/vtouch.sock and its single-line response is
 * returned as a WebSocket text message.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/time.h>

#define WS_PORT 27183
#define MAX_PAYLOAD 4096
#define HTTP_MAX 8192
#define UDS_PATH "/data/local/tmp/vtouch-runtime/merge.sock"
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static volatile sig_atomic_t g_stop;
static volatile int g_current_client = -1;  /* 当前客户端 fd，用于强制断开 */

/* 客户端状态 */
struct client_state {
    int udsfd;
    size_t llen;
    unsigned char line[MAX_PAYLOAD];
};
static struct client_state g_client_state = {-1, 0, {0}};

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int read_full(int fd, void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, (char *)buf + off, len - off, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) { if (g_stop) return -1; continue; }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, (const char *)buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Small SHA-1 implementation, sufficient for Sec-WebSocket-Accept. */
struct sha1 {
    uint32_t h[5];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
};
static uint32_t rol32(uint32_t x, unsigned n) { return (x << n) | (x >> (32U - n)); }
static uint32_t be32(const unsigned char *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void sha1_block(struct sha1 *s, const unsigned char *p)
{
    uint32_t w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
    for (i = 16; i < 80; ++i) w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3]; e=s->h[4];
    for (i = 0; i < 80; ++i) {
        if (i < 20) { f=(b&c)|((~b)&d); k=0x5a827999U; }
        else if (i < 40) { f=b^c^d; k=0x6ed9eba1U; }
        else if (i < 60) { f=(b&c)|(b&d)|(c&d); k=0x8f1bbcdcU; }
        else { f=b^c^d; k=0xca62c1d6U; }
        t=rol32(a,5)+f+e+k+w[i]; e=d; d=c; c=rol32(b,30); b=a; a=t;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}
static void sha1_init(struct sha1 *s)
{
    s->h[0]=0x67452301U; s->h[1]=0xefcdab89U; s->h[2]=0x98badcfeU;
    s->h[3]=0x10325476U; s->h[4]=0xc3d2e1f0U; s->bits=0; s->used=0;
}
static void sha1_update(struct sha1 *s, const unsigned char *p, size_t n)
{
    s->bits += (uint64_t)n * 8;
    while (n) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->block + s->used, p, take); s->used += take; p += take; n -= take;
        if (s->used == 64) { sha1_block(s, s->block); s->used = 0; }
    }
}
static void sha1_final(struct sha1 *s, unsigned char out[20])
{
    unsigned char pad[128]; size_t n, i;
    uint64_t bits = s->bits;
    memset(pad, 0, sizeof(pad)); pad[0] = 0x80;
    n = (s->used < 56) ? (56 - s->used) : (120 - s->used);
    sha1_update(s, pad, n);
    for (i=0; i<8; ++i) pad[i] = (unsigned char)(bits >> (56 - i*8));
    sha1_update(s, pad, 8);
    for (i=0; i<5; ++i) { out[i*4]=(unsigned char)(s->h[i]>>24); out[i*4+1]=(unsigned char)(s->h[i]>>16); out[i*4+2]=(unsigned char)(s->h[i]>>8); out[i*4+3]=(unsigned char)s->h[i]; }
}
static int base64(const unsigned char *in, size_t n, char *out, size_t cap)
{
    static const char tab[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i=0, o=0; unsigned v;
    if (cap < ((n+2)/3)*4 + 1) return -1;
    while (i < n) {
        v=(unsigned)in[i++]<<16; if (i<n) v|=(unsigned)in[i++]<<8; if (i<n) v|=in[i++];
        out[o++]=tab[(v>>18)&63]; out[o++]=tab[(v>>12)&63];
        out[o++]=(i-1<n+0 ? tab[(v>>6)&63] : '=');
        out[o++]=(i<n ? tab[v&63] : '=');
    }
    /* The index test above needs the original length for the 1-byte case. */
    if (n % 3 == 1) { out[o-3]='='; out[o-2]='='; }
    else if (n % 3 == 2) out[o-1]='=';
    out[o]=0; return (int)o;
}

static int header_value(const char *req, const char *name, char *out, size_t cap)
{
    const char *p=req, *e, *c; size_t nl=strlen(name), n;
    while (*p) {
        e=strstr(p,"\r\n"); if (!e) break;
        c=memchr(p, ':', (size_t)(e-p));
        if (c && (size_t)(c-p)==nl && strncasecmp(p,name,nl)==0) {
            p=c+1; while (p<e && (*p==' ' || *p=='\t')) ++p;
            n=(size_t)(e-p); while (n && (p[n-1]==' ' || p[n-1]=='\t')) --n;
            if (n==0 || n+1>cap) return -1; memcpy(out,p,n); out[n]=0; return 0;
        }
        p=e+2;
    }
    return -1;
}
static int has_token(const char *s, const char *token)
{
    size_t n=strlen(token); const char *p=s;
    while (*p) { while (*p==',' || *p==' ' || *p=='\t') ++p; if (strncasecmp(p,token,n)==0 && (p[n]==0 || p[n]==',' || p[n]==' ' || p[n]=='\t')) return 1; while (*p && *p!=',') ++p; }
    return 0;
}
static int websocket_handshake(int fd)
{
    char req[HTTP_MAX], key[128], upgrade[64], connection[128], version[32], accept[64];
    unsigned char digest[20]; struct sha1 s; size_t used=0; ssize_t n; const char *guid="258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    while (used+1 < sizeof(req)) {
        n=recv(fd,req+used,1,0); if (n<=0) return -1; used+=(size_t)n; req[used]=0;
        if (used>=4 && memcmp(req+used-4,"\r\n\r\n",4)==0) break;
    }
    if (used+1>=sizeof(req) || strncmp(req,"GET ",4)!=0 || header_value(req,"Sec-WebSocket-Key",key,sizeof(key))<0 || header_value(req,"Upgrade",upgrade,sizeof(upgrade))<0 || header_value(req,"Connection",connection,sizeof(connection))<0 || header_value(req,"Sec-WebSocket-Version",version,sizeof(version))<0 || strcasecmp(upgrade,"websocket")!=0 || !has_token(connection,"Upgrade") || strcmp(version,"13")!=0 || strlen(key)!=24) return -1;
    sha1_init(&s); sha1_update(&s,(const unsigned char *)key,strlen(key)); sha1_update(&s,(const unsigned char *)guid,strlen(guid)); sha1_final(&s,digest); if (base64(digest,20,accept,sizeof(accept))<0) return -1;
    { char response[256]; int len=snprintf(response,sizeof(response),"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",accept); return (len>0 && (size_t)len<sizeof(response) && write_full(fd,response,(size_t)len)==0) ? 0 : -1; }
}
static int ws_send(int fd, unsigned opcode, const unsigned char *p, size_t n)
{
    unsigned char h[10]; size_t hn;
    if (n>MAX_PAYLOAD || (opcode>=8 && n>125)) return -1;
    h[0]=(unsigned char)(0x80 | (opcode&15)); if (n<126) { h[1]=(unsigned char)n; hn=2; }
    else { h[1]=126; h[2]=(unsigned char)(n>>8); h[3]=(unsigned char)n; hn=4; }
    return write_full(fd,h,hn) || write_full(fd,p,n);
}
static int uds_connect(void)
{
    int fd=socket(AF_UNIX,SOCK_STREAM,0); struct sockaddr_un a;
    if (fd<0) return -1; memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX; strncpy(a.sun_path,UDS_PATH,sizeof(a.sun_path)-1);
    if (connect(fd,(struct sockaddr *)&a,sizeof(a))<0) { close(fd); return -1; }
    return fd;
}
static int uds_request(int fd, const unsigned char *line, size_t len, unsigned char *reply, size_t *reply_len)
{
    size_t used=0; ssize_t n;
    if (write_full(fd,line,len)<0 || write_full(fd,"\n",1)<0) return -1;
    while (used+1<MAX_PAYLOAD) { n=recv(fd,reply+used,1,0); if (n<=0) { close(fd); return -1; } used+=(size_t)n; if (reply[used-1]=='\n') break; }
    if (used==0 || used>=MAX_PAYLOAD) return -1; if (reply[used-1]=='\n') --used; *reply_len=used; return 0;
}
static int proxy_line(int wsfd, int udsfd, unsigned char *line, size_t *line_len,
                      unsigned char *reply)
{
    size_t rlen;
    if (*line_len && line[*line_len-1]=='\r') --*line_len;
    if (uds_request(udsfd, line, *line_len, reply, &rlen)<0) return -1;
    if (ws_send(wsfd,1,reply,rlen)<0) return -1;
    *line_len=0;
    return 0;
}
static int client_loop(int fd)
{
    unsigned char hdr[2], ext[8], mask[4], payload[MAX_PAYLOAD], reply[MAX_PAYLOAD];
    size_t n, i; unsigned opcode, len7, fin, masked; uint64_t len;
    struct client_state *st = &g_client_state;
    
    /* 首次调用：连接 UDS */
    if(st->udsfd<0){
        st->udsfd=uds_connect();
        if (st->udsfd<0) return -1;
        g_current_client = fd;
        st->llen=0;
    }
    
    /* 被踢掉检查 */
    if (g_current_client != fd) { 
        if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;}
        st->llen=0;
        return -1; 
    }
    
    /* 非阻塞读取一帧 */
    if (read_full(fd,hdr,2)<0) { 
        if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;}
        g_current_client = -1; 
        st->llen=0;
        return -1; 
    }
    
    fin=hdr[0]>>7; opcode=hdr[0]&15; masked=hdr[1]>>7; len7=hdr[1]&127;
        if (!masked || !fin || (opcode!=1 && opcode!=8 && opcode!=9 && opcode!=10)) { ws_send(fd,8,(const unsigned char *)"\x03\xea",2); if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;} st->llen=0; return -1; }
        len=len7; if (len7==126) { if (read_full(fd,ext,2)<0){if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;}st->llen=0;return -1;} len=((uint64_t)ext[0]<<8)|ext[1]; }
        else if (len7==127) { if (read_full(fd,ext,8)<0){if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;}st->llen=0;return -1;} len=0; for(i=0;i<8;i++) len=(len<<8)|ext[i]; }
        if (len>MAX_PAYLOAD || (opcode>=8 && len>125)) { ws_send(fd,8,(const unsigned char *)"\x03\xef",2); if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;} st->llen=0; return -1; }
        if (read_full(fd,mask,4)<0 || read_full(fd,payload,(size_t)len)<0){if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;}st->llen=0;return -1;} for(i=0;i<(size_t)len;i++) payload[i]^=mask[i&3];
        if (opcode==8) { ws_send(fd,8,payload,(size_t)len); if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;} st->llen=0; return 0; }
        if (opcode==9) { if(ws_send(fd,10,payload,(size_t)len)<0){if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;}st->llen=0;return -1;} return 0; }
        if (opcode==10) return 0;
        for(i=0;i<(size_t)len;i++){if(payload[i]=='\n'||payload[i]=='\r')payload[i]=' ';}for(i=0;i<(size_t)len&&st->llen<MAX_PAYLOAD;i++)if(payload[i]!='\n'&&payload[i]!='\r')st->line[st->llen++]=payload[i];
        if(proxy_line(fd,st->udsfd,st->line,&st->llen,reply)<0){if(st->udsfd>=0){close(st->udsfd);st->udsfd=-1;}st->llen=0;return -1;}
    return 0;  /* 成功处理一帧，返回继续 poll */
}
int main(void)
{
    int lf,cf,opt=1,nfds,i; 
    struct sockaddr_in a; 
    struct sigaction sa;
    struct pollfd pfd[2];  /* 0=listen, 1=client */
    memset(&sa,0,sizeof(sa)); sa.sa_handler=on_signal; sigemptyset(&sa.sa_mask); sigaction(SIGTERM,&sa,0); sigaction(SIGINT,&sa,0); signal(SIGPIPE,SIG_IGN);
    lf=socket(AF_INET,SOCK_STREAM,0); if(lf<0)return 1; 
    setsockopt(lf,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt)); 
    memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_port=htons(WS_PORT); 
    if(inet_pton(AF_INET,"127.0.0.1",&a.sin_addr)!=1 || bind(lf,(struct sockaddr *)&a,sizeof(a))<0 || listen(lf,8)<0){close(lf);return 1;}
    fprintf(stderr,"vtouchws: listening t=%lld on 127.0.0.1:%d\n",now_ms(),WS_PORT);
    
    cf=-1;
    pfd[0].fd=lf; pfd[0].events=POLLIN;
    pfd[1].fd=-1; pfd[1].events=POLLIN;
    
    while(!g_stop){ 
        nfds=(cf>=0)?2:1;
        if(poll(pfd,nfds,1000)<0){if(errno==EINTR)continue;break;}
        
        /* 新连接到来 */
        if(pfd[0].revents&POLLIN){
            int new_cf=accept(lf,0,0); 
            if(new_cf>=0){
                if(cf>=0){
                    /* 踢掉旧连接 */
                    fprintf(stderr,"vtouchws: kicking old client fd=%d t=%lld\n",cf,now_ms());
                    close(cf);
                    pfd[1].fd=-1;
                }
                cf=new_cf;
                pfd[1].fd=cf;
                fprintf(stderr,"vtouchws: accepted fd=%d t=%lld\n",cf,now_ms()); fflush(stderr);
                if(websocket_handshake(cf)!=0){
                    fprintf(stderr,"vtouchws: handshake failed t=%lld\n",now_ms()); fflush(stderr);
                    close(cf); cf=-1; pfd[1].fd=-1;
                }else{
                    fprintf(stderr,"vtouchws: handshake ok t=%lld\n",now_ms()); fflush(stderr);
                    g_current_client=cf;
                }
            }
        }
        
        /* 已有客户端数据到达 */
        if(cf>=0 && (pfd[1].revents&(POLLIN|POLLHUP|POLLERR))){
            if(client_loop(cf)<0 || (pfd[1].revents&(POLLHUP|POLLERR))){
                fprintf(stderr,"vtouchws: client disconnected fd=%d t=%lld\n",cf,now_ms());
                close(cf); cf=-1; pfd[1].fd=-1; g_current_client=-1;
            }
        }
    }
    if(cf>=0)close(cf);
    close(lf); return 0;
}
