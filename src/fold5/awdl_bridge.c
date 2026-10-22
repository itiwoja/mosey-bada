/*
 * awdl_bridge: connect the ESP32 AWDL radio (esp32/awdl-radio) to a TAP
 * interface on the Fold5, so that IPv6 programs (OpenDrop) can use AWDL.
 *
 * The Fold5 kernel has no CP210x/CH34x serial driver, so the USB-UART bridge
 * on the ESP32 board is driven directly through usbfs. Needs root (usbfs node,
 * /dev/tun, interface configuration).
 *
 *   awdl_bridge [-n NAME] [-i IFNAME] [-v LEVEL] [--no-reset]
 *   awdl_bridge --tap-only      # diagnostics: set up awdl0 without the ESP32,
 *                               # log the frames the phone would send over AWDL
 *
 * Never transmits anything on the Fold5's own Wi-Fi; all radio work happens on
 * the ESP32.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/if_tun.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../esp32/awdl-radio/main/hostlink_proto.h"

#define ETH_FRAME_LEN_MAX 1518

enum chip { CHIP_CP210X, CHIP_CH34X };

static const struct {
	uint16_t vid, pid;
	enum chip chip;
	const char *name;
} known[] = {
	{0x10c4, 0xea60, CHIP_CP210X, "CP210x"},
	{0x1a86, 0x7523, CHIP_CH34X, "CH340"},
	{0x1a86, 0x5523, CHIP_CH34X, "CH341"},
	{0x1a86, 0x55d4, CHIP_CH34X, "CH9102"},
};

static int usb_fd = -1, tap_fd = -1;
static unsigned ep_in, ep_out;
static enum chip chip;
static pthread_mutex_t usb_lock = PTHREAD_MUTEX_INITIALIZER;
static char ifname[IFNAMSIZ] = "awdl0";
static char awdl_name[64] = "Fold5";
static int radio_loglevel = 6;
static uint8_t radio_chanseq = HL_CHANSEQ_VERBATIM;  /* AIRDROP_CHANSEQ */
static uint8_t radio_mac[6];
static volatile int have_hello;
static volatile sig_atomic_t stop;
static int tap_only;
static FILE *capture;
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t capture_bytes;

/* Optional bounded Ethernet capture for diagnosing discovery across the UART.
 * Frames read from TAP are host->radio; frames received over UART are radio->host.
 * A successful UART write does not prove over-the-air delivery. */
static void capture_frame(const uint8_t *frame, size_t n) {
	if (!capture || n > ETH_FRAME_LEN_MAX)
		return;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	uint32_t hdr[] = {(uint32_t) ts.tv_sec, (uint32_t) (ts.tv_nsec / 1000), (uint32_t) n, (uint32_t) n};
	pthread_mutex_lock(&capture_lock);
	if (capture_bytes + sizeof(hdr) + n <= 8 * 1024 * 1024) {
		if (fwrite(hdr, sizeof(hdr), 1, capture) == 1 && fwrite(frame, n, 1, capture) == 1) {
			capture_bytes += sizeof(hdr) + n;
			fflush(capture);
		}
	}
	pthread_mutex_unlock(&capture_lock);
}

static int capture_open(void) {
	const char *path = getenv("AIRDROP_PCAP");
	if (!path || !*path)
		return 0;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -1;
	capture = fdopen(fd, "wb");
	if (!capture) {
		close(fd);
		return -1;
	}
	uint32_t hdr[] = {0xa1b2c3d4, 0x00040002, 0, 0, ETH_FRAME_LEN_MAX, 1};
	if (fwrite(hdr, sizeof(hdr), 1, capture) != 1)
		return -1;
	fflush(capture);
	capture_bytes = sizeof(hdr);
	return 0;
}

static void logf_(const char *fmt, ...) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%5ld.%03ld] ", (long) ts.tv_sec, ts.tv_nsec / 1000000);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------------ */
/* usbfs                                                                    */

static int read_hex_file(const char *path, unsigned *out) {
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	int ok = fscanf(f, "%x", out) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

static int read_dec_file(const char *path, unsigned *out) {
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	int ok = fscanf(f, "%u", out) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

static int find_device(char *node, size_t n) {
	DIR *d = opendir("/sys/bus/usb/devices");
	if (!d)
		return -1;
	struct dirent *e;
	int found = -1;
	while ((e = readdir(d))) {
		char p[512];
		unsigned vid, pid, bus, dev;
		snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/idVendor", e->d_name);
		if (read_hex_file(p, &vid))
			continue;
		snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/idProduct", e->d_name);
		if (read_hex_file(p, &pid))
			continue;
		for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
			if (known[i].vid != vid || known[i].pid != pid)
				continue;
			snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/busnum", e->d_name);
			if (read_dec_file(p, &bus))
				continue;
			snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/devnum", e->d_name);
			if (read_dec_file(p, &dev))
				continue;
			snprintf(node, n, "/dev/bus/usb/%03u/%03u", bus, dev);
			chip = known[i].chip;
			logf_("found %s (%04x:%04x) at %s", known[i].name, vid, pid, node);
			found = 0;
		}
		if (!found)
			break;
	}
	closedir(d);
	return found;
}

/* usbfs returns the device and configuration descriptors on read(). */
static int find_bulk_endpoints(void) {
	uint8_t desc[1024];
	lseek(usb_fd, 0, SEEK_SET);
	ssize_t n = read(usb_fd, desc, sizeof(desc));
	int iface = -1;
	for (ssize_t o = 0; o + 2 <= n && desc[o] >= 2; o += desc[o]) {
		uint8_t type = desc[o + 1];
		if (type == USB_DT_INTERFACE)
			iface = desc[o + 2];
		else if (type == USB_DT_ENDPOINT && iface == 0 && (desc[o + 3] & 0x03) == USB_ENDPOINT_XFER_BULK) {
			if (desc[o + 2] & USB_DIR_IN)
				ep_in = desc[o + 2];
			else
				ep_out = desc[o + 2];
		}
	}
	return (ep_in && ep_out) ? 0 : -1;
}

static int ctrl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len) {
	struct usbdevfs_ctrltransfer c = {
		.bRequestType = type, .bRequest = req, .wValue = value, .wIndex = index,
		.wLength = len, .timeout = 1000, .data = data,
	};
	return ioctl(usb_fd, USBDEVFS_CONTROL, &c);
}

