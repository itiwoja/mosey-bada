/* Minimal <net/ethernet.h> for building OWL on ESP-IDF. */
#pragma once
#include <stdint.h>
#define ETHER_ADDR_LEN 6
#define ETH_ALEN 6
#define ETHER_HDR_LEN 14
#define ETHER_MAX_LEN 1518
struct ether_addr {
	uint8_t ether_addr_octet[ETHER_ADDR_LEN];
} __attribute__((__packed__));
