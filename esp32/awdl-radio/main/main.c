/*
 * ESP32 AWDL radio for the Fold5 AirDrop receiver.
 *
 * Runs the OWL AWDL core (sync, election, channel sequence, action and data
 * frames) on an ESP32 that stays on 2.4 GHz channel 6, and exchanges the IPv6
 * Ethernet frames carried over AWDL with the phone over the USB-UART bridge
 * (hostlink_proto.h). The phone side puts them on a TAP interface (awdl0).
 *
 * Event model: everything that touches OWL state runs in awdl_task. The Wi-Fi
 * promiscuous callback, the UART reader and the esp_timer callbacks only post
 * events to its queue.
 *
 * GPL-3.0-or-later, as it links the OWL core.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "channel.h"
#include "circular_buffer.h"
#include "election.h"
#include "frame.h"
#include "ieee80211.h"
#include "log.h"
#include "peers.h"
#include "rx.h"
#include "schedule.h"
#include "state.h"
#include "sync.h"
#include "tx.h"
#include "wire.h"

#include "hostlink_proto.h"
#include "rxfilter.h"
#include "chanseq_follow.h"
#include "ackcoalesce.h"
#include "baudswitch.h"

#define RADIO_CHANNEL AWDL_FIXED_RADIO_CHANNEL
#define HOSTLINK_UART UART_NUM_0
/* Each queued event owns a malloc'd frame (up to ~1.6 kB), so this bounds the worst-case heap
 * the queue can pin: 160 x 1.6 kB = 256 kB in theory, far more than the heap, which is why
 * uart_reader_task also refuses to queue when the free heap is below HEAP_FLOOR. In practice
 * host->radio frames are mostly ~100-byte TCP ACKs; 96 overflowed (dropped 83, run4) in a
 * burst at 921600, and 160 x 24-byte events costs only 1.5 kB more than before. */
#define EVQ_LEN 160
#define HEAP_FLOOR 32768 /* never let queued host frames starve the Wi-Fi driver of this much */
#define UNICAST_QUEUE_LEN 32 /* 32 pointers + up to 32 x ~1.5 kB of frames while the peer is off-channel */
#define MULTICAST_QUEUE_LEN 8
#define WIFI_TX_MAX 1500 /* esp_wifi_80211_tx() limit */

enum ev_type {
	EV_WIFI_RX,
	EV_HOST_FRAME,
	EV_PSF,
	EV_MIF,
	EV_PEERS,
	EV_TX_UNICAST,
	EV_TX_MULTICAST,
	EV_STATS,
};

struct ev {
	uint8_t type;
	uint16_t len;
	uint64_t time_us;
	uint8_t *data; /* malloc'd, owned by the receiver of the event */
};

static QueueHandle_t evq;
static SemaphoreHandle_t uart_lock;
static struct awdl_state awdl;
static struct ieee80211_state ieee;
static struct ether_addr self_addr;
static int log_level = LOG_INFO;
static struct hostlink_stats stats;
/* Requested by the host (HL_SET_CHANSEQ); read by the awdl task only. */
static enum hostlink_chanseq chanseq_mode = HL_CHANSEQ_VERBATIM;
/* Only touched from wifi_rx_cb (Wi-Fi task). */
static struct rxdedup rx_dedup;

static esp_timer_handle_t t_psf, t_mif, t_peers, t_tx, t_mc, t_stats;
/* HL_SET_BAUD. baud_sw is guarded by baud_mux (awdl task, reader task and the timer task all touch it). */
static esp_timer_handle_t t_baud, t_baud_idle;
static inline uint32_t now_ms(void) { return (uint32_t) (esp_timer_get_time() / 1000); }
static struct baudsw baud_sw;
static portMUX_TYPE baud_mux = portMUX_INITIALIZER_UNLOCKED;

