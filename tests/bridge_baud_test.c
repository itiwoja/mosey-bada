/* Host test for the UART speed negotiation state machine in awdl_bridge.c.
 * The block between the BAUD-NEG markers is pure, so it is cut out and included:
 *   sed -n '/^\/\* BAUD-NEG BEGIN/,/^\/\* BAUD-NEG END/p' src/fold5/awdl_bridge.c > baud_neg.inc
 *   cc -Wall -Wextra -DBAUD_NEG_INC='"baud_neg.inc"' -I. -o bridge_baud_test tests/bridge_baud_test.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../esp32/awdl-radio/main/hostlink_proto.h"
#include BAUD_NEG_INC

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static struct baud_neg n;
static struct baud_act a;
static uint32_t t;
static unsigned host; /* what the simulated host UART is set to */

static void apply(void) {
	if (a.host_baud)
		host = a.host_baud;
}
static void rx(uint8_t type, uint32_t arg) { baud_rx(&n, type, arg, t, &a); apply(); }
static void tick(uint32_t dt) { t += dt; baud_tick(&n, t, &a); apply(); }

/* advance in 50 ms steps until pred, at most limit ms; returns elapsed ms or -1 */
#define UNTIL(pred, limit, out) do { int e_ = 0; out = -1; while (e_ <= (limit)) { if (pred) { out = e_; break; } tick(50); e_ += 50; } } while (0)

static void start(const char *list) {
	unsigned c[BAUD_MAX_CAND];
	int cnt;
	CHECK(baud_parse(list, c, &cnt) == 0);
	baud_init(&n, c, cnt);
	host = HOSTLINK_BAUD;
	t = 1000;
}

