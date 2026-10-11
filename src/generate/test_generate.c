/* 碎片档案 · 案卷装载自测（陈）
 * 用法：./test_generate <session_dir>
 * 确认 C 侧能解析 Python 产出的 manifest.bin 并读块，供 transfer 联调前自检。
 */
#include "generate.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <session_dir>\n", argv[0]);
        return 2;
    }
    struct gen_manifest m;
    if (gen_load_manifest(argv[1], &m) != 0) {
        fprintf(stderr, "无法读取 %s/manifest.bin\n", argv[1]);
        return 1;
    }
    printf("session_id=%08x file_n=%d\n", m.session_id, m.file_n);
    for (int i = 0; i < m.file_n; i++) {
        struct gen_file *gf = &m.files[i];
        printf("  file_%u chapter=%u kind=%u size=%u pieces=%u path=%s\n",
               gf->file_id, gf->chapter, gf->kind, gf->file_size,
               gf->piece_count, gf->path);
    }
    uint8_t buf[GEN_PIECE];
    size_t len;
    for (int i = 0; i < m.file_n; i++) {
        uint8_t fid = m.files[i].file_id;
        if (gen_read_piece(&m, fid, 0, buf, &len) == 0)
            printf("  read file_%u piece0 -> %zu bytes OK\n", fid, len);
        else
            printf("  read file_%u piece0 FAILED\n", fid);
    }
    gen_free_manifest(&m);
    return 0;
}