/* Unicast frames from the host wait here until the peer is on our channel. */
static struct buf *uq[UNICAST_QUEUE_LEN];
static int uq_head, uq_len;
/* Pure TCP ACKs merged into a queued one; logged from handle_stats (hostlink_stats has no field for it). */
static uint32_t uq_ack_merged, uq_ack_merged_logged;
static struct buf *mq[MULTICAST_QUEUE_LEN];
static int mq_head, mq_len;

/* ------------------------------------------------------------------------ */
/* Host link                                                                */

static void hostlink_send(uint8_t type, const uint8_t *payload, size_t n) {
	static uint8_t out[2 * (HOSTLINK_MAX_PAYLOAD + 3) + 2];
	if (n > HOSTLINK_MAX_PAYLOAD)
		return;
	xSemaphoreTake(uart_lock, portMAX_DELAY);
	size_t len = slip_encode(type, payload, n, out);
	uart_write_bytes(HOSTLINK_UART, out, len);
	xSemaphoreGive(uart_lock);
}

static void hostlink_send_hello(void) {
	uint8_t p[8];
	p[0] = HOSTLINK_VERSION;
	memcpy(p + 1, self_addr.ether_addr_octet, 6);
	p[7] = RADIO_CHANNEL;
	hostlink_send(HL_HELLO, p, sizeof(p));
}

/* Timer: the host never showed up at the new rate. Runs in the esp_timer task and deliberately does
 * not go through evq (it may be full) or uart_lock (a writer stuck on a full TX ring would delay the
 * deadline): changing the rate under a writer only garbles the frame in flight, which the CRC drops. */
static void baud_revert(int revert) {
	if (!revert)
		return;
	uart_set_baudrate(HOSTLINK_UART, HOSTLINK_BAUD);
	hostlink_send_hello(); /* the host PINGs until it hears this, so a lost one is harmless */
}

static void baud_expire_cb(void *arg) {
	(void) arg;
	portENTER_CRITICAL(&baud_mux);
	int revert = baudsw_expire(&baud_sw);
	portEXIT_CRITICAL(&baud_mux);
	baud_revert(revert);
}

/* 1 s tick: a committed high rate with a silent host (bridge restarted, now PINGing at
 * HOSTLINK_BAUD) goes back to HOSTLINK_BAUD through the same path as an expired commit. */
static void baud_idle_cb(void *arg) {
	(void) arg;
	portENTER_CRITICAL(&baud_mux);
	int revert = baudsw_idle_check(&baud_sw, now_ms());
	portEXIT_CRITICAL(&baud_mux);
	baud_revert(revert);
}

/* Called from the awdl task (it is also the main writer, so it never races itself; every other
 * writer is excluded by uart_lock, held across OK + drain + switch so no frame is cut in half by
 * the rate change). The reader task needs no exclusion: it only drains the RX ring. */
static void handle_set_baud(const uint8_t *f, size_t n) {
	if (n < 5)
		return;
	uint32_t baud = (uint32_t) f[1] | (uint32_t) f[2] << 8 | (uint32_t) f[3] << 16 | (uint32_t) f[4] << 24;
	portENTER_CRITICAL(&baud_mux);
	enum baudsw_verdict v = baudsw_request(&baud_sw, baud, BAUDSW_UART_CLK_HZ);
	portEXIT_CRITICAL(&baud_mux);
	if (v != BAUDSW_ACCEPT)
		return;

	uint8_t p[4] = {(uint8_t) baud, (uint8_t) (baud >> 8), (uint8_t) (baud >> 16), (uint8_t) (baud >> 24)};
	/* hostlink_send takes uart_lock itself; take it around the whole sequence instead. */
	static uint8_t out[2 * (4 + 3) + 2];
	xSemaphoreTake(uart_lock, portMAX_DELAY);
	size_t len = slip_encode(HL_BAUD_OK, p, sizeof(p), out);
	uart_write_bytes(HOSTLINK_UART, out, len);
	if (uart_wait_tx_done(HOSTLINK_UART, pdMS_TO_TICKS(200)) != ESP_OK) {
		/* OK may not have left; stay put and let the host time out and come back. */
		xSemaphoreGive(uart_lock);
		return;
	}
	portENTER_CRITICAL(&baud_mux);
	baudsw_switched(&baud_sw, baud, now_ms());
	portEXIT_CRITICAL(&baud_mux);
	uart_set_baudrate(HOSTLINK_UART, baud);
	/* Drop anything the host sent at the old rate so it cannot count as the first valid frame at
	 * the new one. The host PINGs only after it has received all of HL_BAUD_OK, i.e. after this. */
	uart_flush_input(HOSTLINK_UART);
	esp_timer_start_once(t_baud, (uint64_t) HOSTLINK_BAUD_COMMIT_MS * 1000);
	xSemaphoreGive(uart_lock);
	uint32_t actual = 0;
	uart_get_baudrate(HOSTLINK_UART, &actual);
	log_info("uart switched to %u baud (actual %u), waiting for host", (unsigned) baud, (unsigned) actual);
}

