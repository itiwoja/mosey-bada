#!/bin/bash
# Flash esp32/awdl-radio from WSL through the Windows esptool (USB ports live on Windows).
#   esp32/awdl-radio/flash-windows.sh [COM3]     # port is auto-detected if omitted
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ESPTOOL=${ESPTOOL:-/mnt/c/Users/1kkim/esptool/esptool-windows-amd64/esptool.exe}
STAGE=/mnt/c/Users/1kkim/esptool/awdl-radio
[ -f "$HERE/build/awdl-radio.bin" ] || { echo "build first: idf.py build"; exit 1; }
mkdir -p "$STAGE"
cp "$HERE/build/bootloader/bootloader.bin" "$HERE/build/partition_table/partition-table.bin" \
	"$HERE/build/awdl-radio.bin" "$STAGE/"
PORT=()
[ $# -ge 1 ] && PORT=(-p "$1")
cd "$STAGE"
"$ESPTOOL" --chip esp32 "${PORT[@]}" -b 460800 write-flash \
	0x1000 bootloader.bin 0x8000 partition-table.bin 0x10000 awdl-radio.bin
