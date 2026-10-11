/* 碎片档案 · 案卷装载（陈，方案 A）
 *
 * 只从磁盘 session_<id>/ 读成品（file_<id> + manifest.bin），不生成、不渲染图像。
 * manifest.bin 字节布局严格对齐 v1 约定 §5.4（大端）：
 *   session_id(4) seed_hash(32) file_n(1)
 *   文件项重复 file_n 次：file_id(1) chapter(1) kind(1) file_size(4)
 *                          file_sha256(32) piece_count(2) piece_hash × 32
 * 与线上 MANIFEST 正文相同；转移层（彭）按 pieces[].hash 校验即可。
 */
#include "generate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t rd_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

int gen_load_manifest(const char *session_dir, struct gen_manifest *m) {
    char path[512];
    snprintf(path, sizeof(path), "%s/manifest.bin", session_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t hdr[4 + 32 + 1];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return -1; }

    memset(m, 0, sizeof(*m));
    m->session_id = rd_be32(hdr);
    memcpy(m->seed_hash, hdr + 4, 32);
    m->file_n = hdr[36];
    if (m->file_n > GEN_MAX_FILES) m->file_n = GEN_MAX_FILES;

    for (int i = 0; i < m->file_n; i++) {
        struct gen_file *gf = &m->files[i];
        uint8_t e[1 + 1 + 1 + 4 + 32 + 2];
        if (fread(e, 1, sizeof(e), f) != sizeof(e)) { fclose(f); return -1; }
        gf->file_id   = e[0];
        gf->chapter   = e[1];
        gf->kind      = e[2];
        gf->file_size = rd_be32(e + 3);
        memcpy(gf->file_hash, e + 7, 32);
        gf->piece_count = rd_be16(e + 39);
        if (gf->piece_count) {
            gf->pieces = calloc(gf->piece_count, sizeof(struct gen_piece));
            if (!gf->pieces) { fclose(f); return -1; }
            size_t need = (size_t)gf->piece_count * 32;
            if (fread(gf->pieces, 1, need, f) != need) { fclose(f); return -1; }
        }
        snprintf(gf->path, sizeof(gf->path), "%s/file_%u", session_dir, gf->file_id);
    }
    fclose(f);
    return 0;
}

void gen_free_manifest(struct gen_manifest *m) {
    for (int i = 0; i < m->file_n; i++)
        free(m->files[i].pieces);
    m->file_n = 0;
}

int gen_read_piece(const struct gen_manifest *m, uint8_t file_id,
                   uint16_t piece_idx, uint8_t *out, size_t *out_len) {
    const struct gen_file *gf = NULL;
    for (int i = 0; i < m->file_n; i++)
        if (m->files[i].file_id == file_id) { gf = &m->files[i]; break; }
    if (!gf) return -1;
    if (piece_idx >= gf->piece_count) return -1;

    FILE *f = fopen(gf->path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)piece_idx * GEN_PIECE, SEEK_SET) != 0) { fclose(f); return -1; }

    size_t remain = gf->file_size - (size_t)piece_idx * GEN_PIECE;
    size_t want = remain < GEN_PIECE ? remain : GEN_PIECE;
    size_t got = fread(out, 1, want, f);
    fclose(f);
    if (got != want) return -1;

    *out_len = got;
    return 0;
}
