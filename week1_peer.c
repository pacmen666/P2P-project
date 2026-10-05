/* Experiment 1 only: one zero-filled 16 KiB block, session=1, file=1.
 * No Tracker integration or production manifest yet (protocol section 13).
 * The requester has test peer_id=1, listener peer_id=2, so lower ID connects.
 */
#define _POSIX_C_SOURCE 200809L
#include "fa_stream.h"
#include "common.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/select.h>

struct app {
    int listen_fd, fd, connecting, wake[2], serving, rc;
    int woke, requested, received, ack, served;
    size_t read_size, reads, partial_headers;
    double connect_deadline, deadline;
    struct fa_reader reader;
    struct fa_writer writer;
    uint8_t block[LOGIC_PIECE_SIZE], digest[SHA256_DIGEST_LENGTH];
};
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (double)t.tv_sec+t.tv_nsec/1e9;
}
static int configure(int fd) {
    int one=1;
    if(fd>=FD_SETSIZE || fa_nonblock(fd)<0 || setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one))<0) return -1;
    return 0;
}
static int message(void *ctx,const struct fa_message *m) {
    struct app *a=ctx;
    printf("MESSAGE cmd=0x%02x req=%u body=%u\n",m->cmd,m->request,m->length);
    if(a->serving && m->cmd==CMD_PIECE_REQ) {
        if(m->length!=9 || fa_u32(m->body)!=1 || m->body[4]!=1 || fa_u16(m->body+5)!=0 || fa_u16(m->body+7)!=0xffff || !m->request) return -1;
        uint8_t body[10+LOGIC_PIECE_SIZE]; memset(body,0,10);
        fa_put32(body,1); body[4]=1; fa_put16(body+8,LOGIC_PIECE_SIZE);
        memcpy(body+10,a->block,LOGIC_PIECE_SIZE);
        if(fa_enqueue(&a->writer,CMD_PIECE_DATA,m->request,body,sizeof(body))<0) return -1;
        a->served=1; return 0;
    }
    if(m->cmd==CMD_HEARTBEAT && m->length==0) return fa_enqueue(&a->writer,CMD_HEARTBEAT_ACK,0,NULL,0);
    if(!a->serving && m->cmd==CMD_HEARTBEAT_ACK && m->length==0 && m->request==0) { a->ack=1; return 0; }
    if(!a->serving && m->cmd==CMD_PIECE_DATA) {
        if(!a->requested || a->received || m->request!=1 || m->length!=10+LOGIC_PIECE_SIZE ||
           fa_u32(m->body)!=1 || m->body[4]!=1 || m->body[5]!=0 ||
           fa_u16(m->body+6)!=0 || fa_u16(m->body+8)!=LOGIC_PIECE_SIZE) return -1;
        uint8_t got[SHA256_DIGEST_LENGTH]; SHA256(m->body+10,LOGIC_PIECE_SIZE,got);
        printf("SHA256="); for(size_t i=0;i<sizeof(got);i++) printf("%02x",got[i]); puts("");
        if(memcmp(got,a->digest,sizeof(got))) { fputs("HASH_MISMATCH\n",stderr); return -1; }
        a->received=1; puts("BLOCK_OK bytes=16384 session=1 file=1 piece=0"); return 0;
    }
    return -1;
}
static int request(struct app *a) {
    uint8_t body[9]; fa_put32(body,1); body[4]=1; fa_put16(body+5,0); fa_put16(body+7,0xffff);
    if(fa_enqueue(&a->writer,CMD_PIECE_REQ,1,body,sizeof(body))<0 ||
       fa_enqueue(&a->writer,CMD_HEARTBEAT,0,NULL,0)<0) return -1;
    /* Both complete messages go into one output queue before flush. */
    a->requested=1; a->deadline=now()+TCP_PIECE_TIMEOUT_SEC;
    puts("REQUEST_QUEUED with HEARTBEAT; two adjacent frames"); return 0;
}
static void *network(void *ctx) {
    struct app *a=ctx; a->rc=1;
    for(;;) {
        double t=now();
        if(t>=a->deadline || (a->connecting && t>=a->connect_deadline)) { fputs("TIMEOUT\n",stderr); break; }
        if(a->woke && !a->serving && a->fd>=0 && !a->connecting && !a->requested)
            if(request(a)<0) break;
        if(!a->serving && a->received && a->ack) { a->rc=0; break; }
        fd_set rd,wr; FD_ZERO(&rd); FD_ZERO(&wr);
        int max=a->wake[0]; FD_SET(a->wake[0],&rd);
        if(a->listen_fd>=0) { FD_SET(a->listen_fd,&rd); if(a->listen_fd>max) max=a->listen_fd; }
        if(a->fd>=0) {
            if(!a->connecting) FD_SET(a->fd,&rd);
            if(a->connecting || a->writer.end>a->writer.offset) FD_SET(a->fd,&wr);
            if(a->fd>max) max=a->fd;
        }
        double left=a->deadline-t;
        if(a->connecting && a->connect_deadline-t<left) left=a->connect_deadline-t;
        struct timeval tv={(long)left,(long)((left-(long)left)*1e6)};
        int n=select(max+1,&rd,&wr,NULL,&tv);
        if(n<0) { if(errno==EINTR) continue; perror("select"); break; }
        if(FD_ISSET(a->wake[0],&rd)) {
            if(fa_wake_drain(a->wake[0])<0) break;
            a->woke=1; puts("WAKE_OK: main thread -> pipe -> network select");
        }
        if(a->listen_fd>=0 && FD_ISSET(a->listen_fd,&rd)) {
            int fd=accept(a->listen_fd,NULL,NULL);
            if(fd<0) { if(errno==EAGAIN || errno==EINTR) continue; perror("accept"); break; }
            if(configure(fd)<0) { close(fd); break; }
            a->fd=fd; close(a->listen_fd); a->listen_fd=-1;
            puts("CONNECTED peer_id=1 SO_KEEPALIVE=1");
            /* New fd was not in select's original sets. */
            continue;
        }
        if(a->fd>=0 && FD_ISSET(a->fd,&wr)) {
            if(a->connecting) {
                int error=0; socklen_t len=sizeof(error);
                if(getsockopt(a->fd,SOL_SOCKET,SO_ERROR,&error,&len)<0 || error) { fprintf(stderr,"CONNECT_FAILED error=%d\n",error); break; }
                a->connecting=0; puts("CONNECTED peer_id=2 SO_KEEPALIVE=1");
            }
            if(fa_flush(&a->writer,a->fd)<0) { perror("send"); break; }
        }
        if(a->fd>=0 && !a->connecting && FD_ISSET(a->fd,&rd)) {
            uint8_t buf[4096]; ssize_t k=recv(a->fd,buf,a->read_size,0);
            if(k<0) { if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR) continue; perror("recv"); break; }
            if(k==0) {
                if(fa_reader_eof(&a->reader)<0) fputs("TRUNCATED_FRAME\n",stderr);
                else if(a->serving && a->served && a->writer.offset==a->writer.end) a->rc=0;
                else fputs("PEER_CLOSED_BEFORE_COMPLETION\n",stderr);
                break;
            }
            a->reads++;
            if(fa_feed(&a->reader,buf,(size_t)k,message,a)<0) { fputs("INVALID_FRAME_OR_BODY\n",stderr); break; }
            if(a->reader.header_got>0 && a->reader.header_got<FA_HEADER) {
                a->partial_headers++;
                if(a->partial_headers<=3) printf("READ_HEADER waiting: %zu/8 bytes retained\n",a->reader.header_got);
            }
        }
    }
    printf("SUMMARY reads=%zu partial_headers=%zu result=%s\n",a->reads,a->partial_headers,a->rc?"FAIL":"PASS");
    return NULL;
}
static long number(const char *s,long low,long high) {
    char *end; errno=0; long v=strtol(s,&end,10);
    if(errno || !*s || *end || v<low || v>high) return -1;
    return v;
}
int main(int argc,char **argv) {
    if(argc<4 || argc>5 || (strcmp(argv[1],"serve") && strcmp(argv[1],"get"))) {
        fprintf(stderr,"usage: %s serve|get IPv4 PORT [recv-size:1..4096]\n",argv[0]); return 2;
    }
    long port=number(argv[3],1,65535), chunk=argc==5?number(argv[4],1,4096):3;
    if(port<0 || chunk<0) return 2;
    struct app *a=calloc(1,sizeof(*a)); if(!a) return 1;
    a->fd=a->listen_fd=-1; a->serving=!strcmp(argv[1],"serve"); a->read_size=(size_t)chunk;
    a->deadline=now()+20; SHA256(a->block,sizeof(a->block),a->digest);
    setvbuf(stdout,NULL,_IONBF,0); signal(SIGPIPE,SIG_IGN);
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr)); addr.sin_family=AF_INET; addr.sin_port=htons((uint16_t)port);
    if(inet_pton(AF_INET,argv[2],&addr.sin_addr)!=1 || fa_wake_open(a->wake)<0 || a->wake[0]>=FD_SETSIZE) { free(a); return 1; }
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0 || configure(fd)<0) { perror("socket/configure"); return 1; }
    if(a->serving) {
        int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        if(bind(fd,(struct sockaddr *)&addr,sizeof(addr))<0 || listen(fd,4)<0) { perror("bind/listen"); return 1; }
        a->listen_fd=fd; printf("LISTEN %s:%ld test_peer_id=2\n",argv[2],port);
    } else {
        a->fd=fd; int rc=connect(fd,(struct sockaddr *)&addr,sizeof(addr));
        if(rc<0 && errno!=EINPROGRESS) { perror("connect"); return 1; }
        a->connecting=rc<0; a->connect_deadline=now()+1.5;
    }
    pthread_t worker;
    if(pthread_create(&worker,NULL,network,a)) return 1;
    /* Simulates one UI command; main does not access network sockets. */
    struct timespec delay={0,100000000}; nanosleep(&delay,NULL);
    if(fa_wake_signal(a->wake[1])<0) fputs("wake signal failed\n",stderr);
    pthread_join(worker,NULL);
    int rc=a->rc;
    if(a->fd>=0) close(a->fd);
    if(a->listen_fd>=0) close(a->listen_fd);
    close(a->wake[0]); close(a->wake[1]); free(a); return rc;
}
