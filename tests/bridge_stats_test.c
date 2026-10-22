/* Host test for awdl_bridge.c's HL_STATS formatter (old, new and truncated payloads).
 * The bridge itself needs Android/linux uapi headers that clash with glibc's, so the
 * function is cut out of the source and included here:
 *   sed -n '/^static int format_stats/,/^}/p' src/fold5/awdl_bridge.c > format_stats.inc
 *   cc -Wall -Wextra -DFORMAT_STATS_INC='"format_stats.inc"' -I. -o bridge_stats_test tests/bridge_stats_test.c
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../esp32/awdl-radio/main/hostlink_proto.h"
#include FORMAT_STATS_INC

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
	struct hostlink_stats s;
	char out[256];

	memset(&s, 0, sizeof(s));
	s.rx_seen = 7; s.peers = 2;
	s.rx_data_retry = 11; s.rx_data_dup = 5; s.rx_data_ampdu = 3; s.tx_uq_dropped = 4;
	s.chanseq = HL_CHANSEQ_INTERSECT;

	/* old firmware: V1 size only, no extra fields shown */
	CHECK(format_stats((uint8_t *) &s, HOSTLINK_STATS_V1_SIZE, out, sizeof(out)) == 0);
	CHECK(strstr(out, "seen 7") && strstr(out, "peers 2"));
	CHECK(!strstr(out, "rx retry") && !strstr(out, "chanseq"));

	/* new firmware: full struct */
	CHECK(format_stats((uint8_t *) &s, sizeof(s), out, sizeof(out)) == 0);
	CHECK(strstr(out, "| rx retry 11 dup 5 ampdu 3 | uq drop 4 | chanseq intersect"));

	/* too short: ignored */
	out[0] = 0;
	CHECK(format_stats((uint8_t *) &s, HOSTLINK_STATS_V1_SIZE - 1, out, sizeof(out)) == -1);
	CHECK(out[0] == 0);

	printf(fails ? "FAILED\n" : "OK: 3 cases\n");
	return fails != 0;
}
