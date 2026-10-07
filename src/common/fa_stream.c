#include "fa_stream.h"
#include "common.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

uint16_t fa_u16(const uint8_t *p) { uint16_t v; memcpy(&v,p,2); return ntohs(v); }
uint32_t fa_u32(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return ntohl(v); }
void fa_put16(uint8_t *p,uint16_t v) { v=htons(v); memcpy(p,&v,2); }
void fa_put32(uint8_t *p,uint32_t v) { v=htonl(v); memcpy(p,&v,4); }
static int valid_cmd(uint8_t cmd) { return cmd>=CMD_JOIN_REQ && cmd<=CMD_MANIFEST_REQ; }
void fa_reader_init(struct fa_reader *r) { memset(r,0,sizeof(*r)); }
int fa_reader_eof(const struct fa_reader *r) {
    return r->failed || r->header_got || r->body_got ? -1 : 0;
}
int fa_feed(struct fa_reader *r,const void *data,size_t n,fa_handler cb,void *ctx) {
    const uint8_t *p=data;
    if(r->failed || (!p && n) || !cb) return -1;
    while(n) {
        if(r->header_got<FA_HEADER) {
            size_t k=FA_HEADER-r->header_got; if(k>n) k=n;
            memcpy(r->header+r->header_got,p,k); r->header_got+=k; p+=k; n-=k;
            if(r->header_got<FA_HEADER) continue;
            if(fa_u16(r->header)!=V1_MAGIC || r->header[2]!=V1_VERSION || !valid_cmd(r->header[3])) {
                r->failed=1; return -1;
            }
            r->body_len=fa_u16(r->header+6);
        }
        size_t k=r->body_len-r->body_got; if(k>n) k=n;
        if(k) { memcpy(r->body+r->body_got,p,k); r->body_got+=k; p+=k; n-=k; }
        if(r->body_got==r->body_len) {
            struct fa_message m={r->header[3],fa_u16(r->header+4),r->body_len,r->body};
            if(cb(ctx,&m)<0) { r->failed=1; return -1; }
            r->header_got=r->body_got=0; r->body_len=0;
        }
    }
    return 0;
}
int fa_pack(uint8_t *dst,size_t cap,uint8_t cmd,uint16_t request,const void *body,size_t len) {
    if(!dst || len>FA_BODY_MAX || cap<FA_HEADER+len || (!body && len) || !valid_cmd(cmd)) return -1;
    fa_put16(dst,V1_MAGIC); dst[2]=V1_VERSION; dst[3]=cmd;
    fa_put16(dst+4,request); fa_put16(dst+6,(uint16_t)len);
    if(len) memcpy(dst+FA_HEADER,body,len);
    return (int)(FA_HEADER+len);
}
int fa_enqueue(struct fa_writer *w,uint8_t cmd,uint16_t req,const void *body,size_t len) {
    if(w->offset>w->end || w->end>FA_TX_CAP || len>FA_BODY_MAX) return -1;
    if(FA_TX_CAP-w->end<FA_HEADER+len && w->offset) {
        memmove(w->bytes,w->bytes+w->offset,w->end-w->offset);
        w->end-=w->offset; w->offset=0;
    }
    int n=fa_pack(w->bytes+w->end,FA_TX_CAP-w->end,cmd,req,body,len);
    if(n<0) return -1;
    w->end+=(size_t)n; return 0;
}
int fa_flush(struct fa_writer *w,int fd) {
    while(w->offset<w->end) {
        int flags=0;
#ifdef MSG_NOSIGNAL
        flags=MSG_NOSIGNAL;
#endif
        ssize_t n=send(fd,w->bytes+w->offset,w->end-w->offset,flags);
        if(n>0) { w->offset+=(size_t)n; continue; }
        if(n<0 && errno==EINTR) continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
        return -1;
    }
    w->offset=w->end=0; return 1;
}
int fa_nonblock(int fd) {
    int f=fcntl(fd,F_GETFL,0); return f<0 ? -1 : fcntl(fd,F_SETFL,f|O_NONBLOCK);
}
int fa_wake_open(int p[2]) {
    if(pipe(p)<0) return -1;
    if(fa_nonblock(p[0])<0 || fa_nonblock(p[1])<0) { close(p[0]); close(p[1]); return -1; }
    return 0;
}
int fa_wake_signal(int fd) {
    uint8_t b=1; ssize_t n;
    do { n=write(fd,&b,1); } while(n<0 && errno==EINTR);
    return n==1 || (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) ? 0 : -1;
}
int fa_wake_drain(int fd) {
    uint8_t b[128]; int total=0;
    for(;;) {
        ssize_t n=read(fd,b,sizeof(b));
        if(n>0) { total+=(int)n; continue; }
        if(n<0 && errno==EINTR) continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return total;
        return -1;
    }
}
