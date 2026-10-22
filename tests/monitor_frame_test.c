#define main monitor_session_entry
#include "../src/fold5/monitor_session.c"
#undef main
#include <assert.h>
#include <stdlib.h>
/* radiotap: [0..3] header, present at 4; builds TSFT(8)+FLAGS+RATE+CHANNEL after one present word. */
static size_t mk_rt(unsigned char *b, unsigned flags, unsigned freq) {
    memset(b,0,32); b[2]=8+8+1+1+2+4-0; /* patched below */
    b[4]=0x0f; b[16]=(unsigned char)flags; b[17]=0x0c;
    b[18]=(unsigned char)freq; b[19]=(unsigned char)(freq>>8); /* channel at 18 (align 2) */
    b[2]=22; return 22;
}
static size_t mk_data(unsigned char *b, int qos, int htc, int retry) {
    static const unsigned char bssid[6]={0x00,0x25,0x00,0xff,0x94,0x73},llc[8]={0xaa,0xaa,3,0,0x17,0xf2,8,0};
    memset(b,0,64); b[0]=qos?0x88:0x08; b[1]=(retry?0x08:0)|(htc?0x80:0);
    for(int i=0;i<6;i++)b[4+i]=(unsigned char)(0x10+i);
    memcpy(b+16,bssid,6);
    size_t h=24+(qos?2+(htc?4:0):0); memcpy(b+h,llc,8); memset(b+h+8,0x55,10);
    return h+18;
}
static void radiotap_tests(void) {
    unsigned char f[128]; size_t rt; int fcs; unsigned freq;
    mk_rt(f,0x10,5220);
    assert(radiotap(f,22,&rt,&fcs,&freq)&&rt==22&&fcs==1&&freq==5220);
    mk_rt(f,0x00,2437);
    assert(radiotap(f,22,&rt,&fcs,&freq)&&fcs==0&&freq==2437);
    assert(!radiotap(f,21,&rt,&fcs,&freq)); /* length exceeds buffer */
    memset(f,0,32); f[2]=8; f[4]=0x0a; /* FLAGS+CHANNEL, short header: channel overruns */
    assert(!radiotap(f,8,&rt,&fcs,&freq));
    memset(f,0,32); f[2]=16; f[4]=0x0a; f[8]=0x10; f[10]=0x6a; f[11]=0x14; f[9]=0; /* FLAGS@8, CHANNEL@10 */
    assert(radiotap(f,16,&rt,&fcs,&freq)&&fcs&&freq==5226);
    /* extended present word: bit31 set, fields start after both words (offset 12) */
    memset(f,0,32); f[2]=18; f[4]=0x02; f[7]=0x80; f[12]=0x10; f[8]=0; 
    assert(radiotap(f,18,&rt,&fcs,&freq)&&fcs&&freq==0);
    f[7]=0x80; f[2]=10; assert(!radiotap(f,10,&rt,&fcs,&freq)); /* second present word missing */
    f[0]=1; assert(!radiotap(f,18,&rt,&fcs,&freq));
}
static void data_tests(void) {
    unsigned char f[160]; struct awdl_info a;
    for(int qos=0;qos<2;qos++)for(int htc=0;htc<=qos;htc++)for(int fcs=0;fcs<2;fcs++){
        size_t rt=mk_rt(f,fcs?0x10:0,5220), n=mk_data(f+rt,qos,htc,1);
        size_t total=rt+n+(fcs?4:0), hdr=24+(qos?2+(htc?4:0):0);
        assert(awdl_data(f,total,rt,fcs,&a));
        assert(a.dst[0]==0x10&&a.dst[5]==0x15&&a.retry==1&&a.payload==n-hdr);
        mk_data(f+rt,qos,htc,0); assert(awdl_data(f,total,rt,fcs,&a)&&!a.retry);
        for(size_t k=0;k<total;k++){ /* truncated frames must not crash; short ones are rejected */
            int r=awdl_data(f,k,rt,fcs,&a); if(k<rt+hdr+8+(fcs?4:0))assert(!r);
        }
    }
    size_t rt=mk_rt(f,0,5220), n=mk_data(f+rt,1,0,0);
    f[rt+16]^=1; assert(!awdl_data(f,rt+n,rt,0,&a)); f[rt+16]^=1;      /* addr3 differs */
    f[rt+24+2+7]^=1; assert(!awdl_data(f,rt+n,rt,0,&a)); f[rt+24+2+7]^=1; /* LLC differs */
    f[rt+1]|=0x01; assert(!awdl_data(f,rt+n,rt,0,&a)); f[rt+1]&=~0x01;  /* ToDS */
    f[rt+1]|=0x40; assert(!awdl_data(f,rt+n,rt,0,&a)); f[rt+1]&=~0x40;  /* protected */
    f[rt]=0xd0; assert(!awdl_data(f,rt+n,rt,0,&a));                      /* management */
    f[rt]=0x80; assert(!awdl_data(f,rt+n,rt,0,&a));                      /* QoS null-ish subtype 0xc? not 0x88 */
    f[rt]=0x88; assert(awdl_data(f,rt+n,rt,0,&a));
}
static void arg_tests(void) {
    struct cfg c; unsigned char m[6];
    char *ok[]={"p","--sniff","5220","60","--width","80","--target","AA:bb:cc:00:11:22","--pcap","x.pcap"};
    assert(parse_sniff(10,ok,&c)&&c.freq==5220&&c.center==5210&&c.width==80&&c.has_target&&c.target[0]==0xaa&&c.target[5]==0x22);
    char *a1[]={"p","--sniff","5220","1"}; assert(parse_sniff(4,a1,&c)&&c.center==5220&&c.width==20&&!c.has_target);
    char *a2[]={"p","--sniff","5220","1801"}; assert(!parse_sniff(4,a2,&c));
    char *a3[]={"p","--sniff","5220","0"}; assert(!parse_sniff(4,a3,&c));
    char *a4[]={"p","--sniff","100","5"}; assert(!parse_sniff(4,a4,&c));
    char *a5[]={"p","--sniff","5220","5","--bogus","1"}; assert(!parse_sniff(6,a5,&c));
    char *a6[]={"p","--sniff","5220","5","--width"}; assert(!parse_sniff(5,a6,&c));
    char *a7[]={"p","--sniff","5220","5","--width","60"}; assert(!parse_sniff(6,a7,&c));
    char *a8[]={"p","--sniff","5220x","5"}; assert(!parse_sniff(4,a8,&c));
    char *a9[]={"p","--sniff","2437","5","--width","80"}; assert(!parse_sniff(6,a9,&c));
    char *b1[]={"p","--sniff","5220","5","--target","aa:bb:cc:dd:ee"}; assert(!parse_sniff(6,b1,&c));
    char *b2[]={"p","--sniff","5220","5","--width","40"}; assert(parse_sniff(6,b2,&c)&&c.center==5230);
    char *b3[]={"p","--sniff","5240","5","--width","40"}; assert(parse_sniff(6,b3,&c)&&c.center==5230);
    char *b4[]={"p","--sniff","5220","5","--width","80","--center","5210"}; assert(parse_sniff(8,b4,&c)&&c.center==5210);
    assert(parse_mac("00:25:00:ff:94:73",m)&&m[3]==0xff);
    assert(!parse_mac("00-25-00-ff-94-73",m)&&!parse_mac("00:25:00:ff:94:7g",m)&&!parse_mac("",m));
    assert(center80(5180)==5210&&center80(5240)==5210&&center80(5805)==5775&&center80(5250)==0&&center80(5500)==5530);
}
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
    radiotap_tests();data_tests();arg_tests();
    for(size_t n=1;n<512;n++){
        unsigned char *b=malloc(n);assert(b);size_t rt;int fcs;unsigned fq;struct awdl_info a;
        for(int pass=0;pass<2;pass++){
            for(size_t i=0;i<n;i++)b[i]=(unsigned char)rand();
            if(pass&&n>3){b[0]=0;b[2]=(unsigned char)(8+rand()%8);b[3]=0;}
            if(radiotap(b,n,&rt,&fcs,&fq)){(void)awdl_data(b,n,rt,fcs,&a);}
        }
        free(b);
    }
    puts("AWDL action parser: valid, truncated, protected, HT-control and malformed inputs passed");
    return 0;
}