/* CP210x (AN571) */
#define CP_OUT 0x41
#define CP_IFC_ENABLE 0x00
#define CP_SET_LINE_CTL 0x03
#define CP_SET_MHS 0x07
#define CP_SET_BAUDRATE 0x1e

/* CH34x (Linux drivers/usb/serial/ch341.c) */
#define CH_OUT 0x40
#define CH_IN 0xc0
#define CH_READ_VERSION 0x5f
#define CH_WRITE_REG 0x9a
#define CH_SERIAL_INIT 0xa1
#define CH_MODEM_CTRL 0xa4
#define CH_CLKRATE 48000000
#define CH_CLK_DIV(ps, fact) (1 << (12 - 3 * (ps) - (fact)))
#define CH_MIN_RATE(ps) (CH_CLKRATE / (CH_CLK_DIV((ps), 1) * 512))

static int ch34x_divisor(unsigned speed) {
	static const unsigned min_rates[] = {CH_MIN_RATE(0), CH_MIN_RATE(1), CH_MIN_RATE(2), CH_MIN_RATE(3)};
	int ps, fact = 1, clk_div, div;
	for (ps = 3; ps >= 0; ps--)
		if (speed > min_rates[ps])
			break;
	if (ps < 0)
		return -1;
	clk_div = CH_CLK_DIV(ps, fact);
	div = CH_CLKRATE / (clk_div * speed);
	if (div < 9 || div > 255) {
		div /= 2;
		clk_div *= 2;
		fact = 0;
	}
	if (div < 2)
		return -1;
	if (16 * CH_CLKRATE / (clk_div * div) - 16 * speed >= 16 * speed - 16 * CH_CLKRATE / (clk_div * (div + 1)))
		div++;
	if (fact == 1 && div % 2 == 0) {
		div /= 2;
		fact = 0;
	}
	return (0x100 - div) << 8 | fact << 2 | ps;
}

static int set_lines(int dtr, int rts) {
	if (chip == CHIP_CP210X)
		return ctrl(CP_OUT, CP_SET_MHS, (uint16_t) (0x0300 | (dtr ? 1 : 0) | (rts ? 2 : 0)), 0, NULL, 0);
	/* CH34x lines are active low in this register */
	return ctrl(CH_OUT, CH_MODEM_CTRL, (uint16_t) ~((dtr ? 1 << 5 : 0) | (rts ? 1 << 6 : 0)), 0, NULL, 0);
}

/* Change only the rate of an already set-up bridge. serial_setup() re-enables
 * the CP210x interface / re-inits the CH34x, either of which may touch DTR/RTS,
 * and on an ESP32 board those lines drive EN and IO0: a rate switch must not
 * risk resetting the radio into its ROM bootloader. */
static int serial_set_baud(unsigned baud) {
	if (chip == CHIP_CP210X) {
		uint8_t b[4] = {baud & 0xff, (baud >> 8) & 0xff, (baud >> 16) & 0xff, (baud >> 24) & 0xff};
		return ctrl(CP_OUT, CP_SET_BAUDRATE, 0, 0, b, 4) < 0 ? -1 : 0;
	}
	uint8_t ver[2] = {0, 0};
	ctrl(CH_IN, CH_READ_VERSION, 0, 0, ver, 2);
	int div = ch34x_divisor(baud);
	if (div < 0)
		return -1;
	if (ver[0] > 0x27)
		div |= 1 << 7;
	return ctrl(CH_OUT, CH_WRITE_REG, 0x1312, (uint16_t) div, NULL, 0) < 0 ? -1 : 0;
}

