#include "ui_state.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <arpa/inet.h>

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
    /* 真实证物文件路径由 transfer 装载清单后经 ui_set_file_content 写入 */
    f->img_path[0] = '\0';
    f->desc[0] = '\0';
}

int ui_set_file_status(struct ui_state *s, uint8_t file_id, int have,
                       const char *source_nick)
{
    if (file_id < 1 || file_id > UI_MAX_FILES) return -1;
    struct ui_file *f = &s->files[file_id - 1];
    if (!f->used) return -1;
    f->have = have ? 1 : 0;
    if (source_nick && source_nick[0]) {
        snprintf(f->source_nick, sizeof(f->source_nick), "%s", source_nick);
    } else if (!have) {
        f->source_nick[0] = '\0';
    }
    return 0;
}

void ui_set_file_content(struct ui_state *s, uint8_t file_id,
                         const char *img_path, const char *desc)
{
    if (file_id < 1 || file_id > UI_MAX_FILES) return;
    struct ui_file *f = &s->files[file_id - 1];
    if (!f->used) return;
    if (img_path && img_path[0])
        snprintf(f->img_path, sizeof(f->img_path), "%s", img_path);
    if (desc && desc[0])
        snprintf(f->desc, sizeof(f->desc), "%s", desc);
}

void ui_advance_phase(struct ui_state *s, enum ui_phase phase)
{
    if (phase >= UI_PHASE_IDLE && phase <= UI_PHASE_DONE)
        s->phase = phase;
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

/* ---- JSON 序列化（供 Web 案情板轮询；字段与 ui_state.h 一一对应） ---- */

static void json_str(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  fputs("\\\"", fp); break;
        case '\\': fputs("\\\\", fp); break;
        case '\n': fputs("\\n", fp); break;
        case '\r': fputs("\\r", fp); break;
        case '\t': fputs("\\t", fp); break;
        default:
            if (c < 0x20) fprintf(fp, "\\u%04x", c);
            else fputc(c, fp);
        }
    }
    fputc('"', fp);
}

static void json_ip(FILE *fp, uint32_t ip_n, const char *fallback)
{
    if (ip_n == 0) {
        json_str(fp, fallback);
        return;
    }
    struct in_addr ia;
    ia.s_addr = ip_n;
    json_str(fp, inet_ntoa(ia));
}

void ui_state_write_json(const struct ui_state *s, const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return;

    fprintf(fp, "{\"phase\":%d,\"room_id\":%u,\"session_id\":%u,"
                "\"my_peer_id\":%u,\"my_nick\":",
            s->phase, s->room_id, s->session_id, s->my_peer_id);
    json_str(fp, s->my_nick);
    fprintf(fp, ",\"room_online\":%d,\"path_type\":%d,\"status\":",
            s->room_online, s->path_type);
    json_str(fp, s->status);
    fprintf(fp, ",\"refl_text\":");
    json_str(fp, s->refl_text);

    fprintf(fp, ",\"events\":[");
    for (int i = 0; i < s->event_n; i++) {
        if (i) fputc(',', fp);
        json_str(fp, s->events[i]);
    }
    fprintf(fp, "]");

    fprintf(fp, ",\"peers\":[");
    int first = 1;
    for (int i = 0; i < UI_MAX_PEERS; i++) {
        const struct ui_peer *p = &s->peers[i];
        if (!p->used) continue;
        if (!first) fputc(',', fp);
        first = 0;
        fprintf(fp, "{\"peer_id\":%u,\"nick\":", p->peer_id);
        json_str(fp, p->nick);
        fprintf(fp, ",\"priv_ip\":");
        json_ip(fp, p->priv_ip_n, "-");
        fprintf(fp, ",\"tcp_port\":%u,\"udp_port\":%u,\"refl_ip\":",
                p->tcp_port_h, p->udp_port_h);
        json_ip(fp, p->refl_ip_n, "-");
        fprintf(fp, ",\"refl_port\":%u}", p->refl_port_h);
    }
    fprintf(fp, "]");

    fprintf(fp, ",\"files\":[");
    first = 1;
    for (int i = 0; i < UI_MAX_FILES; i++) {
        const struct ui_file *f = &s->files[i];
        if (!f->used) continue;
        if (!first) fputc(',', fp);
        first = 0;
        fprintf(fp, "{\"file_id\":%u,\"chapter\":%u,\"kind\":%u,\"size\":%u,\"hash_hex\":",
                f->file_id, f->chapter, f->kind, f->file_size);
        json_str(fp, f->hash_hex);
        fprintf(fp, ",\"have\":%d,\"source_nick\":", f->have);
        json_str(fp, f->source_nick);
        fprintf(fp, ",\"img_path\":");
        json_str(fp, f->img_path);
        fprintf(fp, ",\"desc\":");
        json_str(fp, f->desc);
        fprintf(fp, "}");
    }
    fprintf(fp, "]}\n");

    fclose(fp);
    rename(tmp, path);
}
