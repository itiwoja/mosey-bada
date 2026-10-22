/*
 * Wire format between the ESP32 AWDL radio and the Fold5 bridge daemon.
 * Shared by esp32/awdl-radio and src/fold5/awdl_bridge.c.
 *
 * Every message is SLIP-framed (RFC 1055): END=0xC0, ESC=0xDB, ESC_END=0xDC,
 * ESC_ESC=0xDD. Inside a frame: type (1 byte), payload, CRC-16/CCITT-FALSE of
 * type+payload (2 bytes, big-endian). Frames with a bad CRC are dropped, which
 * also discards the ROM bootloader's text output printed at reset.
 */
#ifndef HOSTLINK_PROTO_H
#define HOSTLINK_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define HOSTLINK_VERSION 1
#define HOSTLINK_BAUD 921600
#define HOSTLINK_MAX_PAYLOAD 1600

#define SLIP_END 0xC0
#define SLIP_ESC 0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

enum hostlink_type {
	/* both directions */
	HL_ETH = 0x01,          /* Ethernet frame (dst, src, ethertype, payload) */
	/* radio -> host */
	HL_LOG = 0x02,          /* level (1) + text */
	HL_HELLO = 0x03,        /* version (1) + AWDL MAC (6) + channel (1) */
	HL_PEER_ADD = 0x04,     /* MAC (6) + name (rest, not terminated) */
	HL_PEER_DEL = 0x05,     /* MAC (6) */
	HL_STATS = 0x06,        /* struct hostlink_stats, little-endian */
	HL_BAUD_OK = 0x07,      /* baud (4, LE): HL_SET_BAUD accepted, switching after this frame */
	/* host -> radio */
	HL_SET_NAME = 0x10,     /* AWDL host name (rest, not terminated) */
	HL_SET_LOGLEVEL = 0x11, /* level (1): 3 error .. 8 trace */
	HL_PING = 0x12,         /* answered with HL_HELLO */
	HL_SET_CHANSEQ = 0x13,  /* strategy (1): enum hostlink_chanseq. Old firmware ignores it */
	HL_SET_BAUD = 0x14,     /* baud (4, LE). Old firmware ignores it, so no HL_BAUD_OK comes back */
};

/* Raising the UART speed (HL_SET_BAUD), so that a dropped or unsupported rate
 * can never leave the two ends unable to talk:
 *   1. host sends HL_SET_BAUD at the current rate.
 *   2. radio answers HL_BAUD_OK (same rate), drains its TX, switches, and arms
 *      HOSTLINK_BAUD_COMMIT_MS. Unsupported values get no answer.
 *   3. host switches its USB-UART to the new rate and sends HL_PING.
 *   4. the first valid frame the radio decodes at the new rate commits it (and
 *      the PING is answered with HL_HELLO as usual). If none arrives within
 *      HOSTLINK_BAUD_COMMIT_MS, the radio falls back to HOSTLINK_BAUD.
 *   5. host without a HELLO at the new rate within HOSTLINK_BAUD_COMMIT_MS
 *      returns to HOSTLINK_BAUD and PINGs until the radio's HELLO comes back.
 * A radio reboot always starts at HOSTLINK_BAUD (the host sees it as a HELLO
 * it did not ask for, or as silence at the high rate, and renegotiates). */
#define HOSTLINK_BAUD_COMMIT_MS 1500

/* How the radio derives the channel sequence it advertises from the sync
 * master's. VERBATIM copies it (FINDINGS §25). INTERSECT keeps only the slots
 * on the radio's own channel and blanks the rest, so the peer stops sending to
 * us in slots we cannot hear; with no overlap it falls back to VERBATIM. */
enum hostlink_chanseq {
	HL_CHANSEQ_VERBATIM = 0,
	HL_CHANSEQ_INTERSECT = 1,
	/* INTERSECT, plus the radio's channel in every slot the master leaves empty
	 * (it is off-channel or idle there), inviting it to schedule more of them
	 * on ours. Experimental: §25 may reject it. */
	HL_CHANSEQ_FILL = 2,
};