static int serial_setup(unsigned baud) {
	if (chip == CHIP_CP210X) {
		uint8_t b[4] = {baud & 0xff, (baud >> 8) & 0xff, (baud >> 16) & 0xff, (baud >> 24) & 0xff};
		if (ctrl(CP_OUT, CP_IFC_ENABLE, 1, 0, NULL, 0) < 0)
			return -1;
		if (ctrl(CP_OUT, CP_SET_BAUDRATE, 0, 0, b, 4) < 0)
			return -1;
		if (ctrl(CP_OUT, CP_SET_LINE_CTL, 0x0800, 0, NULL, 0) < 0) /* 8N1 */
			return -1;
		return 0;
	}
	uint8_t ver[2] = {0, 0};
	ctrl(CH_IN, CH_READ_VERSION, 0, 0, ver, 2);
	if (ctrl(CH_OUT, CH_SERIAL_INIT, 0, 0, NULL, 0) < 0)
		return -1;
	int div = ch34x_divisor(baud);
	if (div < 0)
		return -1;
	if (ver[0] > 0x27)
		div |= 1 << 7;
	if (ctrl(CH_OUT, CH_WRITE_REG, 0x1312, (uint16_t) div, NULL, 0) < 0)
		return -1;
	if (ctrl(CH_OUT, CH_WRITE_REG, 0x2518, 0x00c3, NULL, 0) < 0) /* RX+TX enable, 8N1 */
		return -1;
	logf_("CH34x version 0x%02x, divisor 0x%04x", ver[0], div);
	return 0;
}

/* BAUD-NEG BEGIN (cut out by tests/bridge_baud_test.c: keep this block free of I/O) */
/* UART speed negotiation (protocol: see the HL_SET_BAUD comment in hostlink_proto.h).
 * Pure state machine: the caller feeds it received frames and clock ticks (ms) and
 * carries out the returned actions, so every failure path can be tested on the host.
 *
 *   INIT --HELLO--> WAIT_OK --BAUD_OK--> WAIT_HELLO --any frame--> FAST
 *     |               | 500ms no OK         | 1500ms                 | 4s without frames
 *     v               v (next candidate)    v                        v
 *   DONE <--all--  (next candidate)       RECOVER (921600, PING; every 600ms a 300ms
 *     ^                                      | HELLO at 921600       probe at the candidate
 *     +---- no candidates left --------------+ -> next candidate     rate, in case the radio
 *                                                                    committed but its HELLO
 *   FAST death goes to RECOVER with restart: HELLO -> full re-init   got lost)
 *   and negotiation from the first candidate. */
#define BAUD_MAX_CAND 8
#define BAUD_MAX_RATE 3000000          /* CP2102N limit */
#define BAUD_DEFAULT_LIST "2000000,1500000,1000000"
#define BAUD_OK_WAIT_MS 500
#define BAUD_PING_MS 250
#define BAUD_RECOVER_PING_MS 200
#define BAUD_RECOVER_BASE_MS 600
#define BAUD_RECOVER_PROBE_MS 300
/* The radio sends HL_STATS every 5 s, so silence alone is no proof of a reset.
 * After 3 s we PING (a live radio answers at once); at 4 s we give up on the rate. */
#define BAUD_SILENT_PING_MS 3000
#define BAUD_SILENT_DEAD_MS 4000

enum baud_state { BS_INIT, BS_WAIT_OK, BS_WAIT_HELLO, BS_RECOVER, BS_FAST, BS_DONE };

struct baud_neg {
	unsigned cand[BAUD_MAX_CAND];
	int ncand, idx;
	enum baud_state st;
	unsigned cur;                      /* rate the host UART is set to */
	int any_ok;                        /* the radio ever answered HL_BAUD_OK */
	int restart;                       /* RECOVER entered from FAST: radio probably rebooted */
	int probing;                       /* RECOVER: currently listening at the candidate rate */
	int retry_ok;                      /* DONE: a later HELLO (radio reboot) may renegotiate */
	uint32_t t_state, t_ping, t_phase, t_valid;
};

struct baud_act {
	unsigned send_set_baud;            /* != 0: send HL_SET_BAUD with this rate (at the current rate) */
	unsigned host_baud;                /* != 0: switch the host UART to this rate, then drop partial frames */
	int ping;                          /* send HL_PING */
	int reinit;                        /* resend HL_SET_LOGLEVEL / HL_SET_NAME: the radio rebooted */
	char msg[112];                     /* log line, empty if none */
};

/* "2000000,1500000" -> out[]. A lone "0" or "921600" means: do not negotiate (*cnt = 0).
 * Returns -1 for anything else that is not a rate in (921600, 3000000]. */
static int baud_parse(const char *s, unsigned *out, int *cnt) {
	unsigned long first = 0;
	int n = 0, tokens = 0;
	if (!s || !*s)
		s = BAUD_DEFAULT_LIST;
	*cnt = 0;
	for (;;) {
		char *end;
		if (*s < '0' || *s > '9')
			return -1;
		unsigned long v = strtoul(s, &end, 10);
		if (*end != ',' && *end != 0)
			return -1;
		if (tokens++ == 0)
			first = v;
		if (v <= HOSTLINK_BAUD || v > BAUD_MAX_RATE) {
			if (*end == 0 && tokens == 1 && (first == 0 || first == HOSTLINK_BAUD))
				return 0;
			return -1;
		}
		int dup = 0;
		for (int i = 0; i < n; i++)
			dup |= out[i] == v;
		if (!dup) {
			if (n == BAUD_MAX_CAND)
				return -1;
			out[n++] = (unsigned) v;
		}
		if (*end == 0)
			break;
		s = end + 1;
	}
	*cnt = n;
	return 0;
}

/* Highest rate a CP210x part does (AN571 GET_PARTNUM; values from Linux cp210x.c).
 * 0 = unknown part: do not filter, let the trial decide. */
