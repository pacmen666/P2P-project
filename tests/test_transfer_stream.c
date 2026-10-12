/* Reuses the original Week 1 stream regression cases with canonical include/src paths. */
#include "fa_stream.h"
#include "common.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>

struct expected { size_t count, len; uint8_t cmd; uint16_t req; const uint8_t *body; };
static int check(void *ctx,const struct fa_message *m) {
    struct expected *e=ctx;
    assert(m->cmd==e->cmd && m->request==e->req && m->length==e->len);
    assert(!m->length || !memcmp(m->body,e->body,m->length)); e->count++; return 0;
}
int main(void) {
    struct fa_reader *r=malloc(sizeof(*r)); assert(r);
    uint8_t body[19]; for(size_t i=0;i<sizeof(body);i++) body[i]=(uint8_t)i;
    uint8_t frame[27]; assert(fa_pack(frame,sizeof(frame),CMD_PIECE_DATA,0x1234,body,sizeof(body))==27);
    const uint8_t head[]={0x46,0x41,1,0x0b,0x12,0x34,0,19}; assert(!memcmp(frame,head,8));
    puts("PASS network byte order: 46 41 01 0b 12 34 00 13");
    for(size_t split=0;split<=sizeof(frame);split++) {
        struct expected e={0,sizeof(body),CMD_PIECE_DATA,0x1234,body}; fa_reader_init(r);
        assert(fa_feed(r,frame,split,check,&e)==0);
        assert(fa_feed(r,frame+split,sizeof(frame)-split,check,&e)==0);
        assert(e.count==1 && fa_reader_eof(r)==0);
    }
    struct expected e={0,sizeof(body),CMD_PIECE_DATA,0x1234,body}; fa_reader_init(r);
    for(size_t i=0;i<sizeof(frame);i++) assert(fa_feed(r,frame+i,1,check,&e)==0);
    assert(e.count==1); puts("PASS all 28 split positions and byte-by-byte input");
    uint8_t joined[54]; memcpy(joined,frame,27); memcpy(joined+27,frame,27);
    e.count=0; fa_reader_init(r); assert(fa_feed(r,joined,54,check,&e)==0 && e.count==2);
    puts("PASS two frames supplied in one read");
    uint8_t empty[8]; assert(fa_pack(empty,8,CMD_HEARTBEAT,0,NULL,0)==8);
    struct expected z={0,0,CMD_HEARTBEAT,0,NULL}; fa_reader_init(r);
    assert(fa_feed(r,empty,8,check,&z)==0 && z.count==1); puts("PASS empty-body frame");
    for(size_t cut=1;cut<sizeof(frame);cut++) {
        fa_reader_init(r); e.count=0; assert(fa_feed(r,frame,cut,check,&e)==0);
        assert(fa_reader_eof(r)==-1 && e.count==0);
    }
    puts("PASS truncated header/body rejected at EOF");
    for(int which=0;which<3;which++) {
        uint8_t bad[8]; memcpy(bad,empty,8); bad[which==0?0:which==1?2:3]=0xff;
        fa_reader_init(r); assert(fa_feed(r,bad,8,check,&z)==-1);
        assert(fa_feed(r,empty,8,check,&z)==-1);
    }
    puts("PASS bad magic, version and command rejected; reader stays failed");
    uint8_t *large=malloc(65543), *payload=malloc(65535); assert(large && payload);
    memset(payload,0xa5,65535); assert(fa_pack(large,65543,CMD_MANIFEST,7,payload,65535)==65543);
    struct expected big={0,65535,CMD_MANIFEST,7,payload}; fa_reader_init(r);
    assert(fa_feed(r,large,65543,check,&big)==0 && big.count==1);
    assert(fa_pack(large,65543,CMD_MANIFEST,7,payload,65536)==-1);
    assert(fa_pack(frame,7,CMD_HEARTBEAT,0,NULL,0)==-1);
    puts("PASS maximum body length and output bounds");
    int sv[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0);
    assert(fa_nonblock(sv[0])==0 && fa_nonblock(sv[1])==0);
    int small=1024; assert(setsockopt(sv[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
    struct fa_writer *w=calloc(1,sizeof(*w)); assert(w);
    for(int i=0;i<4;i++) assert(fa_enqueue(w,CMD_MANIFEST,7,payload,65535)==0);
    assert(fa_enqueue(w,CMD_HEARTBEAT,0,NULL,0)==-1);
    assert(fa_flush(w,sv[0])==0 && w->offset>0 && w->offset<w->end);
    fa_reader_init(r); big.count=0;
    for(int attempt=0;attempt<10000 && (w->end || big.count<4);attempt++) {
        uint8_t buf[777]; ssize_t n;
        while((n=recv(sv[1],buf,sizeof(buf),0))>0) assert(fa_feed(r,buf,(size_t)n,check,&big)==0);
        assert(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
        assert(fa_flush(w,sv[0])>=0);
    }
    assert(big.count==4 && !w->end && fa_reader_eof(r)==0);
    close(sv[0]); close(sv[1]); free(w); puts("PASS short writes, EAGAIN, bounded queue, exact-byte delivery");
    int p[2]; assert(fa_wake_open(p)==0); assert(fa_wake_signal(p[1])==0);
    fd_set rd; FD_ZERO(&rd); FD_SET(p[0],&rd); struct timeval tv={0,100000};
    assert(select(p[0]+1,&rd,NULL,NULL,&tv)==1); assert(fa_wake_drain(p[0])==1);
    for(int i=0;i<100000;i++) assert(fa_wake_signal(p[1])==0);
    assert(fa_wake_drain(p[0])>0); assert(fa_wake_drain(p[0])==0);
    close(p[0]); close(p[1]); puts("PASS pipe wake, drain and full-pipe coalescing");
    free(large); free(payload); free(r); puts("ALL STREAM TESTS PASS"); return 0;
}