struct hostlink_stats {
	uint32_t rx_seen;        /* frames delivered by promiscuous mode */
	uint32_t rx_awdl_action; /* AWDL action frames queued */
	uint32_t rx_awdl_data;   /* AWDL data frames queued */
	uint32_t rx_dropped;     /* dropped because the queue was full */
	uint32_t tx_action;
	uint32_t tx_data;
	uint32_t tx_failed;      /* esp_wifi_80211_tx() errors */
	uint32_t peers;
	/* Appended in the same HOSTLINK_VERSION: readers must accept a payload that
	 * ends after `peers` (older firmware) and treat missing fields as absent. */
	uint32_t rx_data_retry;  /* AWDL data frames with the 802.11 retry bit set */
	uint32_t rx_data_dup;    /* of those, dropped as duplicates (same TA + seq_ctrl) */
	uint32_t rx_data_ampdu;  /* AWDL data frames that arrived inside an A-MPDU */
	uint32_t tx_uq_dropped;  /* unicast frames dropped because the queue was full */
	uint32_t chanseq;        /* enum hostlink_chanseq in effect */
} __attribute__((packed));

#define HOSTLINK_STATS_V1_SIZE (8 * sizeof(uint32_t))

static inline uint16_t hostlink_crc16(const uint8_t *p, size_t n) {
	uint16_t crc = 0xffff;
	while (n--) {
		crc ^= (uint16_t) (*p++) << 8;
		for (int i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
	}
	return crc;
}

/* Incremental SLIP decoder. Feed bytes; returns the frame length (type +
 * payload, CRC removed) when a valid frame is complete, 0 otherwise. */
struct slip_decoder {
	uint8_t buf[HOSTLINK_MAX_PAYLOAD + 3];
	size_t len;
	int esc;
	int overflow;
};

static inline size_t slip_decode_byte(struct slip_decoder *d, uint8_t c) {
	if (c == SLIP_END) {
		size_t n = d->len;
		int bad = d->overflow;
		d->len = 0;
		d->esc = 0;
		d->overflow = 0;
		if (bad || n < 3)
			return 0;
		uint16_t want = (uint16_t) (d->buf[n - 2] << 8 | d->buf[n - 1]);
		if (hostlink_crc16(d->buf, n - 2) != want)
			return 0;
		return n - 2;
	}
	if (d->esc) {
		d->esc = 0;
		if (c == SLIP_ESC_END)
			c = SLIP_END;
		else if (c == SLIP_ESC_ESC)
			c = SLIP_ESC;
		else {
			d->overflow = 1; /* protocol error: drop this frame */
			return 0;
		}
	} else if (c == SLIP_ESC) {
		d->esc = 1;
		return 0;
	}
	if (d->len >= sizeof(d->buf)) {
		d->overflow = 1;
		return 0;
	}
	d->buf[d->len++] = c;
	return 0;
}

/* Encode type+payload into out (needs 2 * (n + 3) + 2 bytes). Returns length. */
static inline size_t slip_encode(uint8_t type, const uint8_t *payload, size_t n, uint8_t *out) {
	size_t o = 0;
	uint8_t crc_in[1] = {type};
	uint16_t crc = 0xffff;
	/* CRC over type + payload without copying the payload */
	{
		const uint8_t *parts[2] = {crc_in, payload};
		size_t lens[2] = {1, n};
		for (int k = 0; k < 2; k++) {
			const uint8_t *p = parts[k];
			for (size_t i = 0; i < lens[k]; i++) {
				crc ^= (uint16_t) p[i] << 8;
				for (int b = 0; b < 8; b++)
					crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
			}
		}
	}
	uint8_t tail[2] = {(uint8_t) (crc >> 8), (uint8_t) crc};
	out[o++] = SLIP_END;
	const uint8_t *parts[3] = {crc_in, payload, tail};
	size_t lens[3] = {1, n, 2};
	for (int k = 0; k < 3; k++) {
		for (size_t i = 0; i < lens[k]; i++) {
			uint8_t c = parts[k][i];
			if (c == SLIP_END) {
				out[o++] = SLIP_ESC;
				out[o++] = SLIP_ESC_END;
			} else if (c == SLIP_ESC) {
				out[o++] = SLIP_ESC;
				out[o++] = SLIP_ESC_ESC;
			} else {
				out[o++] = c;
			}
		}
	}
	out[o++] = SLIP_END;
	return o;
}

#endif /* HOSTLINK_PROTO_H */
