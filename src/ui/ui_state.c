#include "ui_state.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

void ui_state_init(struct ui_state *s)
{
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mu, NULL);
    s->phase = UI_PHASE_IDLE;
    s->path_type = PATH_UNKNOWN;
    s->room_online = 0;
    snprintf(s->status, sizeof(s->status), "未进房");
}

void ui_state_lock(struct ui_state *s) { pthread_mutex_lock(&s->mu); }
void ui_state_unlock(struct ui_state *s) { pthread_mutex_unlock(&s->mu); }

void ui_set_status(struct ui_state *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->status, sizeof(s->status), fmt, ap);
    va_end(ap);
}

void ui_push_event(struct ui_state *s, const char *fmt, ...)
{
    char line[UI_EVENT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (s->event_n < UI_MAX_EVENTS) {
        snprintf(s->events[s->event_n], UI_EVENT_MAX, "%s", line);
        s->event_n++;
    } else {
        memmove(s->events[0], s->events[1],
                (UI_MAX_EVENTS - 1) * UI_EVENT_MAX);
        snprintf(s->events[UI_MAX_EVENTS - 1], UI_EVENT_MAX, "%s", line);
    }
}

void ui_clear_peers(struct ui_state *s)
{
    memset(s->peers, 0, sizeof(s->peers));
}

void ui_set_peer(struct ui_state *s, const struct ui_peer *p)
{
    for (int i = 0; i < UI_MAX_PEERS; i++) {
        if (s->peers[i].used && s->peers[i].peer_id == p->peer_id) {
            s->peers[i] = *p;
            return;
        }
    }
    for (int i = 0; i < UI_MAX_PEERS; i++) {
        if (!s->peers[i].used) {
            s->peers[i] = *p;
            s->peers[i].used = 1;
            return;
        }
    }
}

void ui_apply_manifest_header(struct ui_state *s, uint32_t session_id,
                              const uint8_t *seed_hash_ignored)
{
    (void)seed_hash_ignored; /* 故意不展示、不复制到界面字段 */
    s->session_id = session_id;
    if (s->phase == UI_PHASE_IN_ROOM || s->phase == UI_PHASE_IDLE)
        s->phase = UI_PHASE_CH1;
}

static void hash_prefix_hex(const uint8_t h[32], char out[UI_HASH_HEX])
{
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[i * 2]     = hex[(h[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[h[i] & 0xF];
    }
    out[16] = '\0';
}

void ui_set_file_meta(struct ui_state *s, uint8_t file_id, uint8_t chapter,
                      uint8_t kind, uint32_t file_size,
                      const uint8_t file_hash[32])
{
    if (file_id < 1 || file_id > UI_MAX_FILES) return;
    struct ui_file *f = &s->files[file_id - 1];
    f->used = 1;
    f->file_id = file_id;
    f->chapter = chapter;
    f->kind = kind;
    f->file_size = file_size;
    hash_prefix_hex(file_hash, f->hash_hex);
    f->have = 0;
    f->source_nick[0] = '\0';
}

const char *ui_path_label(int path_type)
{
    switch (path_type) {
    case PATH_LAN_TCP:  return "本网直连";
    case PATH_PUNCH:    return "打洞";
    case PATH_RELAY:    return "中继";
    case PATH_ANNOUNCE: return "通告发现";
    default:            return "未知";
    }
}

const char *ui_phase_label(enum ui_phase p)
{
    switch (p) {
    case UI_PHASE_IDLE:   return "空闲";
    case UI_PHASE_IN_ROOM:return "在房";
    case UI_PHASE_CH1:    return "第一章";
    case UI_PHASE_QUIZ:   return "章间问答";
    case UI_PHASE_CH2:    return "第二章";
    case UI_PHASE_CHOICE: return "三项择一";
    case UI_PHASE_DONE:   return "结束";
    default:              return "?";
    }
}