static unsigned cp210x_max_baud(int partnum) {
	switch (partnum) {
		case 0x01: return HOSTLINK_BAUD;                  /* CP2101 */
		case 0x02: case 0x03: case 0x04: return 1000000;  /* CP2102/3/4 */
		case 0x20: case 0x21: case 0x22: return 3000000;  /* CP2102N */
		default: return 0;
	}
}

static void baud_filter(unsigned *c, int *n, unsigned maxrate) {
	int w = 0;
	for (int i = 0; i < *n; i++)
		if (!maxrate || c[i] <= maxrate)
			c[w++] = c[i];
	*n = w;
}

static void baud_init(struct baud_neg *n, const unsigned *cand, int ncand) {
	memset(n, 0, sizeof(*n));
	memcpy(n->cand, cand, sizeof(unsigned) * (size_t) ncand);
	n->ncand = ncand;
	n->cur = HOSTLINK_BAUD;
	n->st = BS_INIT;
}

/* The host must not send HL_ETH while the two ends may disagree on the rate:
 * the radio drains and switches after BAUD_OK, and a frame sent at the wrong
 * rate is lost anyway (TCP retransmits it; the pause is a few seconds at most). */
static int baud_busy(const struct baud_neg *n) {
	return n->st == BS_WAIT_OK || n->st == BS_WAIT_HELLO || n->st == BS_RECOVER;
}

static void baud_begin(struct baud_neg *n, uint32_t now, struct baud_act *a) {
	a->send_set_baud = n->cand[n->idx];
	n->st = BS_WAIT_OK;
	n->t_state = now;
}

static void baud_giveup(struct baud_neg *n, struct baud_act *a) {
	n->st = BS_DONE;
	n->retry_ok = n->any_ok;
	if (n->any_ok)
		snprintf(a->msg, sizeof(a->msg), "baud: no candidate rate worked; staying at %u baud", HOSTLINK_BAUD);
	else
		snprintf(a->msg, sizeof(a->msg),
		         "baud: radio never answered HL_BAUD_OK (old firmware?); staying at %u baud", HOSTLINK_BAUD);
}

static void baud_next(struct baud_neg *n, uint32_t now, struct baud_act *a) {
	if (++n->idx >= n->ncand)
		baud_giveup(n, a);
	else
		baud_begin(n, now, a);
}

static void baud_to_recover(struct baud_neg *n, uint32_t now, struct baud_act *a) {
	n->st = BS_RECOVER;
	n->probing = 0;
	n->cur = HOSTLINK_BAUD;
	n->t_phase = n->t_ping = now;
	a->host_baud = HOSTLINK_BAUD;
	a->ping = 1;
}

static void baud_commit(struct baud_neg *n, uint32_t now, struct baud_act *a) {
	n->st = BS_FAST;
	n->restart = 0;
	n->t_valid = n->t_ping = now;
	snprintf(a->msg, sizeof(a->msg), "uart at %u baud", n->cur);
}

/* A valid frame arrived. type is the HL_* byte; arg is the rate of HL_BAUD_OK. */
static void baud_rx(struct baud_neg *n, uint8_t type, uint32_t arg, uint32_t now, struct baud_act *a) {
	memset(a, 0, sizeof(*a));
	n->t_valid = now;
	switch (n->st) {
		case BS_INIT:
			if (type != HL_HELLO)
				break;
			if (n->ncand)
				baud_begin(n, now, a);
			else
				n->st = BS_DONE;
			break;
		case BS_DONE:
			if (type == HL_HELLO && n->retry_ok && n->ncand) {
				/* nobody pings after startup, so a HELLO is a rebooted radio */
				n->idx = 0;
				n->any_ok = 0;
				baud_begin(n, now, a);
			}
			break;
		case BS_WAIT_OK:
			if (type == HL_BAUD_OK && arg == n->cand[n->idx]) {
				n->any_ok = 1;
				n->cur = arg;
				a->host_baud = arg;
				a->ping = 1;
				n->st = BS_WAIT_HELLO;
				n->t_state = n->t_ping = now;
			}
			break;
		case BS_WAIT_HELLO:
			if (type != HL_BAUD_OK) /* anything valid at the new rate proves it works */
				baud_commit(n, now, a);
			break;
		case BS_RECOVER:
			if (n->cur != HOSTLINK_BAUD) { /* probing: the radio is alive at the candidate rate */
				if (type != HL_BAUD_OK)
					baud_commit(n, now, a);
			} else if (type == HL_HELLO) {
				a->reinit = n->restart;
				if (n->restart) {
					n->restart = 0;
					n->idx = 0;
					n->any_ok = 0;
					baud_begin(n, now, a);
				} else
					baud_next(n, now, a);
			}
			break;
		case BS_FAST:
			break;
	}
}

