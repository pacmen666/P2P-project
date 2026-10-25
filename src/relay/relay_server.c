/* Formal relay entry (Wang), integrated with the existing fa_stream transport.
 * Keeps room=1 membership and RELAY_WRAP forwarding; shares Peng's tested
 * incremental reader / bounded nonblocking queue instead of two receive buffers.
 * The legacy relay_forward.c is retained only as a historical reference.
 */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "fa_stream.h"
#include "log.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/select.h>
#define SLOTS 16
struct relay_conn { int fd,id,closing; double deadline; struct fa_reader rx; struct fa_writer tx; };
static struct relay_conn peers[SLOTS];
static uint64_t evidence_queued;
static double now(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec/1e9;}
static void close_peer(struct relay_conn *c){if(c->fd>=0)close(c->fd);memset(c,0,sizeof(*c));c->fd=-1;}
static int registered(int id){
    if(id<1 || id>4)return 0;
    FILE *f=fopen("/tmp/room_peers.txt","r");if(!f)return 0;int n,found=0;
    while(fscanf(f,"%d",&n)==1)if(n==id)found=1;
    fclose(f);return found;
}
static int error(struct relay_conn *c,uint16_t req,uint16_t code,int fatal){
    unsigned char b[2];fa_put16(b,code);int rc=fa_enqueue(&c->tx,CMD_ERROR,req,b,2);
    if(fatal){c->closing=1;c->deadline=now()+2;}return rc;
}
static int message(void *ctx,const struct fa_message *m){
    struct relay_conn *c=ctx;if(c->closing)return 0;
    if(m->cmd==CMD_RELAY_BIND){
        if(m->length!=3)return error(c,m->request,ERR_BODY_LEN,1);
        int id=m->body[0];if(fa_u16(m->body+1)!=1 || !registered(id))return error(c,m->request,ERR_RELAY_ID,1);
        if(c->id)return error(c,m->request,ERR_RELAY_ID,1);
        for(int i=0;i<SLOTS;i++)if(&peers[i]!=c && peers[i].fd>=0 && peers[i].id==id)close_peer(&peers[i]);
        c->id=id;log_info("RELAY_BOUND peer=%d room=1",id);return 0;
    }
    if(m->cmd!=CMD_RELAY_WRAP || !c->id)return error(c,m->request,ERR_RELAY_ID,1);
    if(m->length<12)return error(c,m->request,ERR_WRAP_LEN,1);
    unsigned inner=fa_u16(m->body+2);
    if(inner<8 || m->length!=4+inner)return error(c,m->request,ERR_WRAP_LEN,1);
    if(m->body[1]!=c->id)return error(c,m->request,ERR_RELAY_ID,1);
    int target=m->body[0];if(target<1 || target>4 || target==c->id)return error(c,m->request,ERR_RELAY_ID,1);
    for(int i=0;i<SLOTS;i++)if(peers[i].fd>=0 && !peers[i].closing && peers[i].id==target){
        if(fa_enqueue(&peers[i].tx,CMD_RELAY_WRAP,0,m->body,m->length)<0)return error(c,m->request,ERR_RELAY_UNAVAIL,0);
        /* Count only PIECE_DATA payload accepted into the forwarding queue.
           This is NOT acknowledged delivery or unique downloaded bytes. */
        const unsigned char *packet=m->body+4;
        if(inner>=22 && fa_u16(packet)==V1_MAGIC && packet[2]==V1_VERSION &&
           packet[3]==CMD_PIECE_DATA && fa_u16(packet+6)==inner-8 &&
           fa_u16(packet+20)==inner-22)
            evidence_queued+=inner-22;
        log_info("RELAY_FORWARD src=%d dst=%d inner=%u evidence_queued=%llu",
                 c->id,target,inner,(unsigned long long)evidence_queued);return 0;
    }
    return error(c,m->request,ERR_RELAY_UNAVAIL,0);
}
int main(void){
    mkdir("logs",0700);log_init("logs/relay.log");
    signal(SIGPIPE,SIG_IGN);setvbuf(stdout,NULL,_IONBF,0);for(int i=0;i<SLOTS;i++)peers[i].fd=-1;
    int listener=socket(AF_INET,SOCK_STREAM,0),one=1;if(listener<0 || listener>=FD_SETSIZE)return 1;
    if(setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))<0 || fa_nonblock(listener)<0){perror("relay socket options");close(listener);log_close();return 1;}
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons(PORT_RELAY_TCP);
    if(bind(listener,(struct sockaddr *)&addr,sizeof(addr))<0 || listen(listener,16)<0){perror("relay listen");return 1;}
    log_info("RELAY_READY port=37405 room=1");
    for(;;){
        fd_set rd,wr;FD_ZERO(&rd);FD_ZERO(&wr);FD_SET(listener,&rd);int maxfd=listener;
        for(int i=0;i<SLOTS;i++){struct relay_conn *c=&peers[i];if(c->fd<0)continue;
            if((!c->id || c->closing) && now()>c->deadline){close_peer(c);continue;}
            if(!c->closing)FD_SET(c->fd,&rd);
            if(c->tx.end>c->tx.offset)FD_SET(c->fd,&wr);
            if(c->fd>maxfd)maxfd=c->fd;
        }
        struct timeval tv={0,100000};if(select(maxfd+1,&rd,&wr,NULL,&tv)<0){if(errno==EINTR)continue;break;}
        for(int i=0;i<SLOTS;i++){struct relay_conn *c=&peers[i];if(c->fd<0)continue;
            if(FD_ISSET(c->fd,&wr)){int rc=fa_flush(&c->tx,c->fd);if(rc<0 || (rc==1 && c->closing)){close_peer(c);continue;}}
            if(c->fd>=0 && FD_ISSET(c->fd,&rd)){
                unsigned char b[4096];ssize_t n=recv(c->fd,b,sizeof(b),0);
                if(n==0){close_peer(c);continue;}if(n<0){if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)close_peer(c);continue;}
                if(fa_feed(&c->rx,b,(size_t)n,message,c)<0)close_peer(c);
            }
        }
        if(FD_ISSET(listener,&rd)){int fd=accept(listener,NULL,NULL);if(fd>=0){int placed=0;
            if(fd<FD_SETSIZE && fa_nonblock(fd)==0)for(int i=0;i<SLOTS;i++)if(peers[i].fd<0){memset(&peers[i],0,sizeof(peers[i]));peers[i].fd=fd;peers[i].deadline=now()+5;placed=1;break;}
            if(!placed)close(fd);
        }}
    }
    for(int i=0;i<SLOTS;i++)close_peer(&peers[i]);
    close(listener);log_close();return 1;
}
