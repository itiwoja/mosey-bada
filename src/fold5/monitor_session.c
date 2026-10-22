/* Bounded receive-only session on a newly-created Fold5 monitor interface.
 * Never changes wlan0, channel, regulatory settings or firmware. Never sends
 * wireless frames. Always attempts deletion of the interface it created. */
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
int main(int argc,char **argv) {
    if(argc!=2 || strcmp(argv[1],"--capture-three-seconds")) {
        fprintf(stderr,"Usage: %s --capture-three-seconds\n",argv[0]); return 2;
    }
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
    packet=socket(AF_PACKET,SOCK_RAW|SOCK_CLOEXEC,htons(ETH_P_ALL));
    if(packet<0)goto cleanup;
    struct sockaddr_ll addr={.sll_family=AF_PACKET,.sll_protocol=htons(ETH_P_ALL),.sll_ifindex=index};
    if(bind(packet,(void *)&addr,sizeof(addr))<0)goto cleanup;
    timeout.tv_sec=0; timeout.tv_usec=250000;
    if(setsockopt(packet,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))<0)goto cleanup;
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