static void baud_tick(struct baud_neg *n, uint32_t now, struct baud_act *a) {
	memset(a, 0, sizeof(*a));
	switch (n->st) {
		case BS_WAIT_OK:
			if (now - n->t_state >= BAUD_OK_WAIT_MS)
				baud_next(n, now, a);
			break;
		case BS_WAIT_HELLO:
			if (now - n->t_state >= HOSTLINK_BAUD_COMMIT_MS) {
				snprintf(a->msg, sizeof(a->msg), "baud: no reply at %u baud; back to %u", n->cur, HOSTLINK_BAUD);
				baud_to_recover(n, now, a);
			} else if (now - n->t_ping >= BAUD_PING_MS) {
				n->t_ping = now;
				a->ping = 1;
			}
			break;
		case BS_RECOVER:
			if (!n->probing && now - n->t_phase >= BAUD_RECOVER_BASE_MS) {
				n->probing = 1;
				n->cur = n->cand[n->idx];
				n->t_phase = n->t_ping = now;
				a->host_baud = n->cur;
				a->ping = 1;
			} else if (n->probing && now - n->t_phase >= BAUD_RECOVER_PROBE_MS) {
				n->probing = 0;
				n->cur = HOSTLINK_BAUD;
				n->t_phase = n->t_ping = now;
				a->host_baud = HOSTLINK_BAUD;
				a->ping = 1;
			} else if (now - n->t_ping >= BAUD_RECOVER_PING_MS) {
				n->t_ping = now;
				a->ping = 1;
			}
			break;
		case BS_FAST:
			if (now - n->t_valid >= BAUD_SILENT_DEAD_MS) {
				snprintf(a->msg, sizeof(a->msg),
				         "baud: no frames for %u ms at %u baud, radio reset? back to %u",
				         (unsigned) (now - n->t_valid), n->cur, HOSTLINK_BAUD);
				n->restart = 1;
				baud_to_recover(n, now, a);
			} else if (now - n->t_valid >= BAUD_SILENT_PING_MS && now - n->t_ping >= 500) {
				n->t_ping = now;
				a->ping = 1;
			}
			break;
		default:
			break;
	}
}
/* BAUD-NEG END */

static struct baud_neg neg;
static volatile int tx_paused;  /* HL_ETH to the radio is dropped while the UART rate is in flux */
static unsigned baud_user[BAUD_MAX_CAND];  /* AIRDROP_BAUD */
static int baud_user_n;

static int usb_open(int reset) {
	char node[64];
	if (find_device(node, sizeof(node)) < 0) {
		logf_("no ESP32 USB-UART bridge (CP210x/CH34x) found; is the ESP32 plugged in?");
		return -1;
	}
	usb_fd = open(node, O_RDWR);
	if (usb_fd < 0) {
		logf_("open %s: %s", node, strerror(errno));
		return -1;
	}
	if (find_bulk_endpoints() < 0) {
		logf_("no bulk endpoints on interface 0");
		return -1;
	}
	struct usbdevfs_ioctl disc = {.ifno = 0, .ioctl_code = USBDEVFS_DISCONNECT, .data = NULL};
	ioctl(usb_fd, USBDEVFS_IOCTL, &disc); /* in case a kernel driver is bound */
	unsigned ifno = 0;
	if (ioctl(usb_fd, USBDEVFS_CLAIMINTERFACE, &ifno) < 0) {
		logf_("claim interface: %s", strerror(errno));
		return -1;
	}
	if (serial_setup(HOSTLINK_BAUD) < 0) {
		logf_("serial setup failed: %s", strerror(errno));
		return -1;
	}
	if (reset) {
		/* DevKitC auto-reset: RTS drives EN low, DTR (IO0) stays high -> normal boot */
		set_lines(0, 1);
		usleep(100 * 1000);
	}
	set_lines(0, 0);
	logf_("serial ready at %u baud (endpoints in 0x%02x out 0x%02x)", HOSTLINK_BAUD, ep_in, ep_out);
	return 0;
}

/* droppable: HL_ETH, silently discarded while tx_paused. The check is under usb_lock, so
 * once a rate switch has taken the lock no data frame can still be half-way out at the old rate. */
static int usb_write(const uint8_t *p, size_t n, int droppable) {
	pthread_mutex_lock(&usb_lock);
	if (droppable && tx_paused) {
		pthread_mutex_unlock(&usb_lock);
		return 0;
	}
	while (n) {
		size_t chunk = n > 4096 ? 4096 : n;
		struct usbdevfs_bulktransfer b = {.ep = ep_out, .len = (unsigned) chunk, .timeout = 1000, .data = (void *) p};
		int r = ioctl(usb_fd, USBDEVFS_BULK, &b);
		if (r < 0) {
			pthread_mutex_unlock(&usb_lock);
			return -1;
		}
		p += r;
		n -= (size_t) r;
	}
	pthread_mutex_unlock(&usb_lock);
	return 0;
}

static int hl_send(uint8_t type, const uint8_t *payload, size_t n) {
	static uint8_t out[2 * (HOSTLINK_MAX_PAYLOAD + 3) + 2];
	static pthread_mutex_t enc_lock = PTHREAD_MUTEX_INITIALIZER;
	if (n > HOSTLINK_MAX_PAYLOAD)
		return -1;
	pthread_mutex_lock(&enc_lock);
	size_t len = slip_encode(type, payload, n, out);
	int r = usb_write(out, len, type == HL_ETH);
	pthread_mutex_unlock(&enc_lock);
	return r;
}

/* ------------------------------------------------------------------------ */
/* Network configuration                                                    */

