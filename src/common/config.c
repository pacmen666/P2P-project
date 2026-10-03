#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
struct config {
    char host[256];   // 主机名，比如 localhost
    char port[16];    // 端口，比如 37401
};
int load_config(const char *path, struct config *cfg);
int resolve_host(const char *host, const char *port, struct addrinfo **res);
void print_peer(int sockfd);
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);