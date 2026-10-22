/*
 * Host test for the HL_SET_BAUD state machine in esp32/awdl-radio/main/baudswitch.h.
 * Build+run (from the repository root):
 *   cc -Wall -fsanitize=address,undefined -I esp32/awdl-radio/main \
 *      tests/baud_switch_test.c -o /tmp/baud_switch_test && /tmp/baud_switch_test
 */
#include <assert.h>
#include <stdio.h>

#include "baudswitch.h"

#define CLK BAUDSW_UART_CLK_HZ

static void accept_and_switch(struct baudsw *s, uint32_t b) {
	assert(baudsw_request(s, b, CLK) == BAUDSW_ACCEPT);
	baudsw_switched(s, b, 0);
	assert(s->state == BAUDSW_PENDING && s->cur == b);
}

int main(void) {
	struct baudsw s;

	/* value validation */
	baudsw_init(&s);
	assert(s.cur == HOSTLINK_BAUD && s.state == BAUDSW_STABLE);
	assert(baudsw_request(&s, 0, CLK) == BAUDSW_IGNORE);
	assert(baudsw_request(&s, 115200, CLK) == BAUDSW_IGNORE);
	assert(baudsw_request(&s, HOSTLINK_BAUD, CLK) == BAUDSW_IGNORE);  /* not above 921600 */
	assert(baudsw_request(&s, 3000001, CLK) == BAUDSW_IGNORE);
	assert(baudsw_request(&s, 0xffffffffu, CLK) == BAUDSW_IGNORE);
	assert(baudsw_request(&s, 1000000, CLK) == BAUDSW_ACCEPT);
	assert(baudsw_request(&s, 2000000, CLK) == BAUDSW_ACCEPT);
	assert(baudsw_request(&s, 3000000, CLK) == BAUDSW_ACCEPT);
	assert(baudsw_request(&s, 921601, CLK) == BAUDSW_ACCEPT); /* within 2% */
	printf("values: <=921600, >3M, junk ignored; 1M/2M/3M accepted\n");

	/* accepted -> committed */
	baudsw_init(&s);
	accept_and_switch(&s, 2000000);
	assert(baudsw_frame_ok(&s, 0) == 1 && s.state == BAUDSW_STABLE && s.cur == 2000000);
	assert(baudsw_frame_ok(&s, 0) == 0);   /* later frames are no-ops */
	assert(baudsw_expire(&s) == 0);     /* a late timer does nothing */
	assert(s.cur == 2000000);
	/* and a further switch from the committed rate works */
	assert(baudsw_request(&s, 3000000, CLK) == BAUDSW_ACCEPT);
	assert(baudsw_request(&s, 2000000, CLK) == BAUDSW_IGNORE); /* same as current */
	printf("accept -> first valid frame -> committed\n");

	/* accepted -> expired */
	baudsw_init(&s);
	accept_and_switch(&s, 3000000);
	assert(baudsw_expire(&s) == 1);
	assert(s.state == BAUDSW_STABLE && s.cur == HOSTLINK_BAUD);
	assert(baudsw_expire(&s) == 0);
	assert(baudsw_frame_ok(&s, 0) == 0); /* too late to commit */
	assert(baudsw_request(&s, 2000000, CLK) == BAUDSW_ACCEPT); /* host can retry */
	printf("accept -> deadline -> back to HOSTLINK_BAUD, retry possible\n");

	/* request while pending is ignored; the deadline still reverts */
	baudsw_init(&s);
	accept_and_switch(&s, 2000000);
	assert(baudsw_request(&s, 3000000, CLK) == BAUDSW_IGNORE);
	assert(baudsw_request(&s, 2000000, CLK) == BAUDSW_IGNORE);
	assert(s.pending == 2000000 && s.cur == 2000000);
	assert(baudsw_expire(&s) == 1 && s.cur == HOSTLINK_BAUD);
	printf("request while pending ignored, deadline still reverts\n");

	/* committed fast, then the host goes silent (bridge restarted) -> back to 921600 */
	baudsw_init(&s);
	accept_and_switch(&s, 2000000);
	assert(baudsw_frame_ok(&s, 100) == 1);
	assert(baudsw_idle_check(&s, 100 + BAUDSW_IDLE_REVERT_MS - 1) == 0);
	assert(baudsw_idle_check(&s, 100 + BAUDSW_IDLE_REVERT_MS) == 1);
	assert(s.cur == HOSTLINK_BAUD && s.state == BAUDSW_STABLE);
	assert(baudsw_idle_check(&s, 100000) == 0); /* already at 921600: nothing to do */
	assert(baudsw_request(&s, 2000000, CLK) == BAUDSW_ACCEPT);
	/* a live host (valid frame at least every 3 s) never trips it, also across ms wraparound */
	accept_and_switch(&s, 3000000);
	uint32_t t = 0xfffffe00u;
	assert(baudsw_frame_ok(&s, t) == 1);
	for (int i = 0; i < 100; i++) {
		t += 3000;
		assert(baudsw_idle_check(&s, t) == 0);
		assert(baudsw_frame_ok(&s, t) == 0);
	}
	assert(s.cur == 3000000);
	/* PENDING is not touched by the idle check (the commit deadline owns it) */
	baudsw_init(&s);
	accept_and_switch(&s, 2000000);
	assert(baudsw_idle_check(&s, 100000) == 0 && s.state == BAUDSW_PENDING);
	printf("committed fast + 6 s silence -> 921600; frames every 3 s keep it\n");

	/* invalid value never changes state */
	baudsw_init(&s);
	assert(baudsw_request(&s, 5000000, CLK) == BAUDSW_IGNORE && s.state == BAUDSW_STABLE && s.cur == HOSTLINK_BAUD);

	/* the divider model: every accepted rate is within 2% on the real clock */
	for (uint32_t b = HOSTLINK_BAUD + 1; b <= BAUDSW_MAX_BAUD; b += 997) {
		if (baudsw_value_ok(b, CLK)) {
			uint32_t a = baudsw_actual(b, CLK);
			uint32_t d = a > b ? a - b : b - a;
			assert((uint64_t) d * 50 <= b);
		}
	}
	assert(baudsw_actual(3000000, CLK) == 3004694 || baudsw_actual(3000000, CLK) == 3004693);
	printf("baud_switch_test: OK\n");
	return 0;
}
