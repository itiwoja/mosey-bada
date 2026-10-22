/* Round-trip and corruption tests for esp32/awdl-radio/main/hostlink_proto.h.
 *   cc -fsanitize=address,undefined -I esp32/awdl-radio/main tests/hostlink_proto_test.c && ./a.out */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hostlink_proto.h"

static uint8_t enc[2 * (HOSTLINK_MAX_PAYLOAD + 3) + 2];

int main(void) {
	struct slip_decoder d = {0};
	srand(1234);
	/* garbage before the first frame (ROM boot text) must not produce frames */
	const char *boot = "ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON_RESET)\r\n\xc0junk\xc0";
	for (const char *p = boot; *p; p++)
		assert(slip_decode_byte(&d, (uint8_t) *p) == 0);

	int ok = 0, rejected = 0;
	for (int iter = 0; iter < 20000; iter++) {
		size_t n = (size_t) rand() % (HOSTLINK_MAX_PAYLOAD + 1);
		uint8_t payload[HOSTLINK_MAX_PAYLOAD];
		for (size_t i = 0; i < n; i++) /* bias towards the special bytes */
			payload[i] = (rand() % 4 == 0) ? (rand() % 2 ? SLIP_END : SLIP_ESC) : (uint8_t) rand();
		uint8_t type = (uint8_t) rand();
		size_t len = slip_encode(type, payload, n, enc);
		assert(len <= sizeof(enc));
		int corrupt = iter % 5 == 0;
		if (corrupt) {
			size_t at = 1 + (size_t) rand() % (len - 2);
			enc[at] ^= (uint8_t) (1 + rand() % 255);
		}
		size_t got = 0;
		for (size_t i = 0; i < len; i++) {
			size_t r = slip_decode_byte(&d, enc[i]);
			if (r)
				got = r;
		}
		if (!corrupt) {
			assert(got == n + 1);
			assert(d.buf[0] == type);
			assert(memcmp(d.buf + 1, payload, n) == 0);
			ok++;
		} else if (!got) {
			rejected++;
		} else {
			/* a corrupted frame that still decodes must at least not be the original */
			assert(got != n + 1 || d.buf[0] != type || memcmp(d.buf + 1, payload, n) != 0);
		}
	}
	/* oversized frame is dropped, and the decoder recovers */
	for (int i = 0; i < HOSTLINK_MAX_PAYLOAD + 100; i++)
		assert(slip_decode_byte(&d, 0x41) == 0);
	assert(slip_decode_byte(&d, SLIP_END) == 0);
	size_t len = slip_encode(HL_PING, NULL, 0, enc);
	size_t got = 0;
	for (size_t i = 0; i < len; i++)
		got = got ? got : slip_decode_byte(&d, enc[i]);
	assert(got == 1 && d.buf[0] == HL_PING);
	printf("hostlink: %d round trips ok, %d corrupted frames rejected by CRC\n", ok, rejected);
	return 0;
}
