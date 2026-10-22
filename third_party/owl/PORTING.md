# Local changes to OWL for the ESP32 port

- `src/rx.c`, `src/rx.h`: `awdl_rx_timestamp` lets the radio pass the time a frame
  was received, instead of the time it was processed from a queue.
- `src/schedule.c`: with `AWDL_FIXED_RADIO_CHANNEL` defined, unicast and multicast
  are only sent in slots whose channel is that fixed channel.
- `radiotap/` is radiotap-library (upstream submodule), vendored.

The daemon (`daemon/`) is unchanged and unused by the ESP32 build.
- `src/wire.c`, `src/tx.c`, `src/rx.c`: unaligned integer loads/stores replaced with memcpy (Xtensa faults on them).
- `radiotap/radiotap.c`: `1u << IEEE80211_RADIOTAP_EXT` (signed shift into bit 31 is undefined).
- `src/rx.c`: received data frames were emitted at the 1518-byte allocation size
  with trailing garbage; now trimmed to the real length. Payloads too large for an
  Ethernet frame are rejected, and A-MSDU subframes are capped at
  `AWDL_RX_MAX_FRAMES` (callers' output arrays are that size; more overflowed them).
- `src/election.c`: free the peer iterator in `awdl_election_run()` (leaked 16 bytes per run, i.e. every second).