static void post(uint8_t type, uint8_t *data, uint16_t len, uint64_t t) {
	struct ev e = {.type = type, .len = len, .time_us = t, .data = data};
	if (xQueueSend(evq, &e, 0) != pdTRUE) {
		free(data);
		stats.rx_dropped++;
	}
}

static void uart_reader_task(void *arg) {
	(void) arg;
	static struct slip_decoder dec;
	uint8_t chunk[256];
	for (;;) {
		int n = uart_read_bytes(HOSTLINK_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
		for (int i = 0; i < n; i++) {
			size_t len = slip_decode_byte(&dec, chunk[i]);
			if (!len)
				continue;
			/* First valid frame at a newly switched rate commits it. Done here, at decode
			 * time, so it does not depend on the (possibly full) event queue. */
			portENTER_CRITICAL(&baud_mux);
			int committed = baudsw_frame_ok(&baud_sw, now_ms());
			portEXIT_CRITICAL(&baud_mux);
			if (committed)
				esp_timer_stop(t_baud);
			if (esp_get_free_heap_size() < HEAP_FLOOR) {
				stats.rx_dropped++;
				continue;
			}
			uint8_t *copy = malloc(len);
			if (!copy)
				continue;
			memcpy(copy, dec.buf, len);
			post(EV_HOST_FRAME, copy, (uint16_t) len, clock_time_us());
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Things OWL expects from libc / its daemon                                */

char *ether_ntoa(const struct ether_addr *a) {
	static char s[18];
	const uint8_t *o = a->ether_addr_octet;
	snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", o[0], o[1], o[2], o[3], o[4], o[5]);
	return s;
}

int log_log(int level, const char *file, int line, const char *fmt, ...) {
	(void) file;
	(void) line;
	if (level > log_level)
		return 0;
	char msg[200];
	msg[0] = (uint8_t) level;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(msg + 1, sizeof(msg) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return 0;
	if (n > (int) sizeof(msg) - 2)
		n = sizeof(msg) - 2;
	hostlink_send(HL_LOG, (uint8_t *) msg, (size_t) n + 1);
	return 0;
}

void log_set_level(int level) { log_level = level; }

/* ------------------------------------------------------------------------ */
/* Wi-Fi                                                                    */

static void wifi_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
	const wifi_promiscuous_pkt_t *pkt = buf;
	uint64_t now = clock_time_us();
	stats.rx_seen++;
	if (pkt->rx_ctrl.rx_state != 0)
		return; /* bad FCS or other PHY error */
	uint16_t len = pkt->rx_ctrl.sig_len; /* includes the 4-byte FCS */
	enum rxfilter_kind kind = rxfilter_classify(pkt->payload, len, type == WIFI_PKT_MGMT, type == WIFI_PKT_DATA,
	                                            self_addr.ether_addr_octet);
	if (kind == RXF_DROP)
		return;
	if (kind == RXF_DATA) {
		uint16_t fc = (uint16_t) (pkt->payload[0] | pkt->payload[1] << 8);
		if (pkt->rx_ctrl.aggregation)
			stats.rx_data_ampdu++;
		if (fc & 0x0800)
			stats.rx_data_retry++;
		/* Decide before malloc: a duplicate costs nothing further. */
		if (rxdedup_check(&rx_dedup, pkt->payload)) {
			stats.rx_data_dup++;
			return;
		}
	}
	uint8_t *f = malloc(RXFILTER_RADIOTAP_LEN + len);
	if (!f) {
		stats.rx_dropped++;
		return;
	}
	uint16_t n = rxfilter_wrap(pkt->payload, len, (int8_t) pkt->rx_ctrl.rssi, f);
	if (kind == RXF_ACTION)
		stats.rx_awdl_action++;
	else
		stats.rx_awdl_data++;
	post(EV_WIFI_RX, f, n, now);
}

static int wlan_send(const uint8_t *buf, int len) {
	/* OWL prefixes a radiotap header meant for Linux injection; strip it. */
	if (len < 8)
		return -1;
	uint16_t rt_len = (uint16_t) (buf[2] | buf[3] << 8);
	if (rt_len > len || len - rt_len > WIFI_TX_MAX || len - rt_len < 24) {
		stats.tx_failed++;
		return -1;
	}
	esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, buf + rt_len, len - rt_len, true);
	if (err != ESP_OK) {
		stats.tx_failed++;
		return -1;
	}
	return 0;
}

static void wifi_start(void) {
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		nvs_flash_init();
	}
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	/* The hardware ACKs unicast addressed to the interface MAC, so the
	 * interface MAC must be our AWDL address. */
	ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, self_addr.ether_addr_octet));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20));
	wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_rx_cb));
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
	ESP_ERROR_CHECK(esp_wifi_set_channel(RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE));
	/* OWL's radiotap asks for 6 Mbit/s OFDM; keep that for action frames. */
	esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_6M);
}

