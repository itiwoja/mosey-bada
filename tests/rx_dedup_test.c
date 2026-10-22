/*
 * Host test for rxdedup_check() in esp32/awdl-radio/main/rxfilter.h.
 * Build+run (from the repository root):
 *   cc -Wall -fsanitize=address,undefined -I esp32/awdl-radio/main \
 *      tests/rx_dedup_test.c -o /tmp/rx_dedup_test && /tmp/rx_dedup_test
 */
#include <assert.h>
#include <stdio.h>

#include "rxfilter.h"

static uint8_t frame[32];

static void mk(uint8_t ta, uint16_t seq, int retry) {
	memset(frame, 0, sizeof(frame));
	frame[0] = 0x08;
	frame[1] = retry ? 0x08 : 0x00;
	memset(frame + 10, ta, 6);
	frame[22] = (uint8_t) (seq << 4);
	frame[23] = (uint8_t) (seq >> 4);
}

int main(void) {
	struct rxdedup d;
	memset(&d, 0, sizeof(d));
	mk(1, 100, 0);
	assert(!rxdedup_check(&d, frame));
	mk(1, 100, 1); /* retry of a delivered frame */
	assert(rxdedup_check(&d, frame));
	assert(rxdedup_check(&d, frame)); /* and again */
	mk(1, 100, 0); /* same seq_ctrl without retry bit is a new frame */
	assert(!rxdedup_check(&d, frame));
	mk(1, 101, 1); /* retry whose original was lost: deliver */
	assert(!rxdedup_check(&d, frame));
	mk(2, 100, 1); /* same seq from another transmitter */
	assert(!rxdedup_check(&d, frame));
	/* remembers the last RXDEDUP_SEQS values only */
	for (uint16_t s = 200; s < 200 + RXDEDUP_SEQS; s++) {
		mk(1, s, 0);
		assert(!rxdedup_check(&d, frame));
	}
	mk(1, 200 + RXDEDUP_SEQS - 1, 1);
	assert(rxdedup_check(&d, frame));
	mk(1, 100, 1);
	assert(!rxdedup_check(&d, frame)); /* aged out */
	/* more transmitters than slots: oldest evicted, recent ones kept */
	for (uint8_t t = 10; t < 10 + RXDEDUP_PEERS + 3; t++) {
		mk(t, 7, 0);
		assert(!rxdedup_check(&d, frame));
	}
	mk(10 + RXDEDUP_PEERS + 2, 7, 1);
	assert(rxdedup_check(&d, frame));
	mk(10, 7, 1); /* evicted long ago */
	assert(!rxdedup_check(&d, frame));
	printf("rx_dedup_test: OK\n");
	return 0;
}
