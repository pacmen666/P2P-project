/* Peng's transfer integration. Reuses Liu's UI/punch and Chen's loader.
 * All network I/O runs on one thread. Main only enqueues commands + wakes it. */
#define _POSIX_C_SOURCE 200809L
#include "transfer.h"
#include "fa_stream.h"
#include "punch.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <stdatomic.h>
#include <time.h>
#include <poll.h>
#include <sys/stat.h>

struct client {
    struct ui_state ui;
    struct punch_sock punch;
    struct transfer *tr;
    struct fa_reader reader;
    struct fa_writer writer;
    int room_fd, connecting, joined, wake[2], role, automatic, seconds;
    atomic_int stop;
    uint16_t room_id;
    uint32_t room_ip, local_ip;
    char host[256], nick[17];
    double deadline, hb, room_rx, manifest_retry, punch_due, born;
    uint32_t announced_host_ip_n;
    uint16_t announced_host_tcp_h;
    uint32_t announced_session;
    struct sockaddr_in punch_addr[5];
    int punch_seen[5];
    pthread_mutex_t queue_mu;
    char queue[32][512];unsigned head,tail;
};
static double stamp(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void choice_log_append(const char *nick,const char *choice){
    mkdir("logs",0700);
    FILE *fp=fopen("logs/choice_summary.log","a");
    if(!fp)return;
    time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);
    char ts[32];strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",&tm);
    fprintf(fp,"%s nick=%s choice=%s\n",ts,nick,choice);fclose(fp);
}
static void draw(struct client *a){ui_state_lock(&a->ui);ui_term_draw(&a->ui);ui_state_unlock(&a->ui);}
static int room_send(struct client *a,uint8_t cmd,uint16_t req,const void *body,size_t len){return a->room_fd<0?-1:fa_enqueue(&a->writer,cmd,req,body,len);}
static void room_close(struct client *a){if(a->room_fd>=0)close(a->room_fd);a->room_fd=-1;a->connecting=0;a->writer.offset=a->writer.end=0;ui_state_lock(&a->ui);a->ui.room_online=0;ui_set_status(&a->ui,"Tracker 离线；已知玩家仍可取块");ui_state_unlock(&a->ui);puts("TRACKER_OFFLINE");}
static int entry(const unsigned char *b,size_t len,size_t *off,struct ui_peer *p){
    if(*off+16>len)return -1;
    const unsigned char *v=b+*off;unsigned nl=v[15];
    if(!nl || nl>16 || *off+16+nl>len || v[0]<1 || v[0]>4 || memchr(v+16,0,nl))return -1;
    memset(p,0,sizeof(*p));p->used=1;p->peer_id=v[0];memcpy(&p->priv_ip_n,v+1,4);
    p->tcp_port_h=fa_u16(v+5);p->udp_port_h=fa_u16(v+7);memcpy(&p->refl_ip_n,v+9,4);p->refl_port_h=fa_u16(v+13);memcpy(p->nick,v+16,nl);*off+=16+nl;return 0;
}
static int members(struct client *a,const unsigned char *b,size_t len,int join){
    size_t off=join?4:1;if(len<off)return -1;unsigned count=b[join?3:0];if(count>4)return -1;
    struct ui_peer list[4];unsigned seen=0;
    for(unsigned i=0;i<count;i++){if(entry(b,len,&off,&list[i])<0 || (seen&(1u<<list[i].peer_id)))return -1;seen|=1u<<list[i].peer_id;}
    if(off!=len)return -1;
    if(join && (b[2]<1 || b[2]>4 || !(seen&(1u<<b[2]))))return -1;
    ui_state_lock(&a->ui);
    if(join){a->room_id=fa_u16(b);a->ui.room_id=a->room_id;a->ui.my_peer_id=b[2];a->ui.room_online=1;if(!a->ui.session_id)a->ui.phase=UI_PHASE_IN_ROOM;}
    /* Keep cached peers for ongoing transfer; do not erase on Tracker loss. */
    for(unsigned i=0;i<count;i++)ui_set_peer(&a->ui,&list[i]);
    unsigned mine=a->ui.my_peer_id;ui_state_unlock(&a->ui);
    tr_identity(a->tr,mine,a->room_id,a->room_ip);
    for(unsigned i=0;i<count;i++)tr_member(a->tr,&list[i]);
    return 0;
}
static int on_room(void *ctx,const struct fa_message *m){
    struct client *a=ctx;a->room_rx=stamp();
    switch(m->cmd){
    case CMD_JOIN_OK:
        if(members(a,m->body,m->length,1)<0)return -1;
        a->joined=1;
        punch_send_probe(&a->punch,a->host,a->room_ip,a->room_id,a->ui.my_peer_id);
        printf("JOINED id=%u room=%u tcp=%u udp=%u\n",a->ui.my_peer_id,a->room_id,tr_port(a->tr),a->punch.local_port);
        if(a->role==1){size_t n;const unsigned char *raw=tr_manifest_bytes(a->tr,&n);if(n)room_send(a,CMD_MANIFEST,0,raw,n);}
        break;
    case CMD_PEER_LIST:return members(a,m->body,m->length,0);
    case CMD_MANIFEST:if(tr_manifest(a->tr,m->body,m->length)<0)return -1;break;
    case CMD_HEARTBEAT_ACK:break;
    case CMD_ERROR:case CMD_JOIN_ERR:if(m->length!=2)return -1;printf("ROOM_ERROR %04x\n",fa_u16(m->body));break;
    default:break;
    }
    draw(a);return 0;
}
static int joined_socket(struct client *a){
    struct sockaddr_in local;socklen_t n=sizeof(local);if(getsockname(a->room_fd,(struct sockaddr *)&local,&n)<0)return -1;a->local_ip=local.sin_addr.s_addr;
    unsigned char b[64]={0};fa_put16(b,0);b[2]=(uint8_t)a->role;memcpy(b+3,&a->local_ip,4);fa_put16(b+7,tr_port(a->tr));fa_put16(b+9,a->punch.local_port);
    b[11]=(uint8_t)strlen(a->nick);memcpy(b+12,a->nick,b[11]);a->connecting=0;a->hb=a->room_rx=stamp();return room_send(a,CMD_JOIN_REQ,1,b,12+b[11]);
}
static int connect_room(struct client *a){
    if(a->joined){puts("Already joined: restart client to change identity/session");return -1;}
    if(a->room_fd>=0)room_close(a);
    struct addrinfo hints={0},*res=NULL;hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
    char port[16];snprintf(port,sizeof(port),"%d",PORT_ROOM_TCP);
    if(getaddrinfo(a->host,port,&hints,&res)!=0)return -1;
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0){freeaddrinfo(res);return -1;}
    if(fd>=FD_SETSIZE || fa_nonblock(fd)<0){close(fd);freeaddrinfo(res);return -1;}
    a->room_ip=((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    int rc=connect(fd,res->ai_addr,res->ai_addrlen);freeaddrinfo(res);
    if(rc<0 && errno!=EINPROGRESS){close(fd);return -1;}
    a->room_fd=fd;a->connecting=rc<0;a->deadline=stamp()+1.5;fa_reader_init(&a->reader);
    if(!rc)return joined_socket(a);
    return 0;
}
static int same_addr(const struct sockaddr_in *a,uint32_t ip,uint16_t port){return a->sin_addr.s_addr==ip && a->sin_port==htons(port);}
static void udp_read(struct client *a){
    unsigned char buf[1501];struct sockaddr_in src;socklen_t slen=sizeof(src);
    ssize_t n=recvfrom(a->punch.fd,buf,sizeof(buf),0,(struct sockaddr *)&src,&slen);
    if(n<8 || n>1200 || fa_u16(buf)!=V1_MAGIC || buf[2]!=1 || n!=8+fa_u16(buf+6))return;
    unsigned char *b=buf+8;size_t len=(size_t)n-8;
    if(buf[3]==CMD_PROBE_ACK){
        if(!same_addr(&src,a->room_ip,PORT_ROOM_UDP) || len!=6)return;
        uint32_t ip;uint16_t port;if(punch_parse_probe_ack(b,(uint16_t)len,&ip,&port)==0){ui_state_lock(&a->ui);struct in_addr x={.s_addr=ip};snprintf(a->ui.refl_text,sizeof(a->ui.refl_text),"%s:%u",inet_ntoa(x),port);ui_state_unlock(&a->ui);}
    }else if(buf[3]==CMD_ANNOUNCE){
        uint16_t room=0;uint32_t sid=0,hip=0;uint16_t tp=0,up=0;char nick[17];
        if(punch_parse_announce(b,(uint16_t)len,&room,&sid,&hip,&tp,&up,nick,sizeof(nick))==0){
            char ip[32];struct in_addr ia={.s_addr=hip};snprintf(ip,sizeof(ip),"%s",inet_ntoa(ia));
            ui_state_lock(&a->ui);a->ui.path_type=PATH_ANNOUNCE;
            ui_set_status(&a->ui,"收到通告 room=%u host=%s:%u nick=%s",room,ip,tp,nick);
            ui_push_event(&a->ui,"ANNOUNCE：%s room=%u session=%08X",nick,room,sid);
            int online=a->ui.room_online;ui_state_unlock(&a->ui);
            if(!online && (a->announced_host_ip_n!=hip || a->announced_host_tcp_h!=tp || a->announced_session!=sid)){
                a->announced_host_ip_n=hip;a->announced_host_tcp_h=tp;a->announced_session=sid;
                if(tr_connect_announced(a->tr,hip,tp,sid)<0){
                    ui_state_lock(&a->ui);ui_set_status(&a->ui,"通告房主连接失败 %s:%u",ip,tp);
                    ui_push_event(&a->ui,"通告连接失败");ui_state_unlock(&a->ui);
                }
            }
        }
    }else if(buf[3]==CMD_PUNCH){
        uint32_t sid;uint8_t from,to;if(punch_parse_punch(b,(uint16_t)len,&sid,&from,&to)<0 || from<1 || from>4 || from==a->ui.my_peer_id || to!=a->ui.my_peer_id || sid!=a->ui.session_id)return;
        for(int i=0;i<4;i++){struct ui_peer *p=&a->ui.peers[i];if(p->used && p->peer_id==from && (same_addr(&src,p->priv_ip_n,p->udp_port_h)||same_addr(&src,p->refl_ip_n,p->refl_port_h))){
            tr_accept_udp(a->tr,from,&src);
            if(!a->punch_seen[from]){punch_send_punch(&a->punch,&src,sid,to,from);a->punch_seen[from]=1;a->punch_addr[from]=src;}
            break;
        }}
    }else tr_udp(a->tr,buf,(size_t)n,&src);
}
static void command(struct client *a,const char *line){
    printf("COMMAND %s\n",line);
    if(!strcmp(line,"quit")){atomic_store(&a->stop,1);return;}
    if(!strcmp(line,"get") || !strcmp(line,"get all")){tr_download(a->tr,1);return;}
    if(!strcmp(line,"pause")){tr_download(a->tr,0);return;}
    if(!strcmp(line,"stats")){tr_summary(a->tr);return;}
    if(!strcmp(line,"probe")){if(a->joined)punch_send_probe(&a->punch,a->host,a->room_ip,a->room_id,a->ui.my_peer_id);return;}
    if(!strcmp(line,"announce")){if(a->role==1 && a->joined)punch_send_announce(&a->punch,a->room_id,a->ui.session_id,a->local_ip,tr_port(a->tr),a->punch.local_port,a->nick);return;}
    if(!strncmp(line,"join ",5)){
        if(a->joined){puts("Already joined; use get, pause or quit");return;}
        char role[16]={0};int n=sscanf(line+5,"%16s %255s %15s",a->nick,a->host,role);if(n<1)return;a->role=n==3 && !strcmp(role,"host")?1:2;
        ui_state_lock(&a->ui);snprintf(a->ui.my_nick,sizeof(a->ui.my_nick),"%s",a->nick);ui_state_unlock(&a->ui);connect_room(a);return;
    }
    ui_state_lock(&a->ui);
    if(!strcmp(line,"next")){
        if(a->ui.phase==UI_PHASE_CH1 && a->ui.files[0].have && a->ui.files[1].have)ui_advance_phase(&a->ui,UI_PHASE_QUIZ);
        else if(a->ui.phase==UI_PHASE_CH2 && a->ui.files[2].have)ui_advance_phase(&a->ui,UI_PHASE_CHOICE);
        else ui_set_status(&a->ui,"本机证物未校验齐全，不能推进");
    }else if(!strncmp(line,"answer ",7) && a->ui.phase==UI_PHASE_QUIZ)ui_advance_phase(&a->ui,UI_PHASE_CH2);
    else if(!strncmp(line,"choose ",7) && a->ui.phase==UI_PHASE_CHOICE){
        const char *opt=line+7;while(*opt==' ')opt++;
        choice_log_append(a->ui.my_nick[0]?a->ui.my_nick:a->nick,opt);
        ui_advance_phase(&a->ui,UI_PHASE_DONE);
        ui_push_event(&a->ui,"选择已提交，本局结束");
    }
    else if(!strcmp(line,"redraw")){}else ui_set_status(&a->ui,"命令：get / pause / stats / next / answer / choose / quit");
    ui_term_draw(&a->ui);ui_state_unlock(&a->ui);
}
static void *network(void *ctx){
    struct client *a=ctx;a->born=stamp();if(a->automatic)connect_room(a);
    while(!atomic_load(&a->stop)){
        double now=stamp();if(a->seconds>0 && now-a->born>=a->seconds)break;
        if(a->connecting && now>=a->deadline)room_close(a);
        if(a->room_fd>=0 && !a->connecting){
            if(now-a->room_rx>15)room_close(a);
            else if(a->joined){
                if(now-a->hb>=5){room_send(a,CMD_HEARTBEAT,0,NULL,0);a->hb=now;puts("ROOM_HEARTBEAT");}
                size_t n;tr_manifest_bytes(a->tr,&n);if(!n && now-a->manifest_retry>=1){unsigned char b[4]={0};room_send(a,CMD_MANIFEST_REQ,2,b,4);a->manifest_retry=now;}
            }
        }
        if(a->joined && a->ui.session_id && now-a->punch_due>=.5){
            for(int i=0;i<4;i++){struct ui_peer *p=&a->ui.peers[i];if(!p->used || !tr_should_punch(a->tr,p->peer_id))continue;
                struct sockaddr_in dst={0};dst.sin_family=AF_INET;dst.sin_addr.s_addr=p->refl_ip_n?p->refl_ip_n:p->priv_ip_n;dst.sin_port=htons(p->refl_port_h?p->refl_port_h:p->udp_port_h);
                if(dst.sin_addr.s_addr && dst.sin_port)punch_send_punch(&a->punch,&dst,a->ui.session_id,a->ui.my_peer_id,p->peer_id);
            }a->punch_due=now;
        }
        tr_tick(a->tr);
        fd_set rd,wr;FD_ZERO(&rd);FD_ZERO(&wr);int max=a->wake[0];FD_SET(a->wake[0],&rd);FD_SET(a->punch.fd,&rd);if(a->punch.fd>max)max=a->punch.fd;
        if(a->room_fd>=0){if(!a->connecting)FD_SET(a->room_fd,&rd);if(a->connecting || a->writer.end>a->writer.offset)FD_SET(a->room_fd,&wr);if(a->room_fd>max)max=a->room_fd;}
        tr_sets(a->tr,&rd,&wr,&max);struct timeval tv={0,1000};int rc=select(max+1,&rd,&wr,NULL,&tv);
        if(rc<0){if(errno==EINTR)continue;break;}
        /* Process the selected sockets before commands can open/reuse descriptors. */
        if(a->room_fd>=0 && FD_ISSET(a->room_fd,&wr)){
            if(a->connecting){int err=0;socklen_t n=sizeof(err);if(getsockopt(a->room_fd,SOL_SOCKET,SO_ERROR,&err,&n)<0 || err || joined_socket(a)<0)room_close(a);}
            if(a->room_fd>=0 && fa_flush(&a->writer,a->room_fd)<0)room_close(a);
        }
        if(a->room_fd>=0 && FD_ISSET(a->room_fd,&rd)){
            unsigned char b[4096];ssize_t n=recv(a->room_fd,b,sizeof(b),0);
            if(n==0)room_close(a);else if(n>0){if(fa_feed(&a->reader,b,(size_t)n,on_room,a)<0)room_close(a);}else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)room_close(a);
        }
        if(FD_ISSET(a->punch.fd,&rd))udp_read(a);
        tr_ready(a->tr,&rd,&wr);
        if(FD_ISSET(a->wake[0],&rd)){
            fa_wake_drain(a->wake[0]);puts("WAKE_COMMAND");
            for(;;){char line[512];pthread_mutex_lock(&a->queue_mu);if(a->head==a->tail){pthread_mutex_unlock(&a->queue_mu);break;}snprintf(line,sizeof(line),"%s",a->queue[a->head]);a->head=(a->head+1)%32;pthread_mutex_unlock(&a->queue_mu);command(a,line);}
        }
    }
    tr_summary(a->tr);atomic_store(&a->stop,1);return NULL;
}
int main(int argc,char **argv){
    struct client *a=calloc(1,sizeof(*a));if(!a)return 1;a->room_fd=-1;a->role=2;
    char dir[256]=".",session[384]="";int mode=0,cs=0,delay=0,download=0;
    snprintf(a->host,sizeof(a->host),"localhost");snprintf(a->nick,sizeof(a->nick),"detective");
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--host"))a->role=1;
        else if(!strcmp(argv[i],"--auto"))download=1;
        else if(!strcmp(argv[i],"--cs"))cs=1;
        else if(i+1<argc && !strcmp(argv[i],"--room")){snprintf(a->host,sizeof(a->host),"%s",argv[++i]);a->automatic=1;}
        else if(i+1<argc && !strcmp(argv[i],"--nick")){if(strlen(argv[i+1])>16)return 2;snprintf(a->nick,sizeof(a->nick),"%s",argv[++i]);}
        else if(i+1<argc && !strcmp(argv[i],"--data-dir"))snprintf(dir,sizeof(dir),"%s",argv[++i]);
        else if(i+1<argc && !strcmp(argv[i],"--session"))snprintf(session,sizeof(session),"%s",argv[++i]);
        else if(i+1<argc && !strcmp(argv[i],"--mode")){const char *v=argv[++i];if(!strcmp(v,"udp"))mode=1;else if(!strcmp(v,"relay"))mode=2;else if(strcmp(v,"tcp"))return 2;}
        else if(i+1<argc && !strcmp(argv[i],"--delay-ms"))delay=atoi(argv[++i]);
        else if(i+1<argc && !strcmp(argv[i],"--run-seconds"))a->seconds=atoi(argv[++i]);
        else{fprintf(stderr,"usage: %s [--room HOST --nick NAME --host --session DIR --data-dir DIR --auto --mode tcp|udp|relay --cs --delay-ms N --run-seconds N]\n",argv[0]);return 2;}
    }
    if(delay<0 || a->seconds<0)return 2;
    setvbuf(stdout,NULL,_IONBF,0);signal(SIGPIPE,SIG_IGN);log_set_console(0);
    ui_state_init(&a->ui);pthread_mutex_init(&a->queue_mu,NULL);snprintf(a->ui.my_nick,sizeof(a->ui.my_nick),"%s",a->nick);
    if(punch_open(&a->punch)<0 || a->punch.fd>=FD_SETSIZE || fa_wake_open(a->wake)<0)return 1;
    a->tr=tr_create(&a->ui,a->punch.fd,dir);if(!a->tr)return 1;
    if(session[0] && tr_load(a->tr,session)<0){fputs("Invalid session/manifest\n",stderr);return 1;}
    tr_mode(a->tr,mode,cs,delay);tr_download(a->tr,download);draw(a);
    pthread_t thread;if(pthread_create(&thread,NULL,network,a))return 1;
    int eof=0;
    while(!atomic_load(&a->stop)){
        struct pollfd p={STDIN_FILENO,POLLIN,0};int rc=poll(eof?NULL:&p,eof?0:1,100);
        if(rc>0 && (p.revents&(POLLIN|POLLHUP))){char line[512];if(!fgets(line,sizeof(line),stdin)){eof=1;if(!a->seconds)atomic_store(&a->stop,1);continue;}fa_chomp_line(line);
            pthread_mutex_lock(&a->queue_mu);unsigned next=(a->tail+1)%32;if(next!=a->head){snprintf(a->queue[a->tail],512,"%s",line);a->tail=next;}else fputs("Command queue full\n",stderr);pthread_mutex_unlock(&a->queue_mu);fa_wake_signal(a->wake[1]);
        }
    }
    fa_wake_signal(a->wake[1]);pthread_join(thread,NULL);if(a->room_fd>=0)close(a->room_fd);tr_destroy(a->tr);punch_close(&a->punch);close(a->wake[0]);close(a->wake[1]);pthread_mutex_destroy(&a->queue_mu);pthread_mutex_destroy(&a->ui.mu);free(a);return 0;
}