/* ------------------------------------------------------------------------ */
/* AWDL glue (mirrors third_party/owl/daemon/core.c)                        */

static void timer_once(esp_timer_handle_t t, uint64_t in_us) {
	esp_timer_stop(t);
	esp_timer_start_once(t, in_us ? in_us : 1);
}

static void send_action(enum awdl_action_type type) {
	static uint8_t buf[1600];
	int len = awdl_init_full_action_frame(buf, &awdl, &ieee, type);
	if (len < 0)
		return;
	if (wlan_send(buf, len) == 0) {
		awdl.stats.tx_action++;
		stats.tx_action++;
	}
}

static int send_data(const struct buf *eth) {
	static uint8_t frame[1700];
	uint16_t ethertype;
	struct ether_addr src, dst;
	READ_BE16(eth, 12, &ethertype);
	READ_ETHER_ADDR(eth, 0, &dst);
	READ_ETHER_ADDR(eth, 6, &src);
	(void) ethertype;
	buf_strip(eth, 14);
	int len = awdl_init_full_data_frame(frame, &src, &dst, buf_data(eth), buf_len(eth), &awdl, &ieee);
	if (wlan_send(frame, len) < 0)
		return TX_FAIL;
	awdl.stats.tx_data++;
	stats.tx_data++;
	return TX_OK;
wire_error:
	return TX_FAIL;
}

static void handle_unicast(void) {
	uint64_t now = clock_time_us();
	while (uq_len) {
		struct buf *b = uq[uq_head];
		struct ether_addr dst;
		struct awdl_peer *peer;
		read_ether_addr(b, 0, &dst);
		if (awdl_peer_get(awdl.peers.peers, &dst, &peer) < 0) {
			log_debug("drop frame to non-peer %s", ether_ntoa(&dst));
		} else {
			double in = awdl_can_send_unicast_in(&awdl, peer, now, AWDL_UNICAST_GUARD_TU);
			if (in != 0) {
				if (in < 0)
					in = -in + usec_to_sec(ieee80211_tu_to_usec(AWDL_UNICAST_GUARD_TU));
				timer_once(t_tx, sec_to_usec(in));
				return;
			}
			send_data(b);
			awdl.stats.tx_data_unicast++;
		}
		buf_free(b);
		uq_head = (uq_head + 1) % UNICAST_QUEUE_LEN;
		uq_len--;
	}
}

