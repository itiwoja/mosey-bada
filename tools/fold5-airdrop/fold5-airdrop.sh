#!/system/bin/sh
# Fold5 AirDrop receiver control. Runs ON THE PHONE as root (KernelSU su).
#
#   sh fold5-airdrop.sh install     # unpack rootfs.tar.gz pushed next to this script
#   sh fold5-airdrop.sh start       # receive for $AIRDROP_MINUTES (default 10) minutes
#   sh fold5-airdrop.sh stop
#   sh fold5-airdrop.sh status
#
# Needs the ESP32 (esp32/awdl-radio) plugged into the phone's USB-C port.
# Received photos go to /storage/emulated/0/Pictures/AirDrop (visible in Gallery).
#
# Settings (environment):
#   AIRDROP_NAME     name shown on the iPhone          (default: Fold5)
#   AIRDROP_MINUTES  stop automatically after this     (default: 10, 0 = never)
#   AIRDROP_RADIO_LOG radio log level 3..8              (default: 6)
#   AIRDROP_CHANSEQ  how the ESP32 declares its channel sequence to the iPhone:
#                    intersect = only the slots we can serve, verbatim = copy the iPhone's order,
#                    fill = intersect plus ch 6 in every slot the iPhone leaves empty (experimental)
#                    (default: intersect -- the iPhone then moves slots to ch 6; anything else aborts)
#   AIRDROP_BAUD     UART speeds to try after the first hello, fastest first, comma separated
#                    (default: 2000000,1500000,1000000). Any failure falls back to 921600.
#                    921600 or 0 = do not negotiate; anything else aborts
#   AIRDROP_HEIC     1 = advertise HEIF so the iPhone sends HEIC as is (smaller files),
#                    0 = old behaviour, iPhone converts to JPEG (default: 1)
#   AIRDROP_TAP_ONLY=1 diagnostics without the ESP32: awdl0 + opendrop only

BASE=${AIRDROP_BASE:-/data/local/airdrop}
ROOTFS=$BASE/rootfs
DEST=/storage/emulated/0/Pictures/AirDrop
NAME=${AIRDROP_NAME:-Fold5}
MINUTES=${AIRDROP_MINUTES:-10}
RADIO_LOG=${AIRDROP_RADIO_LOG:-6}
CHANSEQ=${AIRDROP_CHANSEQ:-intersect}
BAUD=${AIRDROP_BAUD:-2000000,1500000,1000000}
STAGING=/var/airdrop/staging   # inside the rootfs
OUT=mediaprovider:Pictures/AirDrop   # tidy.py resolves this through MediaProvider's namespace

die() { echo "fold5-airdrop: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (su)"

is_mounted() { grep -q " $1 " /proc/mounts; }

pid_alive() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }

kill_pidfile() {
	if [ -f "$1" ]; then
		kill "$(cat "$1")" 2>/dev/null
		rm -f "$1"
	fi
}

do_install() {
	here=$(dirname "$0")
	[ -f "$here/rootfs.tar.gz" ] || die "rootfs.tar.gz not found next to $0"
	mkdir -p "$ROOTFS" || die "cannot create $ROOTFS"
	tar -xzf "$here/rootfs.tar.gz" -C "$ROOTFS" || die "unpack failed"
	chroot "$ROOTFS" /bin/busybox --install -s 2>/dev/null
	mkdir -p "$ROOTFS$STAGING" "$ROOTFS/root" "$ROOTFS/opt"
	cp "$here/tidy.py" "$ROOTFS/opt/tidy.py"
	cp "$here/awdl_bridge" "$BASE/awdl_bridge" && chmod 755 "$BASE/awdl_bridge"
	chmod 755 "$ROOTFS/opt/tidy.py"
	# Android has no /etc/hosts entry for localhost inside the chroot otherwise.
	[ -f "$ROOTFS/etc/hosts" ] || echo "127.0.0.1 localhost" > "$ROOTFS/etc/hosts"
	echo "installed to $BASE"
}

mount_chroot() {
	is_mounted "$ROOTFS/proc" || mount -t proc proc "$ROOTFS/proc"
	is_mounted "$ROOTFS/sys" || mount -t sysfs sysfs "$ROOTFS/sys"
	is_mounted "$ROOTFS/dev" || mount --bind /dev "$ROOTFS/dev"
}

umount_chroot() {
	for m in "$ROOTFS/dev" "$ROOTFS/sys" "$ROOTFS/proc"; do
		is_mounted "$m" && umount "$m"
	done
}

