#!/bin/sh
# Host build + run of tests/awdl_sim_test.c against the ESP32 firmware's OWL configuration.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OWL=$ROOT/third_party/owl
OUT=${OUT:-${TMPDIR:-/tmp}/awdl_sim_test}
cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wno-unused-function \
	-DAWDL_FIXED_RADIO_CHANNEL=6 \
	-I"$ROOT/esp32/awdl-radio/main" -I"$OWL/src" -I"$OWL/radiotap" \
	"$ROOT/tests/awdl_sim_test.c" \
	"$OWL"/src/channel.c "$OWL"/src/circular_buffer.c "$OWL"/src/crc32.c "$OWL"/src/election.c \
	"$OWL"/src/frame.c "$OWL"/src/hashmap.c "$OWL"/src/peers.c "$OWL"/src/rx.c "$OWL"/src/schedule.c \
	"$OWL"/src/siphash24.c "$OWL"/src/state.c "$OWL"/src/sync.c "$OWL"/src/tx.c "$OWL"/src/version.c \
	"$OWL"/src/wire.c "$OWL"/src/log.c "$OWL"/radiotap/radiotap.c \
	-o "$OUT"
"$OUT"
