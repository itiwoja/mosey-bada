/*
 * Two OWL instances in one process: "phone" (master, Apple-like channel
 * sequence with one slot on channel 6) and "radio" (the ESP32 firmware's
 * configuration: AWDL_FIXED_RADIO_CHANNEL=6). Frames built by the phone go
 * through exactly the firmware's receive path (rxfilter.h + awdl_rx), then we
 * check what the firmware would do.
 *
 * Build: see tests/run-awdl-sim.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chanseq_follow.h"
#include "election.h"
#include "peers.h"
#include "rx.h"
#include "rxfilter.h"
#include "schedule.h"
#include "state.h"
#include "sync.h"
#include "tx.h"

static struct awdl_state phone, radio;
static struct ieee80211_state phone_ieee;
static const struct ether_addr phone_mac = {{0x5a, 0x11, 0x22, 0x33, 0x44, 0x55}};
static const struct ether_addr radio_mac = {{0x26, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}};

/* What the ESP32 sees on air: OWL's TX buffer minus its radiotap, plus FCS. */
static int air_from_owl_tx(const uint8_t *tx, int len, uint8_t *air) {
	uint16_t rt = (uint16_t) (tx[2] | tx[3] << 8);
	int n = len - rt;
	memcpy(air, tx + rt, (size_t) n);
	memset(air + n, 0xa5, 4); /* FCS value is not checked by OWL */
	return n + 4;
}

/* Feed one on-air frame to the radio like wifi_rx_cb()+handle_wifi_rx() do.
 * Returns the number of Ethernet frames produced; first one copied to eth. */
static int radio_receive(const uint8_t *air, int len, int is_mgmt, int is_data, uint64_t now, uint8_t *eth,
                         int *eth_len) {
	enum rxfilter_kind kind = rxfilter_classify(air, (uint16_t) len, is_mgmt, is_data, radio_mac.ether_addr_octet);
	if (kind == RXF_DROP)
		return -1;
	uint8_t wrapped[4000];
	uint16_t n = rxfilter_wrap(air, (uint16_t) len, -40, wrapped);
	struct buf *out_arr[AWDL_RX_MAX_FRAMES];
	struct buf **out = &out_arr[0];
	const struct buf *frame = buf_new_const(wrapped, n);
	awdl_rx_timestamp = now;
	int r = awdl_rx(frame, &out, &radio);
	awdl_rx_timestamp = 0;
	buf_free(frame);
	int produced = (int) (out - &out_arr[0]);
	for (int i = 0; i < produced; i++) {
		if (i == 0 && eth) {
			*eth_len = buf_len(out_arr[i]);
			memcpy(eth, buf_data(out_arr[i]), (size_t) *eth_len);
		}
		buf_free(out_arr[i]);
	}
	return r == RX_OK ? produced : (r < 0 ? -2 : 0);
}

