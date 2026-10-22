/*
 * Pure-TCP-ACK detection for the unicast queue, kept free of ESP-IDF types so
 * tests/ack_coalesce_test.c exercises exactly what the firmware runs.
 *
 * The radio is half duplex: every ACK we put on the air is airtime during which
 * the iPhone's next data frame cannot be received. ACKs are cumulative, so when
 * a newer pure ACK of the same flow is queued behind an unsent older one, the
 * older one carries no information the newer one does not.
 *
 * Only frames that are provably "just an ACK" are ever merged: IPv6 without
 * extension headers, TCP, no payload, flags limited to ACK (+PSH), and no TCP
 * option that could carry SACK (or anything we cannot parse). Duplicate ACKs
 * with SACK blocks drive the sender's fast retransmit and must never be merged.
 */
#ifndef AWDL_ACKCOALESCE_H
#define AWDL_ACKCOALESCE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ACKC_ETH_LEN 14
#define ACKC_IP6_LEN 40
#define ACKC_TCP_MIN 20

struct ackc_flow {
	uint8_t src[16], dst[16];
	uint16_t sport, dport;
	uint32_t ack;
};

/* Returns 1 and fills *f if eth (len bytes, Ethernet II) is a pure TCP ACK. */
static inline int ackc_parse(const uint8_t *eth, size_t len, struct ackc_flow *f) {
	if (len < ACKC_ETH_LEN + ACKC_IP6_LEN + ACKC_TCP_MIN)
		return 0;
	if (eth[12] != 0x86 || eth[13] != 0xdd)
		return 0;
	const uint8_t *ip = eth + ACKC_ETH_LEN;
	if ((ip[0] >> 4) != 6)
		return 0;
	if (ip[6] != 6) /* next header must be TCP itself: no extension headers */
		return 0;
	size_t plen = (size_t) (ip[4] << 8 | ip[5]);
	const uint8_t *tcp = ip + ACKC_IP6_LEN;
	size_t hlen = (size_t) (tcp[12] >> 4) * 4;
	if (hlen < ACKC_TCP_MIN || plen != hlen) /* plen == header means zero payload */
		return 0;
	if (len < ACKC_ETH_LEN + ACKC_IP6_LEN + plen) /* truncated; trailing Ethernet padding is fine */
		return 0;
	uint8_t flags = tcp[13];
	/* ACK required; SYN/FIN/RST/URG, and ECE/CWR (congestion signalling), must stay untouched. PSH is harmless. */
	if ((flags & ~0x08) != 0x10)
		return 0;
	/* Options: only NOP/EOL/Timestamps are known-safe; everything else (SACK=5, unknown) keeps the frame as is. */
	size_t i = ACKC_TCP_MIN;
	while (i < hlen) {
		uint8_t kind = tcp[i];
		if (kind == 0)
			break;
		if (kind == 1) {
			i++;
			continue;
		}
		if (i + 2 > hlen)
			return 0;
		uint8_t olen = tcp[i + 1];
		if (olen < 2 || i + olen > hlen)
			return 0;
		if (kind != 8) /* timestamps */
			return 0;
		i += olen;
	}
	if (f) {
		memcpy(f->src, ip + 8, 16);
		memcpy(f->dst, ip + 24, 16);
		f->sport = (uint16_t) (tcp[0] << 8 | tcp[1]);
		f->dport = (uint16_t) (tcp[2] << 8 | tcp[3]);
		f->ack = (uint32_t) tcp[8] << 24 | (uint32_t) tcp[9] << 16 | (uint32_t) tcp[10] << 8 | tcp[11];
	}
	return 1;
}

/* 1 if the pure ACK `neu` may take the place of the still-unsent pure ACK `old`:
 * same flow, and neu.ack is ahead of old.ack (mod 2^32). */
static inline int ackc_replaces(const struct ackc_flow *old, const struct ackc_flow *neu) {
	if (old->sport != neu->sport || old->dport != neu->dport)
		return 0;
	if (memcmp(old->src, neu->src, 16) || memcmp(old->dst, neu->dst, 16))
		return 0;
	/* Strictly forward only. An equal ack may be a duplicate ACK, which without SACK is the
	 * sender's only loss signal (fast retransmit needs three); merging those would hide it. */
	return (int32_t) (neu->ack - old->ack) > 0;
}

#endif
