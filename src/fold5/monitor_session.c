/* Bounded receive-only session on a newly-created Fold5 monitor interface.
 * Never changes wlan0, regulatory settings or firmware; --sniff sets the channel
 * of the monitor it created only. Never sends wireless frames. Always attempts
 * deletion of the interface it created. */
#define _GNU_SOURCE
#include <errno.h>
#include <arpa/inet.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define IFNAME "fold5mon"
static volatile sig_atomic_t stopped;
static uint32_t sequence;
static void stop_session(int sig) { (void)sig; stopped = 1; }
static size_t put_attr(unsigned char *buf, size_t at, uint16_t type,
                       const void *data, size_t size) {
    struct nlattr *a = (void *)(buf + at);
    a->nla_type = type; a->nla_len = NLA_HDRLEN + size;
    memcpy(buf + at + NLA_HDRLEN, data, size);
    return at + NLA_ALIGN(a->nla_len);
}
static int exchange(int fd, uint16_t family, uint8_t command,
                    unsigned char *buf, size_t length, int resolve) {
    struct nlmsghdr *h = (void *)buf;
    struct genlmsghdr *g = (void *)(buf + NLMSG_HDRLEN);
    h->nlmsg_len = length; h->nlmsg_type = family;
    h->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK; h->nlmsg_seq = ++sequence;
    g->cmd = command; g->version = 1;
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    if (sendto(fd, buf, length, 0, (void *)&kernel, sizeof(kernel)) < 0) return -errno;
    for (int count = 0; count < 16; count++) {
        unsigned char response[16384]; struct sockaddr_nl peer = {0};
        struct iovec io = {response, sizeof(response)};
        struct msghdr msg = {.msg_name=&peer,.msg_namelen=sizeof(peer),.msg_iov=&io,.msg_iovlen=1};
        ssize_t n = recvmsg(fd, &msg, 0);
        if (n < 0) return -errno;
        if (peer.nl_pid || (msg.msg_flags & MSG_TRUNC)) return -EPROTO;
        unsigned int left = n;
        for (struct nlmsghdr *r=(void *)response; NLMSG_OK(r,left); r=NLMSG_NEXT(r,left)) {
            if (r->nlmsg_seq != sequence) continue;
            if (r->nlmsg_type == NLMSG_ERROR) {
                if (r->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) return -EPROTO;
                return ((struct nlmsgerr *)NLMSG_DATA(r))->error;
            }
            if (!resolve || r->nlmsg_len < NLMSG_HDRLEN+GENL_HDRLEN) continue;
            size_t attrs=r->nlmsg_len-NLMSG_HDRLEN-GENL_HDRLEN;
            const struct nlattr *a=(void *)((unsigned char *)NLMSG_DATA(r)+GENL_HDRLEN);
            while (attrs >= NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= attrs) {
                if ((a->nla_type & NLA_TYPE_MASK)==CTRL_ATTR_FAMILY_ID && a->nla_len>=NLA_HDRLEN+2) {
                    uint16_t id; memcpy(&id,(const unsigned char *)a+NLA_HDRLEN,2); return id;
                }
                size_t step=NLA_ALIGN(a->nla_len); if(step>attrs)break;
                attrs-=step; a=(void *)((const unsigned char *)a+step);
            }
        }
    }
    return -ETIMEDOUT;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
/* Counts only vendor-specific AWDL action headers. Does not retain payload. */
static int awdl_action(const unsigned char *frame, size_t size) {
    if(size<8 || frame[0]!=0)return 0;
    size_t rt=frame[2]+((size_t)frame[3]<<8);
    if(rt<8 || rt>size || size-rt<24)return 0;
    const unsigned char *p=frame+rt;
    size_t header=24+((p[1]&0x80)?4:0);
    if(p[0]!=0xd0 || (p[1]&0x40) || size-rt<header+5)return 0;
    return p[header]==127 && p[header+1]==0 && p[header+2]==0x17 && p[header+3]==0xf2 && p[header+4]==8;
}
/* Pure parsers. radiotap(): length, FCS flag and channel MHz from the first present word
 * (TSFT 8@8, FLAGS 1, RATE 1, CHANNEL 2+2@2; later present words are skipped, not decoded). */
static int radiotap(const unsigned char *f, size_t size, size_t *rt, int *fcs, unsigned *freq) {
    *rt=0;*fcs=0;*freq=0;
    if(size<8 || f[0]!=0)return 0;
    size_t len=f[2]+((size_t)f[3]<<8);
    if(len<8 || len>size)return 0;
    uint32_t first=0,w; size_t pos=4;
    do{
        if(pos+4>len)return 0;
        w=f[pos]|((uint32_t)f[pos+1]<<8)|((uint32_t)f[pos+2]<<16)|((uint32_t)f[pos+3]<<24);
        if(pos==4)first=w;
        pos+=4;
    }while(w&0x80000000u);
    if(first&1){pos=(pos+7)&~(size_t)7; if(pos+8>len)return 0; pos+=8;}
    if(first&2){if(pos+1>len)return 0; *fcs=!!(f[pos]&0x10); pos++;}
    if(first&4){if(pos+1>len)return 0; pos++;}
    if(first&8){pos=(pos+1)&~(size_t)1; if(pos+4>len)return 0; *freq=f[pos]|((unsigned)f[pos+1]<<8);}
    *rt=len; return 1;
}
struct awdl_info { unsigned char dst[6]; size_t payload; int retry; };
/* AWDL data: plain/QoS data, ToDS=FromDS=0, unprotected, addr3 = AWDL BSSID, LLC aa aa 03 00 17 f2 08 00.
 * payload = bytes after the 802.11 header (LLC included, FCS excluded). */
static int awdl_data(const unsigned char *frame, size_t size, size_t rt, int fcs, struct awdl_info *out) {
    static const unsigned char bssid[6]={0x00,0x25,0x00,0xff,0x94,0x73},llc[8]={0xaa,0xaa,3,0,0x17,0xf2,8,0};
    if(fcs){if(size<4)return 0; size-=4;}
    if(rt<8 || rt>size || size-rt<24)return 0;
    const unsigned char *p=frame+rt; size_t left=size-rt;
    int qos=p[0]==0x88;
    if(p[0]!=0x08 && !qos)return 0;
    if((p[1]&0x03) || (p[1]&0x40) || memcmp(p+16,bssid,6))return 0;
    size_t header=24+(qos?2+((p[1]&0x80)?4:0):0);
    if(left<header+8 || memcmp(p+header,llc,8))return 0;
    memcpy(out->dst,p+4,6); out->payload=left-header; out->retry=!!(p[1]&0x08);
    return 1;
}
static int parse_num(const char *s, long lo, long hi, long *v) {
    char *end; errno=0; long x=strtol(s,&end,10);
    if(errno || end==s || *end || x<lo || x>hi)return 0;
    *v=x; return 1;
}
static int parse_mac(const char *s, unsigned char *mac) {
    if(strlen(s)!=17)return 0;
    for(int i=0;i<6;i++){
        int v=0;
        for(int j=0;j<2;j++){
            char c=s[i*3+j]; int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
            if(d<0)return 0;
            v=v*16+d;
        }
        if(i<5 && s[i*3+2]!=':')return 0;
        mac[i]=(unsigned char)v;
    }
    return 1;
}
/* 80MHz block centre for a 5GHz primary frequency, 0 if none. */
static int center80(int f) {
    static const int blocks[][3]={{5180,5240,5210},{5260,5320,5290},{5500,5560,5530},{5580,5640,5610},{5660,5720,5690},{5745,5805,5775}};
    for(size_t i=0;i<sizeof(blocks)/sizeof(blocks[0]);i++)if(f>=blocks[i][0]&&f<=blocks[i][1])return blocks[i][2];
    return 0;
}
/* 40MHz centre: primary is the lower channel of its pair when (ch-36)%8==0 (ch<=144) or (ch-149)%8==0. */
static int center40(int f) {
    if(f<5180 || f>5825 || (f-5000)%5)return 0;
    int ch=(f-5000)/5, lower=ch<=144?(ch-36)%8==0:(ch-149)%8==0;
    return lower?f+10:f-10;
}
struct cfg { int freq,seconds,width,center,has_target; unsigned char target[6]; const char *pcap; };
/* Returns 1 for valid sniff arguments (argv[1] is --sniff), 0 for a usage error. */
static int parse_sniff(int argc, char **argv, struct cfg *c) {
    long v; memset(c,0,sizeof(*c)); c->width=20;
    if(argc<4 || !parse_num(argv[2],2412,7125,&v))return 0;
    c->freq=(int)v;
    if(!parse_num(argv[3],1,1800,&v))return 0;
    c->seconds=(int)v;
    for(int i=4;i<argc;i+=2){
        if(i+1>=argc)return 0;
        if(!strcmp(argv[i],"--target")){if(!parse_mac(argv[i+1],c->target))return 0; c->has_target=1;}
        else if(!strcmp(argv[i],"--width")){if(!parse_num(argv[i+1],20,80,&v)||(v!=20&&v!=40&&v!=80))return 0; c->width=(int)v;}
        else if(!strcmp(argv[i],"--center")){if(!parse_num(argv[i+1],4000,7200,&v))return 0; c->center=(int)v;}
        else if(!strcmp(argv[i],"--pcap")){if(!argv[i+1][0])return 0; c->pcap=argv[i+1];}
        else return 0;
    }
    if(!c->center)c->center=c->width==20?c->freq:c->width==40?center40(c->freq):center80(c->freq);
    return c->center!=0;
}
/* Sets the channel of the monitor we created (never wlan0). SET_WIPHY first, SET_CHANNEL if that fails;
 * returns the final errno value (0 on success). */
static int set_channel(int fd, int family, unsigned index, const struct cfg *c) {
    static const uint8_t cmds[2]={NL80211_CMD_SET_WIPHY,NL80211_CMD_SET_CHANNEL};
    static const char *names[2]={"SET_WIPHY","SET_CHANNEL"};
    uint32_t freq=c->freq,width=c->width==20?NL80211_CHAN_WIDTH_20:c->width==40?NL80211_CHAN_WIDTH_40:NL80211_CHAN_WIDTH_80,center=c->center;
    int result=0;
    for(int i=0;i<2;i++){
        unsigned char req[256]={0}; size_t len=NLMSG_HDRLEN+GENL_HDRLEN;
        len=put_attr(req,len,NL80211_ATTR_IFINDEX,&index,4);
        len=put_attr(req,len,NL80211_ATTR_WIPHY_FREQ,&freq,4);
        len=put_attr(req,len,NL80211_ATTR_CHANNEL_WIDTH,&width,4);
        len=put_attr(req,len,NL80211_ATTR_CENTER_FREQ1,&center,4);
        result=-exchange(fd,family,cmds[i],req,len,0);
        printf("set_channel_attempt cmd=%s errno=%d\n",names[i],result);
        if(!result)break;
    }
    printf("set_channel_result=%d freq=%d width=%d center=%d\n",result,c->freq,c->width,c->center);
    fflush(stdout); return result;
}
struct dst { unsigned char mac[6]; unsigned long long frames,bytes; };
static struct {
    unsigned long long frames,action,data,to_target,target_bytes,target_retry;
    unsigned chan[64],chan_n[64]; struct dst dst[64]; int dsts;
} S;
static unsigned top_chan(void) {
    unsigned best=0,n=0;
    for(int i=0;i<64;i++)if(S.chan_n[i]>n){n=S.chan_n[i];best=S.chan[i];}
    return best;
}
static void count_frame(const unsigned char *f, size_t n, const struct cfg *c) {
    size_t rt; int fcs; unsigned freq; struct awdl_info a;
    S.frames++;
    if(radiotap(f,n,&rt,&fcs,&freq) && freq){
        int i=0; while(i<64&&S.chan_n[i]&&S.chan[i]!=freq)i++;
        if(i<64){S.chan[i]=freq;S.chan_n[i]++;}
    } else rt=0;
    S.action+=awdl_action(f,n);
    if(!rt || !awdl_data(f,n,rt,fcs,&a))return;
    S.data++;
    if(c->has_target && !memcmp(a.dst,c->target,6)){S.to_target++;S.target_bytes+=a.payload;S.target_retry+=a.retry;}
    int i=0; while(i<S.dsts&&memcmp(S.dst[i].mac,a.dst,6))i++;
    if(i==S.dsts){if(i==64)return; memcpy(S.dst[i].mac,a.dst,6);S.dsts++;}
    S.dst[i].frames++; S.dst[i].bytes+=a.payload;
}
static void print_stats(const char *tag, int t) {
    printf("%s t=%d frames=%llu awdl_action=%llu awdl_data=%llu to_target=%llu target_bytes=%llu target_retry=%llu chan=%u\n",
           tag,t,S.frames,S.action,S.data,S.to_target,S.target_bytes,S.target_retry,top_chan());
    fflush(stdout);
}
/* Receive-only counting loop. Returns 0 on success, -1 on a receive error. */
static int sniff(int packet, const struct cfg *c) {
    FILE *pcap=NULL; int rc=0;
    if(c->pcap){
        pcap=fopen(c->pcap,"wb");
        if(!pcap)printf("pcap_open_errno=%d\n",errno);
        else{
            uint32_t h[6]={0xa1b2c3d4u,0x00040002u,0,0,256,127}; /* version 2.4 as two LE shorts */
            fwrite(h,sizeof(h),1,pcap);
        }
    }
    double start=now(),until=start+c->seconds,next=start+5;
    while(!stopped && now()<until){
        unsigned char frame[8192]; ssize_t n=recv(packet,frame,sizeof(frame),0);
        if(n>0){
            count_frame(frame,(size_t)n,c);
            if(pcap){
                struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
                uint32_t r[4]={(uint32_t)ts.tv_sec,(uint32_t)(ts.tv_nsec/1000),n>256?256:(uint32_t)n,(uint32_t)n};
                fwrite(r,sizeof(r),1,pcap); fwrite(frame,r[2],1,pcap);
            }
        } else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){rc=-1;break;}
        double t=now();
        if(t>=next){print_stats("sniff",(int)(next-start)); next+=5; if(pcap)fflush(pcap);}
    }
    print_stats("sniff_done",(int)(now()-start));
    for(int k=0;k<8;k++){
        int best=-1;
        for(int i=0;i<S.dsts;i++)if(S.dst[i].frames&&(best<0||S.dst[i].frames>S.dst[best].frames))best=i;
        if(best<0)break;
        const unsigned char *m=S.dst[best].mac;
        printf("top_dst %02x:%02x:%02x:%02x:%02x:%02x frames=%llu bytes=%llu\n",m[0],m[1],m[2],m[3],m[4],m[5],S.dst[best].frames,S.dst[best].bytes);
        S.dst[best].frames=0;
    }
    if(pcap)fclose(pcap);
    fflush(stdout); return rc;
}
int main(int argc,char **argv) {
    struct cfg cfg; int mode=argc>=2 && !strcmp(argv[1],"--sniff");
    if(mode ? !parse_sniff(argc,argv,&cfg) : (argc!=2 || strcmp(argv[1],"--capture-three-seconds"))) {
        fprintf(stderr,"Usage: %s --capture-three-seconds\n       %s --sniff FREQ_MHZ SECONDS [--target MAC] [--width 20|40|80] [--center MHZ] [--pcap PATH]\n",argv[0],argv[0]); return 2;
    }
    setvbuf(stdout,NULL,_IOLBF,0);
    if(getuid()!=0 || if_nametoindex(IFNAME)) { puts("precondition_failed=root_required_or_interface_exists"); return 2; }
    signal(SIGINT,stop_session); signal(SIGTERM,stop_session);
    int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_GENERIC);
    if(fd<0)return 2;
    struct timeval timeout={.tv_sec=2};
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))<0){close(fd);return 2;}
    struct sockaddr_nl local={.nl_family=AF_NETLINK};
    if(bind(fd,(void *)&local,sizeof(local))<0){close(fd);return 2;}
    unsigned char req[256]={0}; size_t base=NLMSG_HDRLEN+GENL_HDRLEN;
    const char name[]="nl80211";
    size_t len=put_attr(req,base,CTRL_ATTR_FAMILY_NAME,name,sizeof(name));
    int family=exchange(fd,GENL_ID_CTRL,CTRL_CMD_GETFAMILY,req,len,1);
    if(family<=0){printf("resolve_error=%d\n",family);close(fd);return 2;}
    memset(req,0,sizeof(req)); uint32_t phy=0,type=NL80211_IFTYPE_MONITOR;
    len=put_attr(req,base,NL80211_ATTR_WIPHY,&phy,4);
    len=put_attr(req,len,NL80211_ATTR_IFTYPE,&type,4);
    len=put_attr(req,len,NL80211_ATTR_IFNAME,IFNAME,sizeof(IFNAME));
    int result=exchange(fd,family,NL80211_CMD_NEW_INTERFACE,req,len,0);
    printf("monitor_create_result=%d (%s)\n",result,result<0?strerror(-result):"success");
    if(result){close(fd);return 3;}
    unsigned index=if_nametoindex(IFNAME); int packet=-1,ctl=-1,rc=4;
    if(!index){puts("created_interface_index_unavailable");goto cleanup;}
    ctl=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
    struct ifreq ifr={0}; strcpy(ifr.ifr_name,IFNAME);
    if(ctl<0 || ioctl(ctl,SIOCGIFFLAGS,&ifr)<0)goto cleanup;
    ifr.ifr_flags|=IFF_UP;
    if(ioctl(ctl,SIOCSIFFLAGS,&ifr)<0){printf("monitor_up_errno=%d\n",errno);goto cleanup;}
    if(mode)set_channel(fd,family,index,&cfg);
    packet=socket(AF_PACKET,SOCK_RAW|SOCK_CLOEXEC,htons(ETH_P_ALL));
    if(packet<0)goto cleanup;
    struct sockaddr_ll addr={.sll_family=AF_PACKET,.sll_protocol=htons(ETH_P_ALL),.sll_ifindex=index};
    if(bind(packet,(void *)&addr,sizeof(addr))<0)goto cleanup;
    timeout.tv_sec=0; timeout.tv_usec=250000;
    if(setsockopt(packet,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))<0)goto cleanup;
    if(mode){
        if(sniff(packet,&cfg)<0)goto cleanup;
        puts("wireless_transmit=never");rc=0;goto cleanup;
    }
    unsigned frames=0,awdl=0; double until=now()+3;
    while(!stopped && now()<until){
        unsigned char frame[8192]; ssize_t n=recv(packet,frame,sizeof(frame),0);
        if(n>0){frames++;awdl+=awdl_action(frame,n);}
        else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)goto cleanup;
    }
    printf("received_frames=%u\nawdl_action_frames=%u\nwireless_transmit=never\n",frames,awdl);rc=0;
cleanup:
    if(packet>=0)close(packet);
    if(ctl>=0)close(ctl);
    if(index){
        memset(req,0,sizeof(req)); len=put_attr(req,base,NL80211_ATTR_IFINDEX,&index,4);
        result=exchange(fd,family,NL80211_CMD_DEL_INTERFACE,req,len,0);
        printf("monitor_delete_result=%d\n",result);if(result)rc=5;
    }
    close(fd);return rc;
}
