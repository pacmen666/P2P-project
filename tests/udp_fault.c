/* Test-only sendto fault injection. Never linked into the application.
 * LD_PRELOAD plus FA_TEST_UDP_DROP=once|all simulates missing PIECE_DATA. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __APPLE__
#define FA_TEST_SENDTO fa_test_sendto
#else
#define FA_TEST_SENDTO sendto
#endif

ssize_t FA_TEST_SENDTO(int fd,const void *buf,size_t n,int flags,const struct sockaddr *addr,socklen_t len){
    static ssize_t (*real_sendto)(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
    static unsigned char seen[3][32][16];
    /* Calls from the interposing library use the original on macOS. */
#ifdef __APPLE__
    if(!real_sendto) real_sendto = sendto;
#else
    if(!real_sendto) *(void **)(&real_sendto)=dlsym(RTLD_NEXT,"sendto");
#endif
    if(!real_sendto)return -1;
    const unsigned char *b=buf;const char *mode=getenv("FA_TEST_UDP_DROP");
    if(mode && n>=22 && b[0]==0x46 && b[1]==0x41 && b[3]==0x0b){
        unsigned f=b[12],k=((unsigned)b[14]<<8)|b[15],sub=((unsigned)b[16]<<8)|b[17];
        if(!strcmp(mode,"all")){ fprintf(stderr, "FAULT_DROP\n"); return (ssize_t)n; }
        if(!strcmp(mode,"once") && f>=1 && f<=3 && k<32 && sub==3 && !seen[f-1][k][sub]){seen[f-1][k][sub]=1;{ fprintf(stderr, "FAULT_DROP\n"); return (ssize_t)n; }}
    }
    return real_sendto(fd,buf,n,flags,addr,len);
}

#ifdef __APPLE__
__attribute__((used))
static struct {
    const void *replacement;
    const void *original;
} fa_sendto_interpose
__attribute__((section("__DATA,__interpose"))) = {
    (const void *)&fa_test_sendto,
    (const void *)&sendto
};
#endif