static void handle_multicast(void) {
	uint64_t now = clock_time_us();
	while (mq_len) {
		double in = awdl_can_send_in(&awdl, now, AWDL_MULTICAST_GUARD_TU);
		if (!(awdl_is_multicast_eaw(&awdl, now) && in == 0)) {
			if (in == 0)
				in = usec_to_sec(ieee80211_tu_to_usec(16)); /* look again next slot */
			else if (in < 0)
				in = -in + usec_to_sec(ieee80211_tu_to_usec(AWDL_MULTICAST_GUARD_TU));
			timer_once(t_mc, sec_to_usec(in));
			return;
		}
		struct buf *b = mq[mq_head];
		send_data(b);
		awdl.stats.tx_data_multicast++;
		buf_free(b);
		mq_head = (mq_head + 1) % MULTICAST_QUEUE_LEN;
		mq_len--;
	}
}

static void handle_host_eth(const uint8_t *eth, size_t n) {
	if (n < 14 || n > ETHER_MAX_LEN)
		return;
	struct buf *b = buf_new_owned(n);
	memcpy((uint8_t *) buf_data(b), eth, n);
	if (eth[0] & 0x01) {
		if (mq_len == MULTICAST_QUEUE_LEN) {
			buf_free(b);
			return;
		}
		mq[(mq_head + mq_len++) % MULTICAST_QUEUE_LEN] = b;
		handle_multicast();
	} else {
		if (!memcmp(eth, self_addr.ether_addr_octet, 6)) {
			hostlink_send(HL_ETH, eth, n); /* to ourselves */
			buf_free(b);
			return;
		}
		struct ackc_flow nf;
		if (ackc_parse(eth, n, &nf)) {
			/* An older unsent pure ACK of the same flow is redundant (ACKs are cumulative), and every
			 * frame not sent is airtime in which we can hear the iPhone. Replace it in place rather than
			 * appending: the slot keeps its position, nothing is shifted, and the queue does not grow. */
			for (int i = uq_len - 1; i >= 0; i--) {
				int idx = (uq_head + i) % UNICAST_QUEUE_LEN;
				struct ackc_flow of;
				if (ackc_parse(buf_data(uq[idx]), buf_len(uq[idx]), &of) && ackc_replaces(&of, &nf)) {
					buf_free(uq[idx]);
					uq[idx] = b;
					uq_ack_merged++;
					return;
				}
			}
		}
		if (uq_len == UNICAST_QUEUE_LEN) {
			/* ACKs are cumulative, so the oldest frame is the cheapest to lose. */
			buf_free(uq[uq_head]);
			uq_head = (uq_head + 1) % UNICAST_QUEUE_LEN;
			uq_len--;
			stats.tx_uq_dropped++;
		}
		uq[(uq_head + uq_len++) % UNICAST_QUEUE_LEN] = b;
		handle_unicast();
	}
}

static enum awdl_chanseq_strategy chanseq_strategy(void) {
	switch (chanseq_mode) {
		case HL_CHANSEQ_INTERSECT:
			return AWDL_CHANSEQ_INTERSECT;
		case HL_CHANSEQ_FILL:
			return CHANSEQ_FILL;
		default:
			return AWDL_CHANSEQ_VERBATIM;
	}
}

