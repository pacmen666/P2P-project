#ifndef FA_STREAM_H
#define FA_STREAM_H
/* Week 1 POSIX transport helpers. Wire format remains protocol v1. */
#include <stddef.h>
#include <stdint.h>
#define FA_HEADER 8u
#define FA_BODY_MAX 65535u
#define FA_TX_CAP (4u * (FA_HEADER + FA_BODY_MAX))
struct fa_message { uint8_t cmd; uint16_t request, length; const uint8_t *body; };
typedef int (*fa_handler)(void *, const struct fa_message *);
struct fa_reader {
    uint8_t header[FA_HEADER], body[FA_BODY_MAX];
    size_t header_got, body_got;
    uint16_t body_len;
    int failed;
};
struct fa_writer { uint8_t bytes[FA_TX_CAP]; size_t offset, end; };
uint16_t fa_u16(const uint8_t *p);
uint32_t fa_u32(const uint8_t *p);
void fa_put16(uint8_t *p, uint16_t n);
void fa_put32(uint8_t *p, uint32_t n);
void fa_reader_init(struct fa_reader *r);
/* Callback body is borrowed until return. Negative callback return poisons reader. */
int fa_feed(struct fa_reader *r, const void *data, size_t n, fa_handler cb, void *ctx);
/* EOF at a message boundary is valid; EOF within a message is a truncation. */
int fa_reader_eof(const struct fa_reader *r);
int fa_pack(uint8_t *dst, size_t cap, uint8_t cmd, uint16_t request,
            const void *body, size_t len);
int fa_enqueue(struct fa_writer *w, uint8_t cmd, uint16_t request,
               const void *body, size_t len);
/* 1=empty, 0=would block, -1=error. Preserves offset across short writes. */
int fa_flush(struct fa_writer *w, int fd);
int fa_nonblock(int fd);
int fa_wake_open(int pair[2]);
/* Pipe is notification only. Store commands separately when UI is integrated. */
int fa_wake_signal(int fd);
int fa_wake_drain(int fd);
/* fgets 之后：去掉 \\r\\n、UTF-8 BOM、首尾空白 */
void fa_chomp_line(char *s);
#endif