static int run(const char *fmt, ...) {
	char cmd[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	int st = system(cmd);
	if (st != 0)
		logf_("command failed (%d): %s", st, cmd);
	return st;
}

static void write_sysctl(const char *name, const char *value) {
	char p[256];
	snprintf(p, sizeof(p), "/proc/sys/net/ipv6/conf/%s/%s", ifname, name);
	int fd = open(p, O_WRONLY);
	if (fd < 0)
		return;
	if (write(fd, value, strlen(value)) < 0)
		logf_("sysctl %s: %s", p, strerror(errno));
	close(fd);
}

static int tap_open(void) {
	tap_fd = open("/dev/tun", O_RDWR);
	if (tap_fd < 0)
		tap_fd = open("/dev/net/tun", O_RDWR);
	if (tap_fd < 0) {
		logf_("open tun: %s", strerror(errno));
		return -1;
	}
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(tap_fd, TUNSETIFF, &ifr) < 0) {
		logf_("TUNSETIFF %s: %s", ifname, strerror(errno));
		return -1;
	}
	return 0;
}

static void lladdr_from_mac(const uint8_t *m, char *out, size_t n) {
	snprintf(out, n, "fe80::%02x%02x:%02xff:fe%02x:%02x%02x", m[0] ^ 0x02, m[1], m[2], m[3], m[4], m[5]);
}

/* Called once the radio has told us its AWDL address. */
static int tap_configure(void) {
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
	memcpy(ifr.ifr_hwaddr.sa_data, radio_mac, 6);
	if (ioctl(s, SIOCSIFHWADDR, &ifr) < 0)
		logf_("SIOCSIFHWADDR: %s", strerror(errno));
	ifr.ifr_mtu = 1400; /* esp_wifi_80211_tx() caps a frame at 1500 bytes */
	if (ioctl(s, SIOCSIFMTU, &ifr) < 0)
		logf_("SIOCSIFMTU: %s", strerror(errno));
	write_sysctl("addr_gen_mode", "0"); /* EUI-64 link-local, like OWL */
	write_sysctl("accept_ra", "0");
	write_sysctl("accept_dad", "0");
	write_sysctl("disable_ipv6", "0");
	if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
		ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
		if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0)
			logf_("SIOCSIFFLAGS: %s", strerror(errno));
	}
	close(s);
	/* Do not depend on addrconf: the EUI-64 link-local is what peers derive
	 * from our MAC (OWL does the same), so set it explicitly. */
	char ll[64];
	lladdr_from_mac(radio_mac, ll, sizeof(ll));
	run("ip -6 addr replace %s/64 dev %s nodad", ll, ifname);
	/* Android routes by policy rules; give awdl0 its own table so link-local
	 * and multicast traffic bound to it can leave and come back. */
	run("ip -6 route replace fe80::/64 dev %s table 9001", ifname);
	run("ip -6 route replace ff00::/8 dev %s table 9001", ifname);
	run("ip -6 rule del pref 9000 2>/dev/null; ip -6 rule add pref 9000 oif %s lookup 9001", ifname);
	run("ip -6 rule del pref 9001 2>/dev/null; ip -6 rule add pref 9001 iif %s lookup 9001", ifname);
	logf_("%s up, MAC %02x:%02x:%02x:%02x:%02x:%02x, MTU 1400", ifname, radio_mac[0], radio_mac[1],
	      radio_mac[2], radio_mac[3], radio_mac[4], radio_mac[5]);
	return 0;
}

static void peer_add(const uint8_t *m, const char *name) {
	char ip6[64];
	lladdr_from_mac(m, ip6, sizeof(ip6));
	logf_("peer + %02x:%02x:%02x:%02x:%02x:%02x \"%s\" -> %s", m[0], m[1], m[2], m[3], m[4], m[5], name, ip6);
	run("ip -6 neigh replace %s lladdr %02x:%02x:%02x:%02x:%02x:%02x nud permanent dev %s", ip6, m[0], m[1], m[2],
	    m[3], m[4], m[5], ifname);
}