static void handle_host_frame(const uint8_t *f, size_t n) {
	switch (f[0]) {
		case HL_ETH:
			handle_host_eth(f + 1, n - 1);
			break;
		case HL_SET_NAME: {
			size_t l = n - 1 < HOST_NAME_LENGTH_MAX ? n - 1 : HOST_NAME_LENGTH_MAX;
			memset(awdl.name, 0, sizeof(awdl.name));
			memcpy(awdl.name, f + 1, l);
			log_info("name set to %s", awdl.name);
			break;
		}
		case HL_SET_LOGLEVEL:
			if (n >= 2)
				log_level = f[1];
			break;
		case HL_SET_CHANSEQ:
			if (n >= 2 && (f[1] == HL_CHANSEQ_VERBATIM || f[1] == HL_CHANSEQ_INTERSECT || f[1] == HL_CHANSEQ_FILL)) {
				chanseq_mode = (enum hostlink_chanseq) f[1];
				stats.chanseq = f[1];
				chanseq_follow_master(&awdl, RADIO_CHANNEL, chanseq_strategy());
			}
			break;
		case HL_SET_BAUD:
			handle_set_baud(f, n);
			break;
		case HL_PING:
			hostlink_send_hello();
			break;
		default:
			break;
	}
}

static void handle_wifi_rx(uint8_t *f, size_t n, uint64_t t) {
	struct buf *out_arr[AWDL_RX_MAX_FRAMES];
	struct buf **out = &out_arr[0];
	const struct buf *frame = buf_new_const(f, n);
	awdl_rx_timestamp = t;
	int result = awdl_rx(frame, &out, &awdl);
	awdl_rx_timestamp = 0;
	/* Frames can be produced even when a later A-MSDU subframe fails: forward
	 * them only on success, but always free them. */
	for (struct buf **b = &out_arr[0]; b < out; b++) {
		if (result == RX_OK)
			hostlink_send(HL_ETH, buf_data(*b), buf_len(*b));
		buf_free(*b);
	}
	if (result < RX_OK)
		awdl.stats.rx_unknown++;
	buf_free(frame);
}

static void handle_mif(void) {
	uint64_t now = clock_time_us();
	uint64_t next_aw = awdl_sync_next_aw_us(now, &awdl.sync);
	uint64_t eaw_len = awdl.sync.presence_mode * awdl.sync.aw_period;
	int slot = awdl_sync_current_eaw(now, &awdl.sync) % AWDL_CHANSEQ_LENGTH;
	if (awdl_chan_num(awdl.channel.sequence[slot], awdl.channel.enc) == RADIO_CHANNEL)
		send_action(AWDL_ACTION_MIF);
	timer_once(t_mif, next_aw + ieee80211_tu_to_usec(eaw_len / 2));
}

static void handle_peers(void) {
	uint64_t cutoff = clock_time_us() - awdl.peers.timeout;
	awdl_peers_remove(awdl.peers.peers, cutoff, awdl.peer_remove_cb, awdl.peer_remove_cb_data);
	awdl_election_run(&awdl.election, &awdl.peers);
	chanseq_follow_master(&awdl, RADIO_CHANNEL, chanseq_strategy());
}

static void handle_stats(void) {
	stats.peers = (uint32_t) awdl_peers_length(awdl.peers.peers);
	hostlink_send(HL_STATS, (const uint8_t *) &stats, sizeof(stats));
	if (uq_ack_merged != uq_ack_merged_logged) {
		log_info("tcp acks coalesced: %u total (+%u)", (unsigned) uq_ack_merged,
		         (unsigned) (uq_ack_merged - uq_ack_merged_logged));
		uq_ack_merged_logged = uq_ack_merged;
	}
}

static void on_peer_add(struct awdl_peer *p, void *arg) {
	(void) arg;
	uint8_t msg[6 + HOST_NAME_LENGTH_MAX];
	size_t nl = strnlen(p->name, HOST_NAME_LENGTH_MAX);
	memcpy(msg, p->addr.ether_addr_octet, 6);
	memcpy(msg + 6, p->name, nl);
	hostlink_send(HL_PEER_ADD, msg, 6 + nl);
}

static void on_peer_remove(struct awdl_peer *p, void *arg) {
	(void) arg;
	hostlink_send(HL_PEER_DEL, p->addr.ether_addr_octet, 6);
}

static void timer_cb(void *arg) {
	post((uint8_t) (uintptr_t) arg, NULL, 0, 0);
}