int main(void) {
	int el;
	unsigned c[BAUD_MAX_CAND];
	int cnt;

	/* parsing */
	CHECK(baud_parse(NULL, c, &cnt) == 0 && cnt == 3 && c[0] == 2000000 && c[2] == 1000000);
	CHECK(baud_parse("921600", c, &cnt) == 0 && cnt == 0);
	CHECK(baud_parse("0", c, &cnt) == 0 && cnt == 0);
	CHECK(baud_parse("2000000,2000000", c, &cnt) == 0 && cnt == 1);
	CHECK(baud_parse("abc", c, &cnt) == -1);
	CHECK(baud_parse("2000000,", c, &cnt) == -1);
	CHECK(baud_parse("2000000,0", c, &cnt) == -1);
	CHECK(baud_parse("115200", c, &cnt) == -1);
	CHECK(baud_parse("9000000", c, &cnt) == -1);
	CHECK(cp210x_max_baud(0x02) == 1000000 && cp210x_max_baud(0x20) == 3000000 && cp210x_max_baud(0x77) == 0);
	{
		unsigned f[3] = {2000000, 1500000, 1000000};
		int fn = 3;
		baud_filter(f, &fn, 1000000);
		CHECK(fn == 1 && f[0] == 1000000);
	}

	/* 1. old firmware: no BAUD_OK ever. Every candidate times out; stay at 921600 */
	start("2000000,1500000,1000000");
	CHECK(!baud_busy(&n));
	rx(HL_ETH, 0); /* data before the first HELLO changes nothing */
	CHECK(n.st == BS_INIT && !a.send_set_baud);
	rx(HL_HELLO, 0);
	CHECK(a.send_set_baud == 2000000 && baud_busy(&n));
	int sets = 1;
	for (el = 0; n.st != BS_DONE && el < 5000; el += 50) {
		tick(50);
		if (a.send_set_baud) {
			sets++;
			CHECK(a.send_set_baud == (sets == 2 ? 1500000u : 1000000u));
		}
	}
	CHECK(sets == 3 && n.st == BS_DONE && host == HOSTLINK_BAUD && !baud_busy(&n));
	CHECK(el <= 3 * BAUD_OK_WAIT_MS);
	CHECK(strstr(a.msg, "old firmware") && strstr(a.msg, "921600"));
	printf("old firmware: back at 921600 after %d ms\n", el);
	/* a later HELLO (no BAUD_OK was ever seen) does not start it again */
	rx(HL_HELLO, 0);
	CHECK(n.st == BS_DONE && !a.send_set_baud);

	/* 2. first candidate: BAUD_OK but dead link; second one works */
	start("2000000,1500000");
	rx(HL_HELLO, 0);
	CHECK(a.send_set_baud == 2000000);
	t += 10;
	rx(HL_BAUD_OK, 1500000); /* wrong rate: ignored */
	CHECK(n.st == BS_WAIT_OK && host == HOSTLINK_BAUD);
	rx(HL_BAUD_OK, 2000000);
	CHECK(host == 2000000 && a.ping && n.st == BS_WAIT_HELLO && baud_busy(&n));
	/* no HELLO at 2M */
	int pings = 0, sent_next = 0;
	uint32_t t0 = t;
	for (el = 0; host != HOSTLINK_BAUD && el < 3000; el += 50) {
		tick(50);
		pings += a.ping;
	}
	CHECK(host == HOSTLINK_BAUD && n.st == BS_RECOVER && pings >= 5);
	CHECK(t - t0 <= HOSTLINK_BAUD_COMMIT_MS + 50);
	CHECK(strstr(a.msg, "back to 921600"));
	printf("candidate1 dead: at 921600 after %u ms\n", (unsigned) (t - t0));
	/* radio's HELLO comes back at 921600: next candidate, no restart init */
	rx(HL_HELLO, 0);
	CHECK(a.send_set_baud == 1500000 && !a.reinit && n.st == BS_WAIT_OK && n.idx == 1);
	sent_next = 1;
	rx(HL_BAUD_OK, 1500000);
	CHECK(host == 1500000);
	rx(HL_HELLO, 0); /* HELLO at the new rate */
	CHECK(n.st == BS_FAST && !baud_busy(&n) && strstr(a.msg, "uart at 1500000 baud"));
	CHECK(sent_next);

	/* 3. all fail: stay at 921600 (radio did answer BAUD_OK, so a reboot may retry) */
	start("2000000,1500000,1000000");
	rx(HL_HELLO, 0);
	t0 = t;
	for (int k = 0; k < 3; k++) {
		CHECK(n.st == BS_WAIT_OK);
		rx(HL_BAUD_OK, n.cand[k]);
		UNTIL(host == HOSTLINK_BAUD, 3000, el);
		CHECK(el >= 0 && n.st == BS_RECOVER);
		rx(HL_HELLO, 0);
	}
	CHECK(n.st == BS_DONE && host == HOSTLINK_BAUD && !baud_busy(&n) && n.retry_ok);
	CHECK(strstr(a.msg, "no candidate rate worked"));
	printf("all dead (3 BAUD_OK, no link): done after %u ms\n", (unsigned) (t - t0));
	rx(HL_HELLO, 0); /* radio rebooted later: negotiate again */
	CHECK(a.send_set_baud == 2000000 && n.idx == 0);

	/* 4. link lost while the HELLO is slow: radio committed but HELLO lost; probe finds it */
	start("2000000");
	rx(HL_HELLO, 0);
	rx(HL_BAUD_OK, 2000000);
	UNTIL(host == HOSTLINK_BAUD, 3000, el);
	CHECK(n.st == BS_RECOVER);
	UNTIL(host == 2000000, 1500, el); /* periodic probe at the candidate rate */
	CHECK(el >= 0 && n.probing && a.ping);
	rx(HL_HELLO, 0);
	CHECK(n.st == BS_FAST && host == 2000000);
	/* without an answer the probe ends and we are back at 921600 */
	start("2000000");
	rx(HL_HELLO, 0); rx(HL_BAUD_OK, 2000000);
	UNTIL(host == HOSTLINK_BAUD && n.st == BS_RECOVER, 3000, el);
	UNTIL(host == 2000000, 1500, el);
	UNTIL(host == HOSTLINK_BAUD, 1000, el);
	CHECK(el >= 0 && !n.probing);

	/* 5. FAST: traffic keeps it alive; silence pings first, a reset is detected at 4 s */
	start("2000000");
	rx(HL_HELLO, 0); rx(HL_BAUD_OK, 2000000); rx(HL_HELLO, 0);
	CHECK(n.st == BS_FAST);
	for (int k = 0; k < 100; k++) { tick(100); if (k % 20 == 0) rx(HL_STATS, 0); } /* 5 s STATS cadence */
	CHECK(n.st == BS_FAST && host == 2000000);
	pings = 0;
	t0 = t;
	rx(HL_ETH, 0);
	t0 = t;
	for (el = 0; n.st == BS_FAST && el < 6000; el += 50) { tick(50); pings += a.ping; }
	CHECK(n.st == BS_RECOVER && host == HOSTLINK_BAUD && n.restart);
	CHECK(pings >= 2); /* PINGs between 3 s and 4 s, plus the one entering RECOVER */
	CHECK((t - t0) >= BAUD_SILENT_DEAD_MS && (t - t0) <= BAUD_SILENT_DEAD_MS + 50);
	CHECK(strstr(a.msg, "radio reset"));
	printf("reset at high rate: detected after %u ms\n", (unsigned) (t - t0));
	/* a live radio answering the silence PING keeps FAST (HELLO at 2M) */
	start("2000000");
	rx(HL_HELLO, 0); rx(HL_BAUD_OK, 2000000); rx(HL_HELLO, 0);
	tick(3200);
	CHECK(n.st == BS_FAST && a.ping);
	rx(HL_HELLO, 0);
	tick(3500);
	CHECK(n.st == BS_FAST);

	/* 6. reboot -> HELLO at 921600 -> full re-init and renegotiation from the top */
	start("2000000,1500000");
	rx(HL_HELLO, 0); rx(HL_BAUD_OK, 2000000); rx(HL_HELLO, 0);
	tick(4000);
	CHECK(n.st == BS_RECOVER && host == HOSTLINK_BAUD);
	rx(HL_HELLO, 0);
	CHECK(a.reinit && a.send_set_baud == 2000000 && n.st == BS_WAIT_OK && n.idx == 0);
	rx(HL_BAUD_OK, 2000000);
	rx(HL_HELLO, 0);
	CHECK(n.st == BS_FAST && host == 2000000 && strstr(a.msg, "uart at 2000000 baud"));

	/* 7. negotiation disabled / nothing left: no SET_BAUD at all */
	start("0");
	rx(HL_HELLO, 0);
	CHECK(n.st == BS_DONE && !a.send_set_baud && !baud_busy(&n));
	rx(HL_HELLO, 0);
	CHECK(!a.send_set_baud);

	printf(fails ? "FAILED\n" : "OK: 7 scenarios\n");
	return fails != 0;
}
