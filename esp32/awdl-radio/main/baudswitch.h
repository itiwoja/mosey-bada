/*
 * HL_SET_BAUD state machine, free of ESP-IDF types so tests/baud_switch_test.c
 * exercises exactly what the firmware runs. The procedure itself is described
 * in hostlink_proto.h; this is the radio's half of it.
 *
 * The invariant: whatever happens (host never arrives at the new rate, bad
 * value, a second request in flight), the radio is at HOSTLINK_BAUD again no
 * later than HOSTLINK_BAUD_COMMIT_MS after it left it. The deadline timer lives
 * outside this file; it only has to call baudsw_expire() when it fires.
 *
 * Locking is the caller's job: the state is touched from the awdl task
 * (request), the UART reader task (frame_ok) and the esp_timer task (expire).
 */
#ifndef AWDL_BAUDSWITCH_H
#define AWDL_BAUDSWITCH_H

#include <stdint.h>

#include "hostlink_proto.h"

#define BAUDSW_MAX_BAUD 3000000u
#define BAUDSW_MAX_ERR_PERMILLE 20 /* 2% */
#ifndef BAUDSW_UART_CLK_HZ
#define BAUDSW_UART_CLK_HZ 80000000u /* ESP32 UART_SCLK_DEFAULT = APB */
#endif

/* After a high-rate commit the radio reverts to HOSTLINK_BAUD if no valid frame comes from the host
 * for this long. Covers a host bridge that restarted (it starts at HOSTLINK_BAUD and PINGs there,
 * which a radio left at the high rate can never decode). A live bridge at the high rate PINGs every
 * 500 ms after 3 s of silence, so 6 s = 3 s + margin never trips on a healthy link. */
#define BAUDSW_IDLE_REVERT_MS 6000

enum baudsw_state {
	BAUDSW_STABLE,  /* current rate is committed (HOSTLINK_BAUD at boot) */
	BAUDSW_PENDING, /* switched, waiting for the first valid frame */
};

struct baudsw {
	enum baudsw_state state;
	uint32_t cur;     /* rate the UART is running at */
	uint32_t pending; /* rate being tried while PENDING */
	uint32_t last_rx_ms; /* last valid host frame (or the switch itself); wraps, compare by difference */
};

enum baudsw_verdict {
	BAUDSW_IGNORE,
	BAUDSW_ACCEPT, /* send HL_BAUD_OK at the old rate, drain, switch, arm the timer */
};

/* The rate the UART divider really produces, computed like the IDF does
 * ((clk << 4) / baud in 1/16 steps) so the host test and the chip agree. */
static inline uint32_t baudsw_actual(uint32_t baud, uint32_t clk) {
	if (!baud)
		return 0;
	uint64_t div16 = ((uint64_t) clk << 4) / baud;
	if (!div16)
		return 0;
	return (uint32_t) (((uint64_t) clk << 4) / div16);
}

/* Above HOSTLINK_BAUD, at most 3 Mbaud, and reachable within 2%. */
static inline int baudsw_value_ok(uint32_t baud, uint32_t clk) {
	if (baud <= HOSTLINK_BAUD || baud > BAUDSW_MAX_BAUD)
		return 0;
	uint32_t act = baudsw_actual(baud, clk);
	uint32_t diff = act > baud ? act - baud : baud - act;
	return (uint64_t) diff * 1000 <= (uint64_t) baud * BAUDSW_MAX_ERR_PERMILLE;
}

static inline void baudsw_init(struct baudsw *s) {
	s->state = BAUDSW_STABLE;
	s->cur = HOSTLINK_BAUD;
	s->pending = 0;
}

/* HL_SET_BAUD arrived. A request while PENDING is ignored: the one frame that
 * could legitimately carry it was decoded at the new rate and has already
 * committed, so anything seen now is a stale duplicate from before the switch.
 * Asking for the rate we are already at is pointless and also ignored. */
static inline enum baudsw_verdict baudsw_request(const struct baudsw *s, uint32_t baud, uint32_t clk) {
	if (s->state != BAUDSW_STABLE || baud == s->cur || !baudsw_value_ok(baud, clk))
		return BAUDSW_IGNORE;
	return BAUDSW_ACCEPT;
}

/* The caller has switched the UART to `baud` and started the timer. */
static inline void baudsw_switched(struct baudsw *s, uint32_t baud, uint32_t now_ms) {
	s->last_rx_ms = now_ms;
	s->state = BAUDSW_PENDING;
	s->pending = baud;
	s->cur = baud;
}

/* A CRC-valid frame was decoded. Returns 1 when this commits the new rate
 * (caller stops the timer). Also refreshes the idle clock. */
static inline int baudsw_frame_ok(struct baudsw *s, uint32_t now_ms) {
	s->last_rx_ms = now_ms;
	if (s->state != BAUDSW_PENDING)
		return 0;
	s->state = BAUDSW_STABLE;
	return 1;
}

/* The deadline passed. Returns 1 when the UART must go back to HOSTLINK_BAUD
 * (and send HL_HELLO); 0 when it was committed in the meantime. */
static inline int baudsw_expire(struct baudsw *s) {
	if (s->state != BAUDSW_PENDING)
		return 0;
	s->state = BAUDSW_STABLE;
	s->cur = HOSTLINK_BAUD;
	return 1;
}

/* Periodic check. Returns 1 when a committed high rate has heard nothing valid for
 * BAUDSW_IDLE_REVERT_MS: the UART must go back to HOSTLINK_BAUD and send HL_HELLO, exactly like
 * baudsw_expire(). PENDING is left to the commit deadline. */
static inline int baudsw_idle_check(struct baudsw *s, uint32_t now_ms) {
	if (s->state != BAUDSW_STABLE || s->cur == HOSTLINK_BAUD)
		return 0;
	if ((uint32_t) (now_ms - s->last_rx_ms) < BAUDSW_IDLE_REVERT_MS)
		return 0;
	s->cur = HOSTLINK_BAUD;
	return 1;
}

#endif /* AWDL_BAUDSWITCH_H */
