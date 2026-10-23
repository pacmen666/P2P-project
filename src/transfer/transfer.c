#define _POSIX_C_SOURCE 200809L
#include "transfer.h"
#include "fa_stream.h"
#include "generate.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#define CONNS 9
#define PIECES 32
struct connection {
    int fd, connecting, id, relay;
    double deadline, last_rx, last_hb;
    struct fa_reader rx;
    struct fa_writer tx;
    struct transfer *owner;
};
struct peer {
    struct ui_peer info;
    struct sockaddr_in udp;
    int udp_ok, fallback;
    uint32_t bits[3];
    double retry_connect, last_bits, route_since;
    int pending, file, piece, path, retries;
    uint16_t request, received;
    double due;
    unsigned char assembling[GEN_PIECE];
};
struct upload {
    int active, id, file, piece, path;
    uint16_t mask, request;
    double due;
};
struct transfer {
    struct ui_state *ui;
    int listener, udp_fd, id, room, mode, cs_only, downloading, delay_ms, cursor;
    uint16_t port, serial;
    uint32_t relay_ip;
    char root[256], dir[384];
    struct gen_manifest manifest;
    unsigned char raw[4096]; size_t raw_len;
    unsigned char *data[3]; uint32_t have[3]; int complete[3], last_source[3];
    struct peer peers[5]; struct connection conn[CONNS];
    struct upload uploads[32];
    double relay_retry, next_request, waiting_log;
    uint64_t up_tcp, up_udp, up_relay, down[5];
    double dl_start, dl_end; unsigned hash_reject; /* 实验8/9统计：完成时间、校验拒绝数 */
};
static double stamp(void) { struct timespec x; clock_gettime(CLOCK_MONOTONIC,&x); return x.tv_sec+x.tv_nsec/1e9; }
static uint32_t full(unsigned n) { return n==32?UINT32_MAX:((1u<<n)-1u); }
static size_t piece_len(struct transfer *t,int f,int k) {
    size_t n=t->manifest.files[f].file_size-(size_t)k*GEN_PIECE;
    return n<GEN_PIECE?n:GEN_PIECE;
}
static int hash_ok(const void *p,size_t n,const unsigned char *expected) {
    unsigned char h[32]; SHA256(p,n,h); return !memcmp(h,expected,32);
}
static void event(struct transfer *t,const char *s,int a,int b) {
    printf("TRANSFER %s %d %d\n",s,a,b); fflush(stdout);
    ui_state_lock(t->ui); ui_push_event(t->ui,"%s %d %d",s,a,b); ui_state_unlock(t->ui);
}
static struct connection *link_for(struct transfer *t,int id) {
    for(int i=0;i<CONNS;i++) if(t->conn[i].fd>=0 && !t->conn[i].relay && t->conn[i].id==id) return &t->conn[i];
    return NULL;
}
static struct connection *relay_for(struct transfer *t) {
    for(int i=0;i<CONNS;i++) if(t->conn[i].fd>=0 && t->conn[i].relay) return &t->conn[i];
    return NULL;
}
static void drop(struct connection *c) {
    struct transfer *t=c->owner;
    if(c->fd>=0) close(c->fd);
    if(c->id>0 && c->id<=4) {
        struct peer *p=&t->peers[c->id];
        if(p->pending && p->path==PATH_LAN_TCP) { event(t,"SOURCE_LOST",c->id,p->piece); p->pending=0; }
        p->retry_connect=stamp()+2;
    }
    if(c->relay) for(int j=1;j<=4;j++) if(t->peers[j].path==PATH_RELAY) t->peers[j].pending=0;
    c->fd=-1; c->tx.offset=c->tx.end=0;
}
static struct connection *add_link(struct transfer *t,int fd,int id,int relay,int connecting) {
    if(fd<0 || fd>=FD_SETSIZE) { if(fd>=0) close(fd); return NULL; }
    int yes=1;
    if(fa_nonblock(fd)<0 || setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&yes,sizeof(yes))<0) {close(fd);return NULL;}
    for(int i=0;i<CONNS;i++) if(t->conn[i].fd<0) {
        struct connection *c=&t->conn[i]; memset(c,0,sizeof(*c));
        c->fd=fd;c->id=id;c->relay=relay;c->connecting=connecting;c->owner=t;
        c->last_rx=c->last_hb=stamp();c->deadline=stamp()+1.5;
        return c;
    }
    close(fd);return NULL;
}
static int send_route(struct transfer *t,int id,int path,uint8_t cmd,uint16_t req,const void *body,size_t len) {
    if(path==PATH_LAN_TCP) {
        struct connection *c=link_for(t,id);
        return c && !c->connecting?fa_enqueue(&c->tx,cmd,req,body,len):-1;
    }
    unsigned char packet[1200]; int n=fa_pack(packet,sizeof(packet),cmd,req,body,len);
    if(n<0 || id<1 || id>4) return -1;
    if(path==PATH_PUNCH) {
        struct peer *p=&t->peers[id]; if(!p->udp_ok || p->fallback) return -1;
        ssize_t sent=sendto(t->udp_fd,packet,(size_t)n,0,(struct sockaddr *)&p->udp,sizeof(p->udp));
        return sent==n?0:-1;
    }
    struct connection *r=relay_for(t);
    if(path==PATH_RELAY && r && !r->connecting) {
        unsigned char wrap[1204];wrap[0]=(uint8_t)id;wrap[1]=(uint8_t)t->id;
        fa_put16(wrap+2,(uint16_t)n);memcpy(wrap+4,packet,(size_t)n);
        return fa_enqueue(&r->tx,CMD_RELAY_WRAP,0,wrap,(size_t)n+4);
    }
    return -1;
}
static int route(struct transfer *t,int id) {
    struct peer *p=&t->peers[id];struct connection *c=link_for(t,id);
    if(!t->mode && c && !c->connecting) return PATH_LAN_TCP;
    if(t->mode!=2 && p->udp_ok && !p->fallback) return PATH_PUNCH;
    if(t->mode==2 || p->fallback) {c=relay_for(t);if(c && !c->connecting)return PATH_RELAY;}
    return 0;
}
static void bits_to(struct transfer *t,int id,int path) {
    if(!t->raw_len || !t->id) return;
    for(int f=0;f<3;f++) {
        unsigned char b[12]={0};unsigned n=t->manifest.files[f].piece_count;
        fa_put32(b,t->manifest.session_id);b[4]=(uint8_t)t->id;b[5]=(uint8_t)(f+1);fa_put16(b+6,(uint16_t)n);
        for(unsigned k=0;k<n;k++) if(t->have[f]&(1u<<k)) b[8+k/8]|=(uint8_t)(0x80u>>(k%8));
        send_route(t,id,path,CMD_BITFIELD,0,b,8+(n+7)/8);
    }
}
static void changed(struct transfer *t,int f,int k) {
    unsigned char b[7];fa_put32(b,t->manifest.session_id);b[4]=(uint8_t)(f+1);fa_put16(b+5,(uint16_t)k);
    for(int id=1;id<=4;id++) if(id!=t->id) {
        struct connection *c=link_for(t,id);
        if(c && !c->connecting)send_route(t,id,PATH_LAN_TCP,CMD_HAVE,0,b,7);
        if(t->peers[id].udp_ok && !t->peers[id].fallback)send_route(t,id,PATH_PUNCH,CMD_HAVE,0,b,7);
        if(t->peers[id].fallback || t->mode==2)send_route(t,id,PATH_RELAY,CMD_HAVE,0,b,7);
    }
}
static void finish_file(struct transfer *t,int f) {
    struct gen_file *g=&t->manifest.files[f];
    if(t->complete[f] || t->have[f]!=full(g->piece_count)) return;
    if(!hash_ok(t->data[f],g->file_size,g->file_hash)) {event(t,"FILE_HASH_FAIL",f+1,0);return;}
    t->complete[f]=1;
    if(!t->dl_end && t->complete[0] && t->complete[1] && t->complete[2])t->dl_end=stamp();
    int from=t->last_source[f];
    ui_state_lock(t->ui);
    ui_set_file_status(t->ui,(uint8_t)(f+1),1,from?t->peers[from].info.nick:"local");
    ui_term_draw(t->ui);ui_state_unlock(t->ui);
    event(t,"FILE_COMPLETE",f+1,from);
}
static int store_piece(struct transfer *t,int id,int f,int k,const unsigned char *bytes,size_t n) {
    if(n!=piece_len(t,f,k) || !hash_ok(bytes,n,t->manifest.files[f].pieces[k].hash)) {t->hash_reject++;event(t,"HASH_REJECT",id,k);return -1;}
    if(t->have[f]&(1u<<k)) return 0;
    FILE *fp=fopen(t->manifest.files[f].path,"r+b");
    if(!fp)fp=fopen(t->manifest.files[f].path,"w+b");
    if(!fp)return -1;
    int ok=fseek(fp,(long)k*GEN_PIECE,SEEK_SET)==0 && fwrite(bytes,1,n,fp)==n;
    if(fclose(fp)!=0)ok=0;
    if(!ok)return -1;
    memcpy(t->data[f]+(size_t)k*GEN_PIECE,bytes,n);t->have[f]|=1u<<k;t->last_source[f]=id;t->down[id]+=n;
    event(t,"PIECE_OK",id,k);changed(t,f,k);finish_file(t,f);return 0;
}
static int valid_manifest(const unsigned char *b,size_t n) {
    if(n<37 || n>4096 || !fa_u32(b) || b[36]!=3)return -1;
    size_t off=37;
    for(int f=0;f<3;f++) {
        if(off+41>n || b[off]!=f+1 || b[off+1]!=(f==2?2:1) || b[off+2]!=(f==2?2:1))return -1;
        unsigned sz=fa_u32(b+off+3),cnt=fa_u16(b+off+39);
        if(!sz || sz>512*1024 || cnt!=(sz+GEN_PIECE-1)/GEN_PIECE || cnt>PIECES)return -1;
        off+=41+32*cnt;if(off>n)return -1;
    }
    return off==n?0:-1;
}
int tr_manifest(struct transfer *t,const unsigned char *b,size_t n) {
    if(valid_manifest(b,n)<0)return -1;
    if(t->raw_len)return t->raw_len==n && !memcmp(t->raw,b,n)?0:-1; /* never mix sessions */
    if(!t->dir[0]) {
        if(mkdir(t->root,0700)<0 && errno!=EEXIST)return -1;
        if(snprintf(t->dir,sizeof(t->dir),"%s/session_%08x",t->root,fa_u32(b))>=(int)sizeof(t->dir))return -1;
    }
    if(mkdir(t->dir,0700)<0 && errno!=EEXIST)return -1;
    char path[512];snprintf(path,sizeof(path),"%s/manifest.bin",t->dir);
    FILE *fp=fopen(path,"wb");if(!fp)return -1;
    int written=fwrite(b,1,n,fp)==n;if(fclose(fp)!=0)written=0;if(!written)return -1;
    if(gen_load_manifest(t->dir,&t->manifest)<0)return -1;
    memcpy(t->raw,b,n);t->raw_len=n;
    for(int f=0;f<3;f++) {
        struct gen_file *g=&t->manifest.files[f];t->data[f]=calloc(1,g->file_size);if(!t->data[f])return -1;
        ui_state_lock(t->ui);ui_apply_manifest_header(t->ui,t->manifest.session_id,NULL);
        ui_set_file_meta(t->ui,g->file_id,g->chapter,g->kind,g->file_size,g->file_hash);ui_state_unlock(t->ui);
        for(unsigned k=0;k<g->piece_count;k++) {
            unsigned char piece[GEN_PIECE];size_t got;
            if(gen_read_piece(&t->manifest,g->file_id,(uint16_t)k,piece,&got)==0 && hash_ok(piece,got,g->pieces[k].hash)) {
                memcpy(t->data[f]+k*GEN_PIECE,piece,got);t->have[f]|=1u<<k;event(t,"RESUME_BLOCK",f+1,(int)k);
            }
        }
        finish_file(t,f);
    }
    event(t,"MANIFEST_READY",(int)n,0);return 0;
}
int tr_load(struct transfer *t,const char *dir) {
    if(strlen(dir)>=sizeof(t->dir)-16)return -1;
    char path[512];snprintf(path,sizeof(path),"%s/manifest.bin",dir);FILE *fp=fopen(path,"rb");if(!fp)return -1;
    unsigned char b[4097];size_t n=fread(b,1,sizeof(b),fp);fclose(fp);
    if(valid_manifest(b,n)<0)return -1;
    snprintf(t->dir,sizeof(t->dir),"%s",dir);return tr_manifest(t,b,n);
}
const unsigned char *tr_manifest_bytes(struct transfer *t,size_t *n) {*n=t->raw_len;return t->raw;}
static int dispatch(struct transfer *t,int id,int path,const struct fa_message *m) {
    if(id<1 || id>4 || id==t->id)return -1;
    struct peer *p=&t->peers[id];const unsigned char *b=m->body;size_t n=m->length;
    if(m->cmd==CMD_HEARTBEAT)return n? -1:send_route(t,id,path,CMD_HEARTBEAT_ACK,0,NULL,0);
    if(m->cmd==CMD_HEARTBEAT_ACK)return n?-1:0;
    if(m->cmd==CMD_BITFIELD) {
        if(!t->raw_len || n<9 || fa_u32(b)!=t->manifest.session_id || b[4]!=id || b[5]<1 || b[5]>3)return -1;
        int f=b[5]-1;unsigned count=t->manifest.files[f].piece_count;
        if(fa_u16(b+6)!=count || n!=8+(count+7)/8)return -1;
        p->bits[f]=0;for(unsigned k=0;k<count;k++)if(b[8+k/8]&(0x80u>>(k%8)))p->bits[f]|=1u<<k;
        return 0;
    }
    if(m->cmd==CMD_ERROR)return 0;
    if(!t->raw_len || n<7 || fa_u32(b)!=t->manifest.session_id || b[4]<1 || b[4]>3)return -1;
    int f=b[4]-1,k;
    if(m->cmd==CMD_HAVE) {
        k=fa_u16(b+5);if(n!=7 || k>=t->manifest.files[f].piece_count)return -1;
        p->bits[f]|=1u<<k;return 0; /* remote availability, never local completion */
    }
    if(m->cmd==CMD_PIECE_REQ || m->cmd==CMD_PIECE_NAK) {
        uint16_t mask=0xffff;
        if(m->cmd==CMD_PIECE_REQ) {if(n!=9 || !m->request)return -1;
        k=fa_u16(b+5);mask=fa_u16(b+7);}
        else {
            if(n<10)return -1;
        k=fa_u16(b+6);
            if(k>=t->manifest.files[f].piece_count)return -1;
            if(b[5]!=3) {if(n!=10 || b[5]<1 || b[5]>2)return -1;
                if(p->pending && p->request==m->request && p->file==f && p->piece==k){p->pending=0;p->bits[f]&=~(1u<<k);}return 0;}
            unsigned count=fa_u16(b+8);if(count>16 || n!=10+count*2 || path==PATH_LAN_TCP)return -1;
            mask=0;for(unsigned j=0;j<count;j++){unsigned sub=fa_u16(b+10+2*j);if(sub>=16)return -1;mask|=(uint16_t)(1u<<sub);}
        }
        if(k>=t->manifest.files[f].piece_count)return -1;
        if(!(t->have[f]&(1u<<k))) {
            unsigned char no[10]={0};fa_put32(no,t->manifest.session_id);no[4]=(unsigned char)(f+1);no[5]=1;fa_put16(no+6,(uint16_t)k);
            return send_route(t,id,path,CMD_PIECE_NAK,m->request,no,10);
        }
        size_t len=piece_len(t,f,k);
        if(path==PATH_LAN_TCP) {
            unsigned char out[GEN_PIECE+10]={0};fa_put32(out,t->manifest.session_id);out[4]=(unsigned char)(f+1);
            fa_put16(out+6,(uint16_t)k);fa_put16(out+8,(uint16_t)len);memcpy(out+10,t->data[f]+k*GEN_PIECE,len);
            int rc=send_route(t,id,path,CMD_PIECE_DATA,m->request,out,len+10);if(!rc)t->up_tcp+=len;return rc;
        }
        mask&=(uint16_t)full((unsigned)((len+1023)/1024));
        for(int u=0;u<32;u++)if(t->uploads[u].active && t->uploads[u].id==id && t->uploads[u].file==f && t->uploads[u].piece==k && t->uploads[u].request==m->request){t->uploads[u].mask|=mask;return 0;}
        for(int u=0;u<32;u++)if(!t->uploads[u].active){t->uploads[u]=(struct upload){1,id,f,k,path,mask,m->request,stamp()};return 0;}
        return -1;
    }
    if(m->cmd==CMD_PIECE_DATA) {
        if(n<10 || b[5]!=0)return -1;
        k=fa_u16(b+6);
        if(k>=t->manifest.files[f].piece_count)return -1;
        if(!p->pending || p->file!=f || p->piece!=k || p->request!=m->request || p->path!=path)return 0; /* stale duplicate */
        size_t len=piece_len(t,f,k);int done=0;
        if(path==PATH_LAN_TCP){if(n!=10+len || fa_u16(b+8)!=len)return -1;memcpy(p->assembling,b+10,len);done=1;}
        else {
            if(n<14)return -1;
            unsigned sub=fa_u16(b+8),count=fa_u16(b+10),size=fa_u16(b+12);
            if(count!=(len+1023)/1024 || sub>=count || size!=(sub+1<count?1024:len-sub*1024) || n!=14+size)return -1;
            memcpy(p->assembling+sub*1024,b+14,size);p->received|=(uint16_t)(1u<<sub);done=p->received==(uint16_t)full(count);
        }
        if(done){
            if(store_piece(t,id,f,k,p->assembling,len)<0) {p->bits[f]&=~(1u<<k);unsigned char no[10]={0};fa_put32(no,t->manifest.session_id);no[4]=(uint8_t)(f+1);no[5]=2;fa_put16(no+6,(uint16_t)k);send_route(t,id,path,CMD_PIECE_NAK,m->request,no,10);}
            p->pending=0;
            ui_state_lock(t->ui);t->ui->path_type=path;ui_state_unlock(t->ui);
        }
        return 0;
    }
    return -1;
}
static int on_message(void *ctx,const struct fa_message *m) {
    struct connection *c=ctx;struct transfer *t=c->owner;c->last_rx=stamp();
    if(c->relay) {
        if(m->cmd==CMD_ERROR)return 0;
        if(m->cmd!=CMD_RELAY_WRAP || m->length<12)return -1;
        const unsigned char *b=m->body;unsigned len=fa_u16(b+2);
        if(len<8 || m->length!=4+len || b[0]!=t->id || b[1]<1 || b[1]>4 || b[1]==t->id)return -1;
        b+=4;if(fa_u16(b)!=V1_MAGIC || b[2]!=1 || fa_u16(b+6)+8u!=len || len>1200)return -1;
        struct fa_message inner={b[3],fa_u16(b+4),fa_u16(b+6),b+8};
        t->peers[m->body[1]].fallback=1;
        return dispatch(t,m->body[1],PATH_RELAY,&inner);
    }
    if(m->cmd==CMD_MANIFEST_REQ){if(m->length!=4)return -1;
        if(t->raw_len && (!fa_u32(m->body)||fa_u32(m->body)==t->manifest.session_id))return fa_enqueue(&c->tx,CMD_MANIFEST,m->request,t->raw,t->raw_len);
        unsigned char error[2];fa_put16(error,ERR_NO_MANIFEST);return fa_enqueue(&c->tx,CMD_ERROR,m->request,error,2);}
    if(m->cmd==CMD_MANIFEST){int rc=tr_manifest(t,m->body,m->length);if(!rc && c->id)bits_to(t,c->id,PATH_LAN_TCP);return rc;}
    if(m->cmd==CMD_BITFIELD && m->length>=9){int id=m->body[4];
        if(id<1 || id>4 || id==t->id || (c->id && c->id!=id))return -1;
        if(!c->id){if(id>t->id || link_for(t,id))return -1;c->id=id;bits_to(t,id,PATH_LAN_TCP);}
    }
    return dispatch(t,c->id,PATH_LAN_TCP,m);
}
static void connected(struct connection *c) {
    struct transfer *t=c->owner;c->connecting=0;c->last_rx=stamp();
    if(c->relay){unsigned char b[3];b[0]=(unsigned char)t->id;fa_put16(b+1,(uint16_t)t->room);fa_enqueue(&c->tx,CMD_RELAY_BIND,1,b,3);event(t,"RELAY_CONNECTED",t->id,0);}
    else if(c->id){bits_to(t,c->id,PATH_LAN_TCP);event(t,"TCP_CONNECTED",t->id,c->id);}
}
static void dial(struct transfer *t,int id,uint32_t ip,uint16_t port,int relay) {
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return;
    if(fa_nonblock(fd)<0){close(fd);return;}
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=ip;addr.sin_port=htons(port);
    int rc=connect(fd,(struct sockaddr *)&addr,sizeof(addr));if(rc<0 && errno!=EINPROGRESS){close(fd);return;}
    struct connection *c=add_link(t,fd,id,relay,rc<0);if(c && !rc)connected(c);
}
struct transfer *tr_create(struct ui_state *ui,int udp_fd,const char *root) {
    struct transfer *t=calloc(1,sizeof(*t));if(!t)return NULL;t->ui=ui;t->udp_fd=udp_fd;t->listener=-1;
    for(int i=0;i<CONNS;i++){t->conn[i].fd=-1;t->conn[i].owner=t;}
    if(strlen(root)>=sizeof(t->root)){free(t);return NULL;}snprintf(t->root,sizeof(t->root),"%s",root);
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0){free(t);return NULL;}t->listener=fd;
    struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);
    if(fd>=FD_SETSIZE || fa_nonblock(fd)<0 || bind(fd,(struct sockaddr *)&a,sizeof(a))<0 || listen(fd,8)<0){tr_destroy(t);return NULL;}
    socklen_t n=sizeof(a);getsockname(fd,(struct sockaddr *)&a,&n);t->port=ntohs(a.sin_port);return t;
}
void tr_destroy(struct transfer *t){if(!t)return;for(int i=0;i<CONNS;i++)if(t->conn[i].fd>=0)close(t->conn[i].fd);if(t->listener>=0)close(t->listener);for(int f=0;f<3;f++)free(t->data[f]);gen_free_manifest(&t->manifest);free(t);}
unsigned short tr_port(struct transfer *t){return t->port;}
void tr_identity(struct transfer *t,unsigned id,unsigned room,uint32_t ip){t->id=(int)id;t->room=(int)room;t->relay_ip=ip;}
void tr_member(struct transfer *t,const struct ui_peer *p) {
    if(p->peer_id<1 || p->peer_id>4)return;
    struct peer *q=&t->peers[p->peer_id];
    if(q->info.used && (q->info.priv_ip_n!=p->priv_ip_n ||
       q->info.tcp_port_h!=p->tcp_port_h || q->info.udp_port_h!=p->udp_port_h)) {
        struct connection *c=link_for(t,p->peer_id);if(c)drop(c);
        for(int u=0;u<32;u++)if(t->uploads[u].id==p->peer_id)t->uploads[u].active=0;
        memset(q,0,sizeof(*q));
    }
    if(!q->info.used)q->route_since=stamp();
    q->info=*p;
}
void tr_mode(struct transfer *t,int mode,int cs_only,int delay_ms){t->mode=mode;t->cs_only=cs_only;t->delay_ms=delay_ms;}
void tr_download(struct transfer *t,int enabled){t->downloading=enabled;if(enabled && !t->dl_start)t->dl_start=stamp();event(t,enabled?"DOWNLOAD_ON":"DOWNLOAD_PAUSED",0,0);}
void tr_accept_udp(struct transfer *t,unsigned id,const struct sockaddr_in *a){if(id<1 || id>4 || id==(unsigned)t->id)return;struct peer *p=&t->peers[id];int first=!p->udp_ok;p->udp=*a;p->udp_ok=1;if(first){event(t,"UDP_READY",(int)id,0);bits_to(t,(int)id,PATH_PUNCH);}}
int tr_udp(struct transfer *t,const unsigned char *buf,size_t n,const struct sockaddr_in *addr){
    if(n<8 || n>1200 || fa_u16(buf)!=V1_MAGIC || buf[2]!=1 || n!=8u+fa_u16(buf+6))return -1;
    for(int id=1;id<=4;id++){struct peer *p=&t->peers[id];if(p->udp_ok && !p->fallback && p->udp.sin_addr.s_addr==addr->sin_addr.s_addr && p->udp.sin_port==addr->sin_port){struct fa_message m={buf[3],fa_u16(buf+4),fa_u16(buf+6),buf+8};return dispatch(t,id,PATH_PUNCH,&m);}}
    return -1;
}
void tr_sets(struct transfer *t,fd_set *rd,fd_set *wr,int *maxfd){FD_SET(t->listener,rd);if(t->listener>*maxfd)*maxfd=t->listener;for(int i=0;i<CONNS;i++){struct connection *c=&t->conn[i];if(c->fd<0)continue;if(!c->connecting)FD_SET(c->fd,rd);if(c->connecting || c->tx.end>c->tx.offset)FD_SET(c->fd,wr);if(c->fd>*maxfd)*maxfd=c->fd;}}
void tr_ready(struct transfer *t,fd_set *rd,fd_set *wr){
    for(int i=0;i<CONNS;i++){struct connection *c=&t->conn[i];if(c->fd<0)continue;
        if(FD_ISSET(c->fd,wr)){if(c->connecting){int error=0;socklen_t n=sizeof(error);if(getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&error,&n)<0 || error){drop(c);continue;}connected(c);}if(fa_flush(&c->tx,c->fd)<0){drop(c);continue;}}
        if(c->fd>=0 && FD_ISSET(c->fd,rd)){unsigned char b[4096];ssize_t n=recv(c->fd,b,sizeof(b),0);if(n==0){drop(c);continue;}if(n<0){if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)drop(c);continue;}if(fa_feed(&c->rx,b,(size_t)n,on_message,c)<0)drop(c);}
    }
    if(FD_ISSET(t->listener,rd)){int fd=accept(t->listener,NULL,NULL);if(fd>=0){struct connection *c=add_link(t,fd,0,0,0);if(c && t->raw_len)fa_enqueue(&c->tx,CMD_MANIFEST,0,t->raw,t->raw_len);}}
}
void tr_tick(struct transfer *t){
    double now=stamp();
    for(int i=0;i<CONNS;i++){struct connection *c=&t->conn[i];if(c->fd<0)continue;
        if((c->connecting && now>=c->deadline) || (!c->relay && now-c->last_rx>15)){drop(c);continue;}
        if(!c->connecting && !c->relay && now-c->last_hb>=5){fa_enqueue(&c->tx,CMD_HEARTBEAT,0,NULL,0);c->last_hb=now;}
    }
    if(!t->id || !t->raw_len)return;
    /* Bind a standby relay connection on every peer, including upload-only peers.
       Otherwise a receiver falling back cannot reach its still-UDP sender. */
    int need_relay=1;
    for(int id=1;id<=4;id++)if(id!=t->id && t->peers[id].info.used){
        struct peer *p=&t->peers[id];
        if(!t->mode && t->id<id && !link_for(t,id) && now>=p->retry_connect){p->retry_connect=now+2;dial(t,id,p->info.priv_ip_n,p->info.tcp_port_h,0);}
        if(t->mode==2 || (!route(t,id) && now-p->route_since>=8))p->fallback=1;
        if(p->fallback)need_relay=1;
        int path=route(t,id);
        if(path && now-p->last_bits>=1){bits_to(t,id,path);p->last_bits=now;}
        if(p->pending && now>=p->due){
            if(p->path==PATH_PUNCH){
                p->retries++;event(t,"UDP_RETRY",id,p->retries);
                if(p->retries>=5){p->fallback=1;p->pending=0;need_relay=1;event(t,"UDP_FALLBACK",id,5);continue;}
                unsigned char nak[42]={0};fa_put32(nak,t->manifest.session_id);nak[4]=(uint8_t)(p->file+1);nak[5]=3;fa_put16(nak+6,(uint16_t)p->piece);
                unsigned count=(unsigned)((piece_len(t,p->file,p->piece)+1023)/1024),missing=0;
                for(unsigned s=0;s<count;s++)if(!(p->received&(1u<<s))){fa_put16(nak+10+missing*2,(uint16_t)s);missing++;}
                fa_put16(nak+8,(uint16_t)missing);send_route(t,id,PATH_PUNCH,CMD_PIECE_NAK,p->request,nak,10+missing*2);p->due=now+.5;
            }else{event(t,"REQUEST_TIMEOUT",id,p->piece);p->pending=0;p->bits[p->file]&=~(1u<<p->piece);struct connection *c=link_for(t,id);if(p->path==PATH_LAN_TCP && c)drop(c);}
        }
    }
    if(need_relay && !relay_for(t) && t->relay_ip && now>=t->relay_retry){t->relay_retry=now+2;dial(t,0,t->relay_ip,PORT_RELAY_TCP,1);}
    /* Send at most one subpacket per tick globally: >=1 ms pacing without sleep. */
    static const double pace=.001;
    for(int u=0;u<32;u++){struct upload *x=&t->uploads[u];if(!x->active || now<x->due)continue;
        if(!x->mask){x->active=0;continue;}int sub=0;while(!(x->mask&(1u<<sub)))sub++;
        size_t len=piece_len(t,x->file,x->piece),count=(len+1023)/1024,size=len-(size_t)sub*1024;if(size>1024)size=1024;
        unsigned char b[1038]={0};fa_put32(b,t->manifest.session_id);b[4]=(uint8_t)(x->file+1);fa_put16(b+6,(uint16_t)x->piece);fa_put16(b+8,(uint16_t)sub);fa_put16(b+10,(uint16_t)count);fa_put16(b+12,(uint16_t)size);
        memcpy(b+14,t->data[x->file]+x->piece*GEN_PIECE+sub*1024,size);
        if(send_route(t,x->id,x->path,CMD_PIECE_DATA,x->request,b,size+14)==0){x->mask&=(uint16_t)~(1u<<sub);if(x->path==PATH_RELAY)t->up_relay+=size;else t->up_udp+=size;}
        else{x->active=0;}
        for(int v=0;v<32;v++)if(t->uploads[v].active)t->uploads[v].due=now+pace;
        break;
    }
    if(!t->downloading || now<t->next_request)return;
    int scheduled=0;
    for(int turn=0;turn<4;turn++){int id=1+(t->cursor+turn)%4;if(id==t->id || (t->cs_only && id!=1))continue;
        struct peer *p=&t->peers[id];int path=route(t,id);if(!path || p->pending)continue;
        for(int f=0;f<3 && !p->pending;f++)for(unsigned k=0;k<t->manifest.files[f].piece_count;k++){
            if(!(p->bits[f]&(1u<<k)) || (t->have[f]&(1u<<k)))continue;
            int busy=0;for(int j=1;j<=4;j++)if(t->peers[j].pending && t->peers[j].file==f && t->peers[j].piece==(int)k)busy=1;
            if(busy)continue;
            unsigned char b[9];fa_put32(b,t->manifest.session_id);b[4]=(uint8_t)(f+1);fa_put16(b+5,(uint16_t)k);fa_put16(b+7,0xffff);
            if(++t->serial==0)++t->serial;
            if(send_route(t,id,path,CMD_PIECE_REQ,t->serial,b,9)<0)break;
            p->pending=1;p->file=f;p->piece=(int)k;p->path=path;p->request=t->serial;p->received=0;p->retries=0;p->due=now+(path==PATH_PUNCH?.5:5);
            event(t,"REQUEST",id,(int)k);scheduled=1;break;
        }
    }
    t->cursor=(t->cursor+1)%4;
    if(scheduled)t->next_request=now+t->delay_ms/1000.0;
    else if(now-t->waiting_log>5 && !(t->complete[0] && t->complete[1] && t->complete[2])){event(t,"WAITING_FOR_AVAILABLE_SOURCE",0,0);t->waiting_log=now;}
}
void tr_summary(struct transfer *t){unsigned long long total=t->up_tcp+t->up_udp+t->up_relay;double el=(t->dl_end>t->dl_start)?(t->dl_end-t->dl_start):-1.0;printf("TRANSFER_STATS peer=%d tcp_up=%llu udp_up=%llu relay_up=%llu total_up=%llu elapsed=%.3f hash_reject=%u",t->id,(unsigned long long)t->up_tcp,(unsigned long long)t->up_udp,(unsigned long long)t->up_relay,total,el,t->hash_reject);for(int id=1;id<=4;id++)printf(" from_%d=%llu",id,(unsigned long long)t->down[id]);puts("");}
int tr_should_punch(struct transfer *t,unsigned id){if(id<1 || id>4 || id==(unsigned)t->id || !t->raw_len || t->mode==2 || t->peers[id].fallback)return 0;struct connection *c=link_for(t,(int)id);return t->mode==1 || (!c && stamp()-t->peers[id].route_since>1.5);}
