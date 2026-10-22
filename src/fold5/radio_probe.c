/* Read-only Fold5 radio capability probe. Never changes interface state,
 * sends wireless frames, scans, or issues vendor commands. */
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static uint32_t seq;
static int fd;
static unsigned vendor_commands, google_commands;
static int attr_ok(const struct nlattr *a, size_t left) {
    return left >= sizeof(*a) && a->nla_len >= sizeof(*a) && a->nla_len <= left;
}
static const void *payload(const struct nlattr *a) {
    return (const unsigned char *)a + NLA_HDRLEN;
}
static size_t plen(const struct nlattr *a) { return a->nla_len - NLA_HDRLEN; }
static uint16_t atype(const struct nlattr *a) { return a->nla_type & NLA_TYPE_MASK; }
static void wiphy_attr(const struct nlattr *a) {
    if (atype(a) == NL80211_ATTR_WIPHY && plen(a) >= 4) {
        uint32_t id; memcpy(&id, payload(a), 4); printf("wiphy=%u\n", id);
    } else if (atype(a) == NL80211_ATTR_WIPHY_NAME) {
        printf("wiphy_name=%.*s\n", (int)plen(a), (const char *)payload(a));
    } else if (atype(a) == NL80211_ATTR_SUPPORTED_IFTYPES) {
        size_t left = plen(a);
        const struct nlattr *v = payload(a);
        while (attr_ok(v, left)) {
            printf("supported_iftype=%u%s\n", atype(v),
                   atype(v) == NL80211_IFTYPE_MONITOR ? " (monitor)" : "");
            size_t step = NLA_ALIGN(v->nla_len);
            if (step > left) break;
            left -= step; v = (const void *)((const unsigned char *)v + step);
        }
    } else if (atype(a) == NL80211_ATTR_VENDOR_DATA) {
        /* GET_WIPHY advertises struct nl80211_vendor_cmd_info entries here.
         * Listing IDs only; do not invoke any vendor command. */
        size_t left = plen(a);
        const struct nlattr *v = payload(a);
        while (attr_ok(v, left)) {
            if (plen(v) >= 8) {
                uint32_t ids[2]; memcpy(ids, payload(v), sizeof(ids));
                vendor_commands++;
                if (ids[0] == 0x1a11) {
                    google_commands++;
                    printf("google_vendor_command=0x%x:%u\n", ids[0], ids[1]);
                }
            }
            size_t step = NLA_ALIGN(v->nla_len);
            if (step > left) break;
            left -= step; v = (const void *)((const unsigned char *)v + step);
        }
    }
}
static int parse_attrs(const unsigned char *p, size_t left, int family_query) {
    int family = -1;
    const struct nlattr *a = (const void *)p;
    while (attr_ok(a, left)) {
        if (family_query && atype(a) == CTRL_ATTR_FAMILY_ID && plen(a) >= 2) {
            uint16_t value; memcpy(&value, payload(a), 2); family = value;
        } else if (!family_query) wiphy_attr(a);
        size_t step = NLA_ALIGN(a->nla_len);
        if (step > left) break;
        left -= step; a = (const void *)((const unsigned char *)a + step);
    }
    return family;
}
static int query(uint16_t family, uint8_t cmd, int resolve) {
    unsigned char request[256] = {0};
    struct nlmsghdr *h = (void *)request;
    struct genlmsghdr *g = (void *)(request + NLMSG_HDRLEN);
    h->nlmsg_len = NLMSG_HDRLEN + GENL_HDRLEN;
    h->nlmsg_type = family;
    h->nlmsg_flags = NLM_F_REQUEST | (resolve ? 0 : NLM_F_DUMP);
    h->nlmsg_seq = ++seq;
    g->cmd = cmd; g->version = 1;
    if (resolve) {
        struct nlattr *a = (void *)(request + h->nlmsg_len);
        const char name[] = "nl80211";
        a->nla_type = CTRL_ATTR_FAMILY_NAME;
        a->nla_len = NLA_HDRLEN + sizeof(name);
        memcpy((unsigned char *)a + NLA_HDRLEN, name, sizeof(name));
        h->nlmsg_len += NLA_ALIGN(a->nla_len);
    } else {
        struct nlattr *a = (void *)(request + h->nlmsg_len);
        a->nla_type = NL80211_ATTR_SPLIT_WIPHY_DUMP;
        a->nla_len = NLA_HDRLEN;
        h->nlmsg_len += NLA_ALIGN(a->nla_len);
    }
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    if (sendto(fd, request, h->nlmsg_len, 0, (void *)&kernel, sizeof(kernel)) < 0) {
        printf("query_send_error=%d (%s)\n", errno, strerror(errno)); return -1;
    }
    /* Bounded receive loop even if the kernel does not terminate its dump. */
    for (int n = 0; n < 256; n++) {
        unsigned char buf[65536]; struct sockaddr_nl peer = {0};
        struct iovec io = {.iov_base = buf, .iov_len = sizeof(buf)};
        struct msghdr msg = {.msg_name = &peer, .msg_namelen = sizeof(peer),
                             .msg_iov = &io, .msg_iovlen = 1};
        ssize_t count = recvmsg(fd, &msg, 0);
        if (count < 0) {
            printf("query_receive_error=%d (%s)\n", errno, strerror(errno)); return -1;
        }
        if ((msg.msg_flags & MSG_TRUNC) || peer.nl_pid != 0) {
            puts("query_invalid_response=1"); return -1;
        }
        unsigned int remain = (unsigned int)count;
        for (struct nlmsghdr *r = (void *)buf; NLMSG_OK(r, remain); r = NLMSG_NEXT(r, remain)) {
            if (r->nlmsg_seq != seq) continue;
            if (r->nlmsg_flags & NLM_F_DUMP_INTR) {
                puts("query_dump_interrupted=1"); return -1;
            }
            if (r->nlmsg_type == NLMSG_ERROR) {
                if (r->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) return -1;
                const struct nlmsgerr *e = NLMSG_DATA(r);
                printf("query_kernel_error=%d\n", e->error); return e->error ? -1 : 0;
            }
            if (r->nlmsg_type == NLMSG_DONE) {
                if (r->nlmsg_len >= NLMSG_LENGTH(sizeof(int))) {
                    int status; memcpy(&status, NLMSG_DATA(r), sizeof(status));
                    if (status) { printf("query_done_error=%d\n", status); return -1; }
                }
                return 0;
            }
            if (r->nlmsg_type != family || r->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN) continue;
            int value = parse_attrs((const unsigned char *)NLMSG_DATA(r) + GENL_HDRLEN,
                                    r->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN, resolve);
            if (resolve && value >= 0) return value;
        }
    }
    puts("query_message_limit=1"); return -1;
}
int main(void) {
    puts("FOLD5_RADIO_PROBE=1\nmode=read_only\nwireless_transmit=never");
    printf("uid=%u\n", (unsigned)getuid());
    FILE *status = fopen("/proc/self/status", "r");
    if (status) {
        char line[256];
        while (fgets(line, sizeof(line), status))
            if (!strncmp(line, "CapEff:", 7)) fputs(line, stdout);
        fclose(status);
    }
    /* Protocol 0: even on success, no packet reception is requested. */
    int packet = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
    if (packet < 0) printf("packet_socket_errno=%d (%s)\n", errno, strerror(errno));
    else { puts("packet_socket_available=1"); close(packet); }
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) { printf("netlink_socket_errno=%d (%s)\n", errno, strerror(errno)); return 2; }
    struct timeval timeout = {.tv_sec = 2};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        puts("timeout_setup_failed=1"); close(fd); return 2;
    }
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    if (bind(fd, (void *)&local, sizeof(local)) < 0) {
        printf("netlink_bind_errno=%d (%s)\n", errno, strerror(errno)); close(fd); return 2;
    }
    int family = query(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1);
    if (family < 0) { puts("nl80211_query=unavailable"); close(fd); return 2; }
    printf("nl80211_family=%d\n", family);
    int result = query((uint16_t)family, NL80211_CMD_GET_WIPHY, 0);
    printf("wiphy_query=%s\n", result == 0 ? "complete" : "unavailable");
    if (result == 0) {
        printf("advertised_vendor_commands=%u\n", vendor_commands);
        printf("advertised_google_vendor_commands=%u\n", google_commands);
    }
    close(fd); return result == 0 ? 0 : 2;
}
