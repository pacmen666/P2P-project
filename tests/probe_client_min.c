#include "common.h"
#include "punch.h"
#include "log.h"
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>

int main(void)
{
    struct punch_sock ps;
    log_init("logs/probe_min.log");
    if (punch_open(&ps) < 0) return 1;
    if (punch_send_probe(&ps, "127.0.0.1", inet_addr("127.0.0.1"), 1, 2) < 0)
        return 2;
    for (int i = 0; i < 50; i++) {
        uint8_t buf[256];
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(ps.fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &slen);
        if (n >= 8) {
            printf("got %zd from %s:%u cmd=0x%02x\n", n,
                   inet_ntoa(src.sin_addr), ntohs(src.sin_port), buf[3]);
            return 0;
        }
        usleep(50000);
    }
    printf("timeout\n");
    return 3;
}
