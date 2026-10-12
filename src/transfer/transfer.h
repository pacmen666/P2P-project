#ifndef FA_TRANSFER_H
#define FA_TRANSFER_H
/* Process-local API; the v1 wire protocol and include/common.h are unchanged.
 * All calls belong to the one network thread. UDP fd is borrowed from punch. */
#include "ui_state.h"
#include <sys/select.h>
struct transfer;
struct transfer *tr_create(struct ui_state *ui, int udp_fd, const char *root);
void tr_destroy(struct transfer *t);
int tr_load(struct transfer *t, const char *session_dir);
int tr_manifest(struct transfer *t, const unsigned char *body, size_t len);
const unsigned char *tr_manifest_bytes(struct transfer *t, size_t *len);
void tr_identity(struct transfer *t, unsigned id, unsigned room, uint32_t relay_ip);
void tr_member(struct transfer *t, const struct ui_peer *p);
unsigned short tr_port(struct transfer *t);
/* mode: 0=automatic TCP->authenticated UDP->relay, 1=UDP experiment, 2=relay test */
void tr_mode(struct transfer *t, int mode, int cs_only, int delay_ms);
void tr_download(struct transfer *t, int enabled);
void tr_accept_udp(struct transfer *t, unsigned id, const struct sockaddr_in *addr);
int tr_udp(struct transfer *t, const unsigned char *buf, size_t len, const struct sockaddr_in *addr);
void tr_sets(struct transfer *t, fd_set *rd, fd_set *wr, int *maxfd);
void tr_ready(struct transfer *t, fd_set *rd, fd_set *wr);
void tr_tick(struct transfer *t);
void tr_summary(struct transfer *t);
/* Whether discovery should try PUNCH; established LAN TCP always has priority. */
int tr_should_punch(struct transfer *t, unsigned id);

#endif