in_chroot() {
	chroot "$ROOTFS" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root \
		OPENDROP_ANNOUNCE_INTERVAL=5 PYTHONUNBUFFERED=1 \
		AIRDROP_STALL_TIMEOUT="${AIRDROP_STALL_TIMEOUT:-120}" \
		AIRDROP_STALL_FLOOR="${AIRDROP_STALL_FLOOR:-30}" \
		AIRDROP_HEIC="${AIRDROP_HEIC:-1}" "$@"
}

do_start() {
	[ -x "$BASE/awdl_bridge" ] && [ -d "$ROOTFS/usr" ] || die "not installed; run install first"
	pid_alive "$BASE/bridge.pid" && die "already running"
	mount_chroot

	if [ "${AIRDROP_TAP_ONLY:-0}" = 1 ]; then
		"$BASE/awdl_bridge" --tap-only > "$BASE/bridge.log" 2>&1 &
	else
		AIRDROP_CHANSEQ="$CHANSEQ" AIRDROP_BAUD="$BAUD" "$BASE/awdl_bridge" -n "$NAME" -v "$RADIO_LOG" > "$BASE/bridge.log" 2>&1 &
	fi
	echo $! > "$BASE/bridge.pid"

	i=0
	until ip -6 addr show dev awdl0 2>/dev/null | grep -q "inet6 fe80"; do
		i=$((i + 1))
		if [ $i -gt 30 ] || ! pid_alive "$BASE/bridge.pid"; then
			tail -n 20 "$BASE/bridge.log" >&2
			do_stop
			die "awdl0 did not come up (see $BASE/bridge.log)"
		fi
		sleep 0.5
	done

	# OpenDrop announces gethostname() as the mDNS SRV target; Android's is
	# "localhost". Give opendrop its own UTS namespace with a DNS-safe name.
	host=$(echo "$NAME" | tr -c 'A-Za-z0-9-\n' '-')
	in_chroot unshare -u sh -c "hostname '$host' && cd $STAGING && exec python3 -m opendrop -n '$NAME' -d -i awdl0 receive" \
		> "$BASE/opendrop.log" 2>&1 &
	echo $! > "$BASE/opendrop.pid"
	in_chroot python3 /opt/tidy.py "$STAGING" "$OUT" --watch 3 > "$BASE/tidy.log" 2>&1 &
	echo $! > "$BASE/tidy.pid"

	if [ "$MINUTES" -gt 0 ] 2>/dev/null; then
		( sleep $((MINUTES * 60)); sh "$0" stop >/dev/null 2>&1 ) &
		echo $! > "$BASE/timer.pid"
	fi
	echo "receiving as \"$NAME\" for ${MINUTES} min. On the iPhone: AirDrop -> Everyone for 10 Minutes, then Share -> AirDrop."
	echo "photos -> $DEST ; logs: $BASE/{bridge,opendrop,tidy}.log"
}

do_stop() {
	kill_pidfile "$BASE/timer.pid"
	kill_pidfile "$BASE/opendrop.pid"
	kill_pidfile "$BASE/tidy.pid"
	kill_pidfile "$BASE/bridge.pid"
	# opendrop runs under sh/chroot; make sure the python child goes too
	for p in $(pgrep -f "python3 -m opendrop" 2>/dev/null) $(pgrep -f "/opt/tidy.py" 2>/dev/null); do
		kill "$p" 2>/dev/null
	done
	sleep 1
	# one last pass so nothing already received is left in staging
	[ -d "$ROOTFS/usr" ] && is_mounted "$ROOTFS/proc" && in_chroot python3 /opt/tidy.py "$STAGING" "$OUT" >> "$BASE/tidy.log" 2>&1
	umount_chroot
	echo "stopped"
}

do_status() {
	for p in bridge opendrop tidy; do
		if pid_alive "$BASE/$p.pid"; then echo "$p: running"; else echo "$p: stopped"; fi
	done
	ip -6 addr show dev awdl0 2>/dev/null | grep inet6
	grep "stats:" "$BASE/bridge.log" 2>/dev/null | tail -n 1
	grep "received:" "$BASE/tidy.log" 2>/dev/null | tail -n 5
}

case "$1" in
	install) do_install ;;
	start) do_start ;;
	stop) do_stop ;;
	status) do_status ;;
	*) echo "usage: $0 install|start|stop|status" >&2; exit 2 ;;
esac
