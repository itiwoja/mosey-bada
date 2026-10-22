/*
 * Host test for ackc_parse()/ackc_replaces() in esp32/awdl-radio/main/ackcoalesce.h.
 * Build+run (from the repository root):
 *   cc -Wall -fsanitize=address,undefined -I esp32/awdl-radio/main \
 *      tests/ack_coalesce_test.c -o /tmp/ack_coalesce_test && /tmp/ack_coalesce_test
 */
#include <assert.h>
#include <stdio.h>

#include "ackcoalesce.h"

/* Build eth+IPv6+TCP. opts/optlen: raw TCP options (multiple of 4). */
static size_t mk(uint8_t *f, uint8_t flags, uint32_t ack, uint16_t sport, uint8_t host, const uint8_t *opts,
                 size_t optlen, size_t payload) {
	size_t hlen = 20 + optlen;
	memset(f, 0, 200);
	f[12] = 0x86;
	f[13] = 0xdd;
	uint8_t *ip = f + 14;
	ip[0] = 0x60;
	ip[4] = (uint8_t) ((hlen + payload) >> 8);
	ip[5] = (uint8_t) (hlen + payload);
	ip[6] = 6;
	ip[7] = 64;
	ip[8] = 0xfe;
	ip[9] = 0x80;
	ip[23] = host;
	ip[24] = 0xfe;
	ip[25] = 0x80;
	ip[39] = 2;
	uint8_t *t = ip + 40;
	t[0] = (uint8_t) (sport >> 8);
	t[1] = (uint8_t) sport;
	t[2] = 0x22;
	t[3] = 0x53;
	t[8] = (uint8_t) (ack >> 24);
	t[9] = (uint8_t) (ack >> 16);
	t[10] = (uint8_t) (ack >> 8);
	t[11] = (uint8_t) ack;
	t[12] = (uint8_t) ((hlen / 4) << 4);
	t[13] = flags;
	if (optlen)
		memcpy(t + 20, opts, optlen);
	return 14 + 40 + hlen + payload;
}

static int pure(uint8_t flags, const uint8_t *opts, size_t optlen, size_t payload) {
	uint8_t f[200];
	size_t n = mk(f, flags, 1000, 5000, 1, opts, optlen, payload);
	return ackc_parse(f, n, NULL);
}

int main(void) {
	uint8_t f[200], g[200];
	static const uint8_t ts[12] = {1, 1, 8, 10, 0, 0, 0, 1, 0, 0, 0, 2};
	static const uint8_t sack[12] = {1, 1, 5, 10, 0, 0, 0, 10, 0, 0, 0, 20};
	static const uint8_t junk[4] = {99, 4, 0, 0};
	static const uint8_t bad[4] = {1, 1, 8, 200}; /* length overruns header */

	/* classification */
	assert(pure(0x10, NULL, 0, 0));
	assert(pure(0x18, NULL, 0, 0));     /* ACK|PSH */
	assert(pure(0x10, ts, 12, 0));      /* timestamps ok */
	assert(!pure(0x12, NULL, 0, 0));    /* SYN|ACK */
	assert(!pure(0x11, NULL, 0, 0));    /* FIN|ACK */
	assert(!pure(0x14, NULL, 0, 0));    /* RST|ACK */
	assert(!pure(0x30, NULL, 0, 0));    /* URG|ACK */
	assert(!pure(0x50, NULL, 0, 0));    /* ECE|ACK */
	assert(!pure(0x90, NULL, 0, 0));    /* CWR|ACK */
	assert(!pure(0x02, NULL, 0, 0));    /* SYN only */
	assert(!pure(0x00, NULL, 0, 0));    /* no ACK */
	assert(!pure(0x10, NULL, 0, 100));  /* data */
	assert(!pure(0x18, NULL, 0, 1));    /* 1 byte data */
	assert(!pure(0x10, sack, 12, 0));   /* SACK */
	assert(!pure(0x10, junk, 4, 0));    /* unknown option */
	assert(!pure(0x10, bad, 4, 0));     /* malformed option */

	size_t n = mk(f, 0x10, 1, 1, 1, NULL, 0, 0);
	assert(ackc_parse(f, n, NULL));
	assert(!ackc_parse(f, n - 1, NULL)); /* truncated */
	assert(!ackc_parse(f, 10, NULL));
	assert(!ackc_parse(f, 0, NULL));
	assert(ackc_parse(f, n + 6, NULL)); /* Ethernet padding is fine */
	f[12] = 0x08; f[13] = 0x00;         /* IPv4 ethertype */
	assert(!ackc_parse(f, n, NULL));
	f[12] = 0x86; f[13] = 0xdd;
	f[14] = 0x40;                       /* version 4 */
	assert(!ackc_parse(f, n, NULL));
	f[14] = 0x60;
	f[14 + 6] = 0;                      /* hop-by-hop extension header */
	assert(!ackc_parse(f, n, NULL));
	f[14 + 6] = 17;                     /* UDP */
	assert(!ackc_parse(f, n, NULL));

	/* replacement */
	struct ackc_flow a, b;
	n = mk(f, 0x10, 1000, 5000, 1, NULL, 0, 0);
	assert(ackc_parse(f, n, &a));
	assert(a.ack == 1000 && a.sport == 5000 && a.dport == 0x2253);
	n = mk(g, 0x10, 2448, 5000, 1, NULL, 0, 0);
	assert(ackc_parse(g, n, &b));
	assert(ackc_replaces(&a, &b));
	assert(!ackc_replaces(&b, &a));   /* ack goes backwards */
	n = mk(g, 0x10, 1000, 5000, 1, NULL, 0, 0);
	ackc_parse(g, n, &b);
	assert(!ackc_replaces(&a, &b));   /* equal ack: may be a duplicate ACK, keep it */
	n = mk(g, 0x10, 3000, 5001, 1, NULL, 0, 0);
	ackc_parse(g, n, &b);
	assert(!ackc_replaces(&a, &b));   /* other source port */
	n = mk(g, 0x10, 3000, 5000, 9, NULL, 0, 0);
	ackc_parse(g, n, &b);
	assert(!ackc_replaces(&a, &b));   /* other address */
	n = mk(g, 0x10, 3000, 5000, 1, NULL, 0, 0);
	ackc_parse(g, n, &b);
	b.dport ^= 1;
	assert(!ackc_replaces(&a, &b));   /* other destination port */

	/* 32-bit wraparound */
	n = mk(f, 0x10, 0xfffffff0u, 5000, 1, NULL, 0, 0);
	ackc_parse(f, n, &a);
	n = mk(g, 0x10, 0x00000100u, 5000, 1, NULL, 0, 0);
	ackc_parse(g, n, &b);
	assert(ackc_replaces(&a, &b));    /* forward across the wrap */
	assert(!ackc_replaces(&b, &a));   /* backward across the wrap */

	puts("ack_coalesce_test: ok");
	return 0;
}
