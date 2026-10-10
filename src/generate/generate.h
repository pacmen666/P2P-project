#ifndef GEN_GENERATE_H
#define GEN_GENERATE_H

#include <stdint.h>
#include <stddef.h>

#define GEN_PIECE       16384          /* 逻辑分块 16 KiB（v1 第 4 节）*/
#define GEN_MAX_FILES   3
#define GEN_SHA256_LEN  32

struct gen_piece {
    uint8_t hash[GEN_SHA256_LEN];      /* 该逻辑分块 SHA-256，块号升序 */
};

struct gen_file {
    uint8_t  file_id;
    uint8_t  chapter;                  /* 1 或 2 */
    uint8_t  kind;                     /* 1 图, 2 文本 */
    uint32_t file_size;
    uint8_t  file_hash[GEN_SHA256_LEN];/* 整文件 SHA-256 */
    uint16_t piece_count;
    struct gen_piece *pieces;          /* piece_count 个 */
    char     path[384];                /* session_<id>/file_<file_id> */
};

struct gen_manifest {
    uint32_t session_id;
    uint8_t  seed_hash[GEN_SHA256_LEN];
    int      file_n;
    struct gen_file files[GEN_MAX_FILES];
};

/* 后端可替换点（方案 A -> B）：
   当前从磁盘 session_<id>/ 读成品。将来要做进程内 C 生成，只需另实现
   gen_generate_session(seed) 把 files[].path 换成内存缓冲，transfer 调用方
   无需改动。 */
int  gen_load_manifest(const char *session_dir, struct gen_manifest *m);
void gen_free_manifest(struct gen_manifest *m);

/* 取第 file_id 个文件、第 piece_idx 块；out 至少 GEN_PIECE 字节。
   返回实际字节数（末块可能更短），失败返回 -1。transfer 拿到后用
   m->files[].pieces[idx].hash 比对摘要即可。 */
int  gen_read_piece(const struct gen_manifest *m, uint8_t file_id,
                    uint16_t piece_idx, uint8_t *out, size_t *out_len);

#endif