static void phone_sends_action(enum awdl_action_type t, uint64_t now) {
	uint8_t tx[2000], air[2000];
	int len = awdl_init_full_action_frame(tx, &phone, &phone_ieee, t);
	assert(len > 0);
	int n = air_from_owl_tx(tx, len, air);
	int r = radio_receive(air, n, 1, 0, now, NULL, NULL);
	assert(r >= 0);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	log_set_level(LOG_WARNING);
	uint64_t t0 = clock_time_us();

	awdl_init_state(&phone, "iPhone", &phone_mac, CHAN_OPCLASS_149, t0);
	ieee80211_init_state(&phone_ieee);
	phone_ieee.fcs = false;
	/* Apple-like sequence (FINDINGS §25): 149 x3, empty, ch6 in slot 8 */
	const int apple[16] = {149, 149, 149, 0, 0, 0, 0, 0, 6, 149, 149, 0, 0, 0, 0, 0};
	for (int i = 0; i < 16; i++)
		phone.channel.sequence[i] = apple[i] == 149 ? CHAN_OPCLASS_149 : apple[i] == 6 ? CHAN_OPCLASS_6 : CHAN_NULL;
	phone.election.self_metric = phone.election.master_metric = 500; /* phone wins the election */

	awdl_init_state(&radio, "Fold5", &radio_mac, CHAN_OPCLASS_6, t0);

	/* --- filter: things the radio must ignore --- */
	{
		uint8_t f[64] = {0xd0, 0x00};
		memset(f + 4, 0xff, 6);
		f[24] = 4; /* public action, not AWDL */
		assert(rxfilter_classify(f, sizeof(f), 1, 0, radio_mac.ether_addr_octet) == RXF_DROP);
		uint8_t d[64] = {0x08, 0x00};
		memcpy(d + 4, "\x02\x01\x02\x03\x04\x05", 6); /* unicast to another station */
		memcpy(d + 16, rxfilter_awdl_bssid, 6);
		assert(rxfilter_classify(d, sizeof(d), 0, 1, radio_mac.ether_addr_octet) == RXF_DROP);
		memcpy(d + 16, "\x00\x11\x22\x33\x44\x55", 6); /* infra data */
		memcpy(d + 4, radio_mac.ether_addr_octet, 6);
		assert(rxfilter_classify(d, sizeof(d), 0, 1, radio_mac.ether_addr_octet) == RXF_DROP);
		assert(rxfilter_classify(d, 20, 0, 1, radio_mac.ether_addr_octet) == RXF_DROP); /* runt */
	}

	/* --- discovery and sync --- */
	for (int i = 0; i < 5; i++) {
		uint64_t now = clock_time_us();
		phone_sends_action(AWDL_ACTION_PSF, now);
		phone_sends_action(AWDL_ACTION_MIF, now);
	}
	struct awdl_peer *p = NULL;
	assert(awdl_peer_get(radio.peers.peers, &phone_mac, &p) >= 0);
	assert(p->has_sequence);
	awdl_election_run(&radio.election, &radio.peers);
	assert(!compare_ether_addr(&radio.election.sync_addr, &phone_mac));
	assert(chanseq_follow_master(&radio, 6, AWDL_CHANSEQ_VERBATIM) == 1);
	assert(!memcmp(radio.channel.sequence, phone.channel.sequence, sizeof(phone.channel.sequence)));
	printf("peer found, phone elected master, sequence adopted verbatim\n");

	/* --- TX gate: open only while the phone is on channel 6 --- */
	{
		int open_ok = 0, open_wrong = 0, shut_on6 = 0, mc_wrong = 0;
		uint64_t start = clock_time_us();
		for (int tu = 0; tu < 16 * 64 * 2; tu++) {
			uint64_t t = start + ieee80211_tu_to_usec((uint64_t) tu);
			int pslot = awdl_sync_current_eaw(t, &phone.sync) % 16;
			int pch = awdl_chan_num(phone.channel.sequence[pslot], phone.channel.enc);
			uint64_t to_boundary = awdl_sync_next_aw_us(t, &phone.sync);
			int near_edge = to_boundary < ieee80211_tu_to_usec(4) ||
			                ieee80211_tu_to_usec(64) - to_boundary < ieee80211_tu_to_usec(4);
			int gate = awdl_same_channel_as_peer(&radio, t, p);
			int mc = awdl_is_multicast_eaw(&radio, t);
			if (near_edge)
				continue;
			if (gate && pch == 6)
				open_ok++;
			else if (gate)
				open_wrong++;
			else if (pch == 6)
				shut_on6++;
			if (mc != (pch == 6))
				mc_wrong++;
		}
		printf("gate: open on ch6 %d TU, open elsewhere %d TU, shut on ch6 %d TU, multicast mismatches %d\n",
		       open_ok, open_wrong, shut_on6, mc_wrong);
		assert(open_ok > 0 && open_wrong == 0 && shut_on6 == 0 && mc_wrong == 0);
	}

	/* --- data: phone -> radio unicast becomes an Ethernet frame for awdl0 --- */
	{
		uint8_t ip6[200];
		for (size_t i = 0; i < sizeof(ip6); i++)
			ip6[i] = (uint8_t) (i * 7);
		ip6[0] = 0x60;
		uint8_t tx[2000], air[2000], eth[2000];
		int eth_len = 0;
		int len = awdl_init_full_data_frame(tx, &phone_mac, &radio_mac, ip6, sizeof(ip6), &phone, &phone_ieee);
		int n = air_from_owl_tx(tx, len, air);
		int r = radio_receive(air, n, 0, 1, clock_time_us(), eth, &eth_len);
		printf("data rx: r=%d eth_len=%d (want %d)\n", r, eth_len, 14 + (int) sizeof(ip6));
		fflush(stdout);
		assert(r == 1);
		assert(eth_len == 14 + (int) sizeof(ip6));
		assert(!memcmp(eth, radio_mac.ether_addr_octet, 6));
		assert(!memcmp(eth + 6, phone_mac.ether_addr_octet, 6));
		assert(eth[12] == 0x86 && eth[13] == 0xdd);
		assert(!memcmp(eth + 14, ip6, sizeof(ip6)));
		/* the same frame addressed to someone else is filtered before OWL */
		const struct ether_addr other = {{0x02, 9, 9, 9, 9, 9}};
		len = awdl_init_full_data_frame(tx, &phone_mac, &other, ip6, sizeof(ip6), &phone, &phone_ieee);
		n = air_from_owl_tx(tx, len, air);
		assert(radio_receive(air, n, 0, 1, clock_time_us(), eth, &eth_len) == -1);
		/* largest frame the firmware may transmit: MTU 1400 must fit esp_wifi_80211_tx's 1500 */
		uint8_t big[1400] = {0x60};
		len = awdl_init_full_data_frame(tx, &radio_mac, &phone_mac, big, sizeof(big), &radio, &phone_ieee);
		uint16_t rt = (uint16_t) (tx[2] | tx[3] << 8);
		printf("data: unicast delivered intact; 1400-byte IPv6 packet -> %d-byte 802.11 frame\n", len - rt);
		assert(len - rt <= 1500);
	}

	/* --- INTERSECT: declare only the slots really on ch6 --- */
	{
		assert(chanseq_follow_master(&radio, 6, AWDL_CHANSEQ_INTERSECT) == 1);
		assert(chanseq_follow_master(&radio, 6, AWDL_CHANSEQ_INTERSECT) == 0); /* stable */
		int on6 = 0, empty = 0;
		for (int i = 0; i < 16; i++) {
			int c = awdl_chan_num(radio.channel.sequence[i], radio.channel.enc);
			if (c == 6) {
				on6++;
				assert(!memcmp(&radio.channel.sequence[i], &phone.channel.sequence[i], sizeof(struct awdl_chan)));
			} else {
				assert(c == 0);
				empty++;
			}
		}
		assert(on6 == 1 && empty == 15 && awdl_chan_num(radio.channel.sequence[8], radio.channel.enc) == 6);
		/* gate still opens in the ch6 slot (and only there), and data arrives */
		int open_ok = 0, open_wrong = 0;
		uint64_t start = clock_time_us();
		for (int tu = 0; tu < 16 * 64 * 2; tu++) {
			uint64_t t = start + ieee80211_tu_to_usec((uint64_t) tu);
			int pslot = awdl_sync_current_eaw(t, &phone.sync) % 16;
			int pch = awdl_chan_num(phone.channel.sequence[pslot], phone.channel.enc);
			if (awdl_same_channel_as_peer(&radio, t, p))
				pch == 6 ? open_ok++ : open_wrong++;
		}
		assert(open_ok > 0 && open_wrong == 0);
		uint8_t ip6[100] = {0x60}, tx[2000], air[2000], eth[2000];
		int eth_len = 0;
		int len = awdl_init_full_data_frame(tx, &phone_mac, &radio_mac, ip6, sizeof(ip6), &phone, &phone_ieee);
		int n = air_from_owl_tx(tx, len, air);
		assert(radio_receive(air, n, 0, 1, clock_time_us(), eth, &eth_len) == 1 && eth_len == 14 + (int) sizeof(ip6));
		/* no overlap: declare the master's sequence verbatim, not an all-empty one */
		struct awdl_chan saved[16];
		memcpy(saved, p->sequence, sizeof(saved));
		for (int i = 0; i < 16; i++)
			if (awdl_chan_num(p->sequence[i], p->sequence_enc) == 6)
				p->sequence[i] = (struct awdl_chan) CHAN_NULL;
		assert(chanseq_follow_master(&radio, 6, AWDL_CHANSEQ_INTERSECT) == 1);
		assert(!memcmp(radio.channel.sequence, p->sequence, sizeof(saved)));
		memcpy(p->sequence, saved, sizeof(saved));
		/* back to VERBATIM rebuilds the declaration */
		assert(chanseq_follow_master(&radio, 6, AWDL_CHANSEQ_VERBATIM) == 1);
		assert(!memcmp(radio.channel.sequence, phone.channel.sequence, sizeof(saved)));
		printf("intersect: 1/16 slots declared (ch6 only), gate open on ch6, zero-overlap falls back to verbatim\n");
	}

	/* --- FILL: INTERSECT plus ch6 bytes in the master's empty slots --- */
	{
		struct awdl_chan saved[16];
		memcpy(saved, p->sequence, sizeof(saved));
		/* phone: 6 at slot 8 (from the earlier setup); make slots 3 and 5 empty, slot 9 ch44 */
		struct awdl_chan six = p->sequence[8];
		assert(awdl_chan_num(six, p->sequence_enc) == 6);
		p->sequence[3] = (struct awdl_chan) CHAN_NULL;
		p->sequence[5] = (struct awdl_chan) CHAN_NULL;
		assert(awdl_chan_num(p->sequence[9], p->sequence_enc) != 6);
		assert(chanseq_follow_master(&radio, 6, CHANSEQ_FILL) == 1);
		assert(chanseq_follow_master(&radio, 6, CHANSEQ_FILL) == 0); /* stable */
		for (int i = 0; i < 16; i++) {
			int pc = awdl_chan_num(p->sequence[i], p->sequence_enc);
			int rc = awdl_chan_num(radio.channel.sequence[i], radio.channel.enc);
			if (pc == 6 || pc == 0) {
				assert(rc == 6); /* peer's ch6 slots stay; its empty slots get ch6 */
				assert(!memcmp(&radio.channel.sequence[i], &six, sizeof(six))); /* the peer's own bytes */
			} else {
				assert(rc == 0); /* ch44 etc. stay empty */
			}
		}
		assert(awdl_chan_num(radio.channel.sequence[3], radio.channel.enc) == 6);
		assert(awdl_chan_num(radio.channel.sequence[5], radio.channel.enc) == 6);
		assert(awdl_chan_num(radio.channel.sequence[9], radio.channel.enc) == 0);
		/* gate opens only where the PEER is on ch6 too: never in the filled slots */
		int open_ok = 0, open_wrong = 0, filled_open = 0;
		uint64_t start = clock_time_us();
		for (int tu = 0; tu < 16 * 64 * 2; tu++) {
			uint64_t t = start + ieee80211_tu_to_usec((uint64_t) tu);
			int pslot = awdl_sync_current_eaw(t, &phone.sync) % 16;
			int pch = awdl_chan_num(p->sequence[pslot], p->sequence_enc);
			if (awdl_same_channel_as_peer(&radio, t, p)) {
				pch == 6 ? open_ok++ : open_wrong++;
				if (pslot == 3 || pslot == 5)
					filled_open++;
			}
		}
		assert(open_ok > 0 && open_wrong == 0 && filled_open == 0);
		/* no ch6 slot in the master's sequence: same as verbatim */
		p->sequence[8] = (struct awdl_chan) CHAN_NULL;
		assert(chanseq_follow_master(&radio, 6, CHANSEQ_FILL) == 1);
		assert(!memcmp(radio.channel.sequence, p->sequence, sizeof(saved)));
		memcpy(p->sequence, saved, sizeof(saved));
		assert(chanseq_follow_master(&radio, 6, AWDL_CHANSEQ_VERBATIM) == 1);
		printf("fill: empty slots take the peer's ch6 bytes, ch44 slots stay empty, gate shut in filled slots, no ch6 -> verbatim\n");
	}

	/* --- master goes away: back to the static sequence --- */
	awdl_peers_remove(radio.peers.peers, clock_time_us() + 1, NULL, NULL);
	awdl_election_run(&radio.election, &radio.peers);
	assert(chanseq_follow_master(&radio, 6, AWDL_CHANSEQ_VERBATIM) == 1);
	assert(awdl_chan_num(radio.channel.sequence[0], radio.channel.enc) == 6);
	printf("peer timeout: reverted to static channel 6\n");
	printf("awdl_sim_test: OK\n");
	return 0;
}
