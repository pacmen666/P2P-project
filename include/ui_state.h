#ifndef UI_STATE_H
#define UI_STATE_H

#include "common.h"
#include <pthread.h>
#include <stdint.h>

#define UI_MAX_PEERS  4
#define UI_MAX_FILES  3
#define UI_MAX_EVENTS 6
#define UI_NICK_MAX   16
#define UI_HASH_HEX   17  /* 前 8 字节 → 16 hex + NUL；禁止展示种子 */
#define UI_EVENT_MAX  96

enum ui_phase {
    UI_PHASE_IDLE = 0,
    UI_PHASE_IN_ROOM,
    UI_PHASE_CH1,
    UI_PHASE_QUIZ,
    UI_PHASE_CH2,
    UI_PHASE_CHOICE,
    UI_PHASE_DONE
};

struct ui_peer {
    int      used;
    uint8_t  peer_id;
    char     nick[UI_NICK_MAX + 1];
    uint32_t priv_ip_n;
    uint16_t tcp_port_h;
    uint16_t udp_port_h;
    uint32_t refl_ip_n;
    uint16_t refl_port_h;
};

struct ui_file {
    int      used;
    uint8_t  file_id;
    uint8_t  chapter;
    uint8_t  kind;
    uint32_t file_size;
    char     hash_hex[UI_HASH_HEX]; /* 整文件摘要前缀，非种子 */
    int      have;                  /* 0 缺失 1 已有；第1周多为 0 */
    char     source_nick[UI_NICK_MAX + 1];
};

struct ui_state {
    pthread_mutex_t mu;
    enum ui_phase   phase;
    uint16_t        room_id;
    uint32_t        session_id;     /* 局号；0 表示尚未收到 MANIFEST */
    uint8_t         my_peer_id;
    char            my_nick[UI_NICK_MAX + 1];
    int             room_online;
    int             path_type;      /* PATH_* */
    char            status[128];    /* 一行状态，无种子 */
    char            refl_text[64];  /* 本机反射地址展示 */
    char            events[UI_MAX_EVENTS][UI_EVENT_MAX]; /* 环缓，重绘不丢 */
    int             event_n;
    struct ui_peer  peers[UI_MAX_PEERS];
    struct ui_file  files[UI_MAX_FILES];
};

void ui_state_init(struct ui_state *s);
void ui_state_lock(struct ui_state *s);
void ui_state_unlock(struct ui_state *s);

void ui_set_status(struct ui_state *s, const char *fmt, ...);
void ui_push_event(struct ui_state *s, const char *fmt, ...);
void ui_clear_peers(struct ui_state *s);
void ui_set_peer(struct ui_state *s, const struct ui_peer *p);
void ui_apply_manifest_header(struct ui_state *s, uint32_t session_id,
                              const uint8_t *seed_hash_ignored);
void ui_set_file_meta(struct ui_state *s, uint8_t file_id, uint8_t chapter,
                      uint8_t kind, uint32_t file_size,
                      const uint8_t file_hash[32]);

const char *ui_path_label(int path_type);
const char *ui_phase_label(enum ui_phase p);

/* 终端案情板；不打印种子或 seed_hash 原文 */
void ui_term_draw(const struct ui_state *s);

#endif
