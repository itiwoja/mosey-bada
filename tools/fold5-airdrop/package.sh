#!/bin/bash
# Build everything the phone needs into dist/fold5-airdrop/ and (optionally) push it.
#
#   tools/fold5-airdrop/package.sh            # build only
#   tools/fold5-airdrop/package.sh --push     # build, then adb push to /data/local/tmp/fold5-airdrop
#
# Builds an Alpine 3.21 aarch64 rootfs with Python and OpenDrop 0.13.0 plus the
# airdrop-mt7921 iOS 26 patch series, and the awdl_bridge binary (Android NDK).
# Alpine packages are signature-checked against alpine-keys (fetched over HTTPS).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NDK=${NDK:-/mnt/c/Users/1kkim/AppData/Local/Android/Sdk/ndk/30.0.16248370}
ADB=${ADB:-/mnt/c/Users/1kkim/AppData/Local/Android/Sdk/platform-tools/adb.exe}
# Set ANDROID_SERIAL when more than one device/emulator is attached (passed as -s:
# adb.exe does not see WSL environment variables).
ADB_ARGS=()
[ -n "${ANDROID_SERIAL:-}" ] && ADB_ARGS=(-s "$ANDROID_SERIAL")
PY=${PY:-python3}
ALPINE=${ALPINE:-v3.21}
MIRROR=https://dl-cdn.alpinelinux.org/alpine/$ALPINE
WORK=${WORK:-$ROOT/work/fold5-airdrop-build}
DIST=$ROOT/dist/fold5-airdrop
PATCHES="ios26-airdrop recv-window py314-send mdns-repeat find-report tls-keylog upload-arms
ask-confirm mdns-reannounce threaded-server url-items zeroconf-update-service salvage-truncated
salvage-trim send-multifile send-status send-stall"

mkdir -p "$WORK" "$DIST"
cd "$WORK"

# apk.static and the Alpine signing keys
if [ ! -x sbin/apk.static ]; then
	f=$(curl -fsS "$MIRROR/main/x86_64/" | grep -o 'apk-tools-static-[0-9a-z._-]*\.apk' | head -1)
	curl -fsSO "$MIRROR/main/x86_64/$f" && tar xzf "$f" 2>/dev/null
fi
if [ ! -d keys/etc/apk/keys ]; then
	f=$(curl -fsS "$MIRROR/main/x86_64/" | grep -o 'alpine-keys-[0-9a-z._-]*\.apk' | head -1)
	mkdir -p keys && curl -fsSO "$MIRROR/main/x86_64/$f" && tar xzf "$f" -C keys 2>/dev/null
fi

R=$WORK/rootfs
rm -rf "$R"
mkdir -p "$R/etc/apk/keys"
cp keys/etc/apk/keys/* "$R/etc/apk/keys/"
cp keys/usr/share/apk/keys/aarch64/* "$R/etc/apk/keys/" 2>/dev/null || true
# --no-scripts: we cannot run aarch64 scripts here; install does busybox --install on the phone.
# Ownership errors are expected when not root; the phone extracts as root.
./sbin/apk.static --arch aarch64 -X "$MIRROR/main" -X "$MIRROR/community" -U \
	--keys-dir "$R/etc/apk/keys" --root "$R" --initdb --no-scripts \
	add alpine-baselayout alpine-keys busybox musl python3 py3-pillow py3-requests py3-zeroconf \
	py3-ifaddr py3-setuptools libarchive openssl ca-certificates-bundle 2>&1 |
	grep -vE "Installing|updating directory permissions" || true

SP=$(echo "$R"/usr/lib/python3.*/site-packages)
"$PY" -m pip install -q --no-deps --no-compile --target "$SP" \
	opendrop==0.13.0 fleep==1.0.1 libarchive-c==5.3 requests-toolbelt==1.0.0
(
	cd "$SP"
	for p in $PATCHES; do
		GIT_DIR=/nonexistent git apply "$ROOT/third_party/airdrop-mt7921/patches/opendrop-$p.patch"
	done
	GIT_DIR=/nonexistent git apply "$ROOT/tools/fold5-airdrop/opendrop-esp32-stream.patch"
	GIT_DIR=/nonexistent git apply "$ROOT/tools/fold5-airdrop/opendrop-heic.patch"
	GIT_DIR=/nonexistent git apply "$ROOT/tools/fold5-airdrop/opendrop-early-finish.patch"
)
echo "opendrop patched ($(echo $PATCHES | wc -w) patches)"

tar --numeric-owner --owner=0 --group=0 -czf "$DIST/rootfs.tar.gz" -C "$R" .
"$PY" "$ROOT/tools/build-radio-probe.py" --ndk "$NDK" --tool awdl-bridge --output "$DIST/awdl_bridge"
cp "$ROOT/tools/fold5-airdrop/fold5-airdrop.sh" "$ROOT/tools/fold5-airdrop/tidy.py" "$DIST/"
ls -la "$DIST"

if [ "${1:-}" = "--push" ]; then
	"$ADB" "${ADB_ARGS[@]}" shell mkdir -p /data/local/tmp/fold5-airdrop
	for f in rootfs.tar.gz awdl_bridge fold5-airdrop.sh tidy.py; do
		"$ADB" "${ADB_ARGS[@]}" push "$(wslpath -w "$DIST/$f" 2>/dev/null || echo "$DIST/$f")" /data/local/tmp/fold5-airdrop/
	done
	echo "pushed. On the phone (root): sh /data/local/tmp/fold5-airdrop/fold5-airdrop.sh install"
fi
