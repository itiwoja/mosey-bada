#define main monitor_session_entry
#include "../src/fold5/monitor_session.c"
#undef main
#include <assert.h>
#include <stdlib.h>
int main(void) {
    unsigned char frame[41]={0};
    frame[2]=8; frame[8]=0xd0;
    unsigned char action[]={127,0,0x17,0xf2,8};
    memcpy(frame+8+24,action,sizeof(action));
    assert(awdl_action(frame,37));
    for(size_t n=0;n<37;n++)assert(!awdl_action(frame,n));
    frame[9]=0x40;assert(!awdl_action(frame,37));frame[9]=0;
    frame[8]=0xd1;assert(!awdl_action(frame,37));frame[8]=0xd0;
    frame[9]=0x80;memset(frame+32,0,9);memcpy(frame+36,action,5);
    assert(awdl_action(frame,41));assert(!awdl_action(frame,40));
    srand(259);
    for(size_t n=1;n<512;n++){
        unsigned char *b=malloc(n);assert(b);
        for(size_t i=0;i<n;i++)b[i]=(unsigned char)rand();
        (void)awdl_action(b,n);free(b);
    }
    puts("AWDL action parser: valid, truncated, protected, HT-control and malformed inputs passed");
    return 0;
}
