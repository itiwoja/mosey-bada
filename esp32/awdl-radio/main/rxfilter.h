/*
 * Receive-side filter for the ESP32 AWDL radio, kept free of ESP-IDF types so
 * tests/awdl_sim_test.c can exercise exactly what the firmware runs.
 */
#ifndef AWDL_RXFILTER_H
#define AWDL_RXFILTER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define RXFILTER_RADIOTAP_LEN 10

enum rxfilter_kind { RXF_DROP = 0, RXF_ACTION, RXF_DATA };

static const uint8_t rxfilter_awdl_bssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};

/* Classify a received 802.11 frame (p, len including the 4-byte FCS).
 * is_mgmt / is_data: the radio's own classification of the frame. */
static inline enum rxfilter_kind rxfilter_classify(const uint8_t *p, uint16_t len, int is_mgmt, int is_data,
                                                   const uint8_t self[6]) {
	if (len < 24 + 4)
		return RXF_DROP;
	uint16_t fc = (uint16_t) (p[0] | p[1] << 8);
	if (is_mgmt && (fc & 0x00fc) == 0x00d0) {
		/* action: category 127 (vendor), OUI 00:17:f2, type 8 (AWDL) */
		if (len < 24 + 5 + 4 || p[24] != 127 || p[25] != 0x00 || p[26] != 0x17 || p[27] != 0xf2 || p[28] != 0x08)
			return RXF_DROP;
		return RXF_ACTION;
	}
	if (is_data && (fc & 0x000c) == 0x0008) {
		if (memcmp(p + 16, rxfilter_awdl_bssid, 6) != 0)
			return RXF_DROP; /* not AWDL */
		if (!(p[4] & 0x01) && memcmp(p + 4, self, 6) != 0)
			return RXF_DROP; /* unicast to somebody else */
		return RXF_DATA;
	}
	return RXF_DROP;
}

/* --- 802.11 retransmission de-duplication ---
 *
 * A sender retransmits a frame (retry bit set, same seq_ctrl) whenever it did
 * not see our ACK. If the original did get through, the copy would reach the
 * host as a second TCP segment. So for each transmitter we remember the last
 * few seq_ctrl values we passed on, and drop a retry that repeats one.
 * Only retries are ever dropped: a retry of a frame we never saw (the original
 * was lost) is the first copy and must be delivered. Peers are few, so a tiny
 * fixed table with oldest-entry eviction is enough. */
#define RXDEDUP_PEERS 8
#define RXDEDUP_SEQS 4

struct rxdedup_entry {
	uint8_t ta[6];
	uint16_t seq[RXDEDUP_SEQS];
	uint8_t used;  /* entry holds a transmitter */
	uint8_t n;     /* valid values in seq[] */
	uint8_t next;  /* ring position for the next value */
	uint32_t tick; /* last use, for eviction */
};

struct rxdedup {
	struct rxdedup_entry e[RXDEDUP_PEERS];
	uint32_t tick;
};

/* p: a data frame accepted by rxfilter_classify() (len >= 28). Returns 1 if it
 * is a retry of a frame already seen from the same transmitter (drop it).
 * Always records the frame's seq_ctrl otherwise. */
static inline int rxdedup_check(struct rxdedup *d, const uint8_t *p) {
	uint16_t fc = (uint16_t) (p[0] | p[1] << 8);
	uint16_t sc = (uint16_t) (p[22] | p[23] << 8);
	const uint8_t *ta = p + 10;
	struct rxdedup_entry *ent = NULL, *victim = &d->e[0];
	d->tick++;
	for (int i = 0; i < RXDEDUP_PEERS; i++) {
		struct rxdedup_entry *c = &d->e[i];
		if (c->used && memcmp(c->ta, ta, 6) == 0) {
			ent = c;
			break;
		}
		if (!c->used || (victim->used && c->tick < victim->tick))
			victim = c;
	}
	if (!ent) {
		ent = victim;
		memset(ent, 0, sizeof(*ent));
		memcpy(ent->ta, ta, 6);
		ent->used = 1;
	}
	ent->tick = d->tick;
	if (fc & 0x0800) {
		for (int i = 0; i < ent->n; i++)
			if (ent->seq[i] == sc)
				return 1;
	}
	ent->seq[ent->next] = sc;
	ent->next = (uint8_t) ((ent->next + 1) % RXDEDUP_SEQS);
	if (ent->n < RXDEDUP_SEQS)
		ent->n++;
	return 0;
}

/* Write a minimal radiotap header (FLAGS with FCS-present, dBm signal) and
 * the frame into out (needs RXFILTER_RADIOTAP_LEN + len). Returns total length. */
static inline uint16_t rxfilter_wrap(const uint8_t *p, uint16_t len, int8_t rssi, uint8_t *out) {
	out[0] = 0; /* version */
	out[1] = 0; /* pad */
	out[2] = RXFILTER_RADIOTAP_LEN;
	out[3] = 0;
	out[4] = (1u << 1) | (1u << 5); /* present: FLAGS | DBM_ANTSIGNAL (little-endian u32) */
	out[5] = 0;
	out[6] = 0;
	out[7] = 0;
	out[8] = 0x10; /* IEEE80211_RADIOTAP_F_FCS */
	out[9] = (uint8_t) rssi;
	memcpy(out + RXFILTER_RADIOTAP_LEN, p, len);
	return (uint16_t) (RXFILTER_RADIOTAP_LEN + len);
}

#endif /* AWDL_RXFILTER_H */