static esp_timer_handle_t make_timer(enum ev_type type, const char *name) {
	esp_timer_handle_t t;
	esp_timer_create_args_t a = {.callback = timer_cb, .arg = (void *) (uintptr_t) type, .name = name};
	ESP_ERROR_CHECK(esp_timer_create(&a, &t));
	return t;
}

static void awdl_task(void *arg) {
	(void) arg;
	struct ev e;
	for (;;) {
		if (xQueueReceive(evq, &e, portMAX_DELAY) != pdTRUE)
			continue;
		switch (e.type) {
			case EV_WIFI_RX:
				handle_wifi_rx(e.data, e.len, e.time_us);
				break;
			case EV_HOST_FRAME:
				handle_host_frame(e.data, e.len);
				break;
			case EV_PSF:
				send_action(AWDL_ACTION_PSF);
				break;
			case EV_MIF:
				handle_mif();
				break;
			case EV_PEERS:
				handle_peers();
				break;
			case EV_TX_UNICAST:
				handle_unicast();
				break;
			case EV_TX_MULTICAST:
				handle_multicast();
				break;
			case EV_STATS:
				handle_stats();
				break;
		}
		free(e.data);
	}
}

void app_main(void) {
	uart_lock = xSemaphoreCreateMutex();
	evq = xQueueCreate(EVQ_LEN, sizeof(struct ev));
	baudsw_init(&baud_sw);

	uart_config_t uc = {
		.baud_rate = HOSTLINK_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	ESP_ERROR_CHECK(uart_driver_install(HOSTLINK_UART, 8192, 16384, 0, NULL, 0));
	ESP_ERROR_CHECK(uart_param_config(HOSTLINK_UART, &uc));

	/* Stable, locally administered unicast address derived from the chip. */
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	mac[0] = (uint8_t) ((mac[0] | 0x02) & ~0x01);
	memcpy(self_addr.ether_addr_octet, mac, 6);

	wifi_start();

	awdl_init_state(&awdl, "Fold5", &self_addr, CHAN_OPCLASS_6, clock_time_us());
	awdl.peer_cb = on_peer_add;
	awdl.peer_remove_cb = on_peer_remove;
	ieee80211_init_state(&ieee);
	ieee.fcs = false; /* the ESP32 MAC appends the FCS */

	t_psf = make_timer(EV_PSF, "psf");
	t_mif = make_timer(EV_MIF, "mif");
	t_peers = make_timer(EV_PEERS, "peers");
	t_tx = make_timer(EV_TX_UNICAST, "tx");
	t_mc = make_timer(EV_TX_MULTICAST, "mc");
	t_stats = make_timer(EV_STATS, "stats");
	esp_timer_create_args_t ba = {.callback = baud_expire_cb, .name = "baud"};
	ESP_ERROR_CHECK(esp_timer_create(&ba, &t_baud));
	esp_timer_create_args_t bi = {.callback = baud_idle_cb, .name = "baud_idle"};
	ESP_ERROR_CHECK(esp_timer_create(&bi, &t_baud_idle));
	ESP_ERROR_CHECK(esp_timer_start_periodic(t_baud_idle, 1000 * 1000));

	xTaskCreatePinnedToCore(awdl_task, "awdl", 8192, NULL, 20, NULL, 1);
	xTaskCreatePinnedToCore(uart_reader_task, "hostlink", 4096, NULL, 15, NULL, 0);

	ESP_ERROR_CHECK(esp_timer_start_periodic(t_psf, ieee80211_tu_to_usec(awdl.psf_interval)));
	ESP_ERROR_CHECK(esp_timer_start_periodic(t_peers, awdl.peers.clean_interval));
	ESP_ERROR_CHECK(esp_timer_start_periodic(t_stats, 5 * 1000 * 1000));
	timer_once(t_mif, 1000);

	hostlink_send_hello();
	log_info("AWDL radio up on channel %d as %s", RADIO_CHANNEL, ether_ntoa(&self_addr));
}