static void peer_del(const uint8_t *m) {
	char ip6[64];
	lladdr_from_mac(m, ip6, sizeof(ip6));
	logf_("peer - %02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	run("ip -6 neigh del %s dev %s 2>/dev/null", ip6, ifname);
}

/* ------------------------------------------------------------------------ */
/* Message handling                                                         */

/* Format a HL_STATS payload (without the type byte). Old firmware sends only the
 * first HOSTLINK_STATS_V1_SIZE bytes; the newer fields are shown only when present.
 * Returns 0 on success, -1 if the payload is too short to be a stats report. */
static int format_stats(const uint8_t *p, size_t n, char *out, size_t cap) {
	struct hostlink_stats s;
	if (n < HOSTLINK_STATS_V1_SIZE)
		return -1;
	memset(&s, 0, sizeof(s));
	memcpy(&s, p, n < sizeof(s) ? n : sizeof(s));
	int w = snprintf(out, cap,
	                 "stats: seen %u awdl_action %u awdl_data %u dropped %u | tx action %u data %u failed %u | peers %u",
	                 s.rx_seen, s.rx_awdl_action, s.rx_awdl_data, s.rx_dropped, s.tx_action, s.tx_data,
	                 s.tx_failed, s.peers);
	if (n >= sizeof(s) && w > 0 && (size_t) w < cap)
		snprintf(out + w, cap - w, " | rx retry %u dup %u ampdu %u | uq drop %u | chanseq %s", s.rx_data_retry,
		         s.rx_data_dup, s.rx_data_ampdu, s.tx_uq_dropped,
		         s.chanseq == HL_CHANSEQ_INTERSECT ? "intersect" : s.chanseq == HL_CHANSEQ_VERBATIM ? "verbatim" :
		         s.chanseq == HL_CHANSEQ_FILL ? "fill" : "?");
	return 0;
}

static void handle_radio_frame(const uint8_t *f, size_t n) {
	switch (f[0]) {
		case HL_ETH:
			capture_frame(f + 1, n - 1);
			if (have_hello && write(tap_fd, f + 1, n - 1) < 0 && errno != EIO)
				logf_("tap write: %s", strerror(errno));
			break;
		case HL_LOG:
			if (n >= 2)
				logf_("radio[%d] %.*s", f[1], (int) (n - 2), f + 2);
			break;
		case HL_HELLO:
			if (n >= 9) {
				if (f[1] != HOSTLINK_VERSION)
					logf_("radio speaks hostlink v%d, expected v%d", f[1], HOSTLINK_VERSION);
				if (!have_hello) {
					memcpy(radio_mac, f + 2, 6);
					logf_("radio hello: channel %d", f[8]);
					tap_configure();
					uint8_t lvl = (uint8_t) radio_loglevel;
					hl_send(HL_SET_LOGLEVEL, &lvl, 1);
					hl_send(HL_SET_NAME, (const uint8_t *) awdl_name, strlen(awdl_name));
					have_hello = 1;
				}
				/* Every HELLO, not just the first: a rebooted ESP32 forgets the strategy
				 * and falls back to its default. Old firmware ignores the command. */
				hl_send(HL_SET_CHANSEQ, &radio_chanseq, 1);
			}
			break;
		case HL_PEER_ADD:
			if (n >= 7) {
				char name[65];
				size_t nl = n - 7 < 64 ? n - 7 : 64;
				memcpy(name, f + 7, nl);
				name[nl] = 0;
				peer_add(f + 1, name);
			}
			break;
		case HL_PEER_DEL:
			if (n >= 7)
				peer_del(f + 1);
			break;
		case HL_STATS:
			{
				char line[256];
				if (format_stats(f + 1, n - 1, line, sizeof(line)) == 0)
					logf_("%s", line);
			}
			break;
		default:
			break;
	}
}

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* CP210x part number (AN571 GET_PARTNUM, as Linux cp210x.c reads it); -1 if the chip does not answer. */
static int cp210x_partnum(void) {
	uint8_t pn = 0;
	if (ctrl(0xc0, 0xff, 0x370b, 0, &pn, 1) != 1)
		return -1;
	return pn;
}

/* Drop candidates the USB-UART cannot do. Unknown chips keep them all: the trial decides. */
static void baud_prepare(void) {
	int n = baud_user_n;
	unsigned c[BAUD_MAX_CAND];
	memcpy(c, baud_user, sizeof(c));
	if (chip == CHIP_CP210X) {
		int pn = cp210x_partnum();
		unsigned max = pn < 0 ? 0 : cp210x_max_baud(pn);
		logf_("CP210x part number %s0x%02x, max %u baud", pn < 0 ? "unknown " : "", pn < 0 ? 0 : pn, max);
		baud_filter(c, &n, max);
	} else {
		int w = 0;
		for (int i = 0; i < n; i++)
			if (ch34x_divisor(c[i]) >= 0)
				c[w++] = c[i];
		n = w;
	}
	if (baud_user_n && !n)
		logf_("baud: no AIRDROP_BAUD candidate is supported by this USB-UART; staying at %u", HOSTLINK_BAUD);
	baud_init(&neg, c, n);
}

/* Carry out what the state machine asked for. Returns 1 if the host rate changed
 * (the caller then drops its half-decoded frame). */
static int baud_apply(const struct baud_act *a) {
	int changed = 0;
	tx_paused = baud_busy(&neg);
	if (a->msg[0])
		logf_("%s", a->msg);
	if (a->reinit) {
		uint8_t lvl = (uint8_t) radio_loglevel;
		hl_send(HL_SET_LOGLEVEL, &lvl, 1);
		hl_send(HL_SET_NAME, (const uint8_t *) awdl_name, strlen(awdl_name));
		hl_send(HL_SET_CHANSEQ, &radio_chanseq, 1);
	}
	if (a->send_set_baud) {
		uint8_t b[4] = {a->send_set_baud & 0xff, (a->send_set_baud >> 8) & 0xff, (a->send_set_baud >> 16) & 0xff,
		                (uint8_t) (a->send_set_baud >> 24)};
		hl_send(HL_SET_BAUD, b, 4);
	}
	if (a->host_baud) {
		pthread_mutex_lock(&usb_lock); /* no data frame may be mid-write while the rate changes */
		if (serial_set_baud(a->host_baud) < 0)
			logf_("baud: switching the USB-UART to %u failed: %s", a->host_baud, strerror(errno));
		pthread_mutex_unlock(&usb_lock);
		changed = 1;
	}
	if (a->ping)
		hl_send(HL_PING, NULL, 0);
	return changed;
}

static void *usb_reader(void *arg) {
	(void) arg;
	static struct slip_decoder dec;
	uint8_t buf[4096];
	while (!stop) {
		struct usbdevfs_bulktransfer b = {.ep = ep_in, .len = sizeof(buf), .timeout = 200, .data = buf};
		int r = ioctl(usb_fd, USBDEVFS_BULK, &b);
		if (r < 0) {
			if (errno == ETIMEDOUT)
				continue;
			logf_("usb read: %s", strerror(errno));
			stop = 1;
			break;
		}
		for (int i = 0; i < r; i++) {
			size_t len = slip_decode_byte(&dec, buf[i]);
			if (len) {
				handle_radio_frame(dec.buf, len);
				/* after handle_radio_frame: a HELLO's HL_SET_* must precede HL_SET_BAUD */
				struct baud_act a;
				uint32_t ok_rate = 0;
				if (dec.buf[0] == HL_BAUD_OK && len >= 5)
					ok_rate = dec.buf[1] | dec.buf[2] << 8 | dec.buf[3] << 16 | (uint32_t) dec.buf[4] << 24;
				baud_rx(&neg, dec.buf[0], ok_rate, now_ms(), &a);
				if (baud_apply(&a)) {
					memset(&dec, 0, sizeof(dec));
					break; /* the rest of this chunk was sent at the old rate */
				}
			}
		}
		struct baud_act a;
		baud_tick(&neg, now_ms(), &a);
		if (baud_apply(&a))
			memset(&dec, 0, sizeof(dec));
	}
	return NULL;
}

static void *tap_reader(void *arg) {
	(void) arg;
	uint8_t frame[1 + ETH_FRAME_LEN_MAX];
	while (!stop) {
		ssize_t n = read(tap_fd, frame, sizeof(frame));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			logf_("tap read: %s", strerror(errno));
			stop = 1;
			break;
		}
		if (n < 14 || !have_hello)
			continue;
		if (frame[12] != 0x86 || frame[13] != 0xdd)
			continue; /* AWDL carries IPv6 only */
		capture_frame(frame, (size_t) n);
		if (tap_only) {
			logf_("tap-only: %zd-byte IPv6 frame to %02x:%02x:%02x:%02x:%02x:%02x, next header %u", n,
			      frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], n > 20 ? frame[20] : 0);
			continue;
		}
		if (hl_send(HL_ETH, frame, (size_t) n) < 0) {
			logf_("usb write: %s", strerror(errno));
			stop = 1;
		}
	}
	return NULL;
}

