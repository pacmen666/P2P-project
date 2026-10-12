/* Test-only sendto fault injection. Never linked into the application.
 * LD_PRELOAD plus FA_TEST_UDP_DROP=once|all simulates missing PIECE_DATA. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
ssize_t sendto(int fd,const void *buf,size_t n,int flags,const struct sockaddr *addr,socklen_t len){
    static ssize_t (*real_sendto)(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
    static unsigned char seen[3][32][16];
    if(!real_sendto) *(void **)(&real_sendto)=dlsym(RTLD_NEXT,"sendto");
    if(!real_sendto)return -1;
    const unsigned char *b=buf;const char *mode=getenv("FA_TEST_UDP_DROP");
    if(mode && n>=22 && b[0]==0x46 && b[1]==0x41 && b[3]==0x0b){
        unsigned f=b[12],k=((unsigned)b[14]<<8)|b[15],sub=((unsigned)b[16]<<8)|b[17];
        if(!strcmp(mode,"all"))return (ssize_t)n;
        if(!strcmp(mode,"once") && f>=1 && f<=3 && k<32 && sub==3 && !seen[f-1][k][sub]){seen[f-1][k][sub]=1;return (ssize_t)n;}
    }
    return real_sendto(fd,buf,n,flags,addr,len);
}
