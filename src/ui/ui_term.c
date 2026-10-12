#include "ui_state.h"

#include <stdio.h>
#include <arpa/inet.h>

static void ip_str(uint32_t ip_n, char *buf, size_t n)
{
    struct in_addr a;
    a.s_addr = ip_n;
    snprintf(buf, n, "%s", inet_ntoa(a));
}

void ui_term_draw(const struct ui_state *s)
{
    printf("\033[2J\033[H");
    printf("======== 碎片档案 / Fragment Archive ========\n");
    printf("阶段：%s    路径：%s    房间服务：%s\n",
           ui_phase_label(s->phase),
           ui_path_label(s->path_type),
           s->room_online ? "在线" : "离线/未知");
    printf("局号：");
    if (s->session_id)
        printf("%08X", s->session_id);
    else
        printf("(待清单)");
    printf("    房间：%u    我：%s (peer %u)\n",
           s->room_id, s->my_nick[0] ? s->my_nick : "-", s->my_peer_id);
    if (s->refl_text[0])
        printf("本机反射：%s\n", s->refl_text);
    printf("状态：%s\n", s->status);
    printf("--------------------------------------------\n");
    printf("成员\n");
    for (int i = 0; i < UI_MAX_PEERS; i++) {
        const struct ui_peer *p = &s->peers[i];
        if (!p->used) continue;
        char ip[32], rip[32];
        ip_str(p->priv_ip_n, ip, sizeof(ip));
        if (p->refl_ip_n) ip_str(p->refl_ip_n, rip, sizeof(rip));
        else snprintf(rip, sizeof(rip), "-");
        printf("  #%u %-16s 私网 %s:%u  UDP %u  反射 %s:%u%s\n",
               p->peer_id, p->nick, ip, p->tcp_port_h, p->udp_port_h,
               rip, p->refl_port_h,
               p->peer_id == s->my_peer_id ? "  <-我" : "");
    }
    printf("--------------------------------------------\n");
    printf("案情板（空板亦可；整文件摘要前缀，不含种子）\n");
    for (int i = 0; i < UI_MAX_FILES; i++) {
        const struct ui_file *f = &s->files[i];
        if (!f->used) {
            printf("  证物 %d：空白\n", i + 1);
            continue;
        }
        printf("  文件%d 章%d %s  %uB  [%s]  %s",
               f->file_id, f->chapter,
               f->kind == 1 ? "图" : (f->kind == 2 ? "文" : "?"),
               f->file_size, f->hash_hex,
               f->have ? "已有" : "缺失");
        if (f->source_nick[0])
            printf("  来源：%s", f->source_nick);
        printf("\n");
    }
    printf("--------------------------------------------\n");
    printf("最近事件（随板保留；完整日志见 logs/client.log）\n");
    if (s->event_n <= 0)
        printf("  （无）\n");
    else {
        for (int i = 0; i < s->event_n; i++)
            printf("  · %s\n", s->events[i]);
    }
    printf("--------------------------------------------\n");
    printf("命令：join/probe/announce | next | answer <答案> | choose <选项>\n");
    printf("  join 侦探 localhost     — 以玩家进房\n");
    printf("  join 房主 localhost host — 以房主进房\n");
    printf("  markhave <文件号> [来源]（联调注入） | redraw | quit\n");
    printf("============================================\n");
    fflush(stdout);
}