static void on_signal(int sig) {
	(void) sig;
	stop = 1;
}

int main(int argc, char **argv) {
	int reset = 1;
	const char *cs = getenv("AIRDROP_CHANSEQ");
	if (cs && *cs) {
		if (!strcmp(cs, "verbatim"))
			radio_chanseq = HL_CHANSEQ_VERBATIM;
		else if (!strcmp(cs, "intersect"))
			radio_chanseq = HL_CHANSEQ_INTERSECT;
		else if (!strcmp(cs, "fill"))
			radio_chanseq = HL_CHANSEQ_FILL;
		else {
			fprintf(stderr, "AIRDROP_CHANSEQ must be verbatim, intersect or fill, got '%s'\n", cs);
			return 2;
		}
	}
	if (baud_parse(getenv("AIRDROP_BAUD"), baud_user, &baud_user_n) < 0) {
		fprintf(stderr, "AIRDROP_BAUD must be 0, %d or a comma list of rates in %d..%d, got '%s'\n", HOSTLINK_BAUD,
		        HOSTLINK_BAUD + 1, BAUD_MAX_RATE, getenv("AIRDROP_BAUD"));
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc)
			snprintf(awdl_name, sizeof(awdl_name), "%s", argv[++i]);
		else if (!strcmp(argv[i], "-i") && i + 1 < argc)
			snprintf(ifname, sizeof(ifname), "%s", argv[++i]);
		else if (!strcmp(argv[i], "-v") && i + 1 < argc)
			radio_loglevel = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-reset"))
			reset = 0;
		else if (!strcmp(argv[i], "--tap-only"))
			tap_only = 1;
		else {
			fprintf(stderr, "usage: %s [-n NAME] [-i IFNAME] [-v LEVEL 3..8] [--no-reset] [--tap-only]\n", argv[0]);
			return 2;
		}
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	if (capture_open() < 0) {
		perror("AIRDROP_PCAP");
		return 1;
	}

	if (tap_only) {
		static const uint8_t test_mac[6] = {0x26, 0x00, 0x5e, 0xf0, 0x1d, 0x05};
		memcpy(radio_mac, test_mac, 6);
		if (tap_open() < 0 || tap_configure() < 0)
			return 1;
		have_hello = 1;
		pthread_t tt;
		pthread_create(&tt, NULL, tap_reader, NULL);
		while (!stop)
			usleep(200 * 1000);
		run("ip -6 rule del pref 9000 2>/dev/null; ip -6 rule del pref 9001 2>/dev/null; true");
		return 0;
	}
	if (usb_open(reset) < 0 || tap_open() < 0)
		return 1;
	baud_prepare();

	pthread_t tu, tt;
	pthread_create(&tu, NULL, usb_reader, NULL);
	for (int i = 0; i < 50 && !have_hello && !stop; i++) {
		if (i % 5 == 0)
			hl_send(HL_PING, NULL, 0);
		usleep(100 * 1000);
	}
	if (!have_hello) {
		logf_("no hello from the radio; is esp32/awdl-radio flashed?");
		return 1;
	}
	pthread_create(&tt, NULL, tap_reader, NULL);
	while (!stop)
		usleep(200 * 1000);
	logf_("stopping");
	run("ip -6 rule del pref 9000 2>/dev/null; ip -6 rule del pref 9001 2>/dev/null; true");
	return 0;
}
