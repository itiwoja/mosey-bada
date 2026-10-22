#!/system/bin/sh
# Run as root, detached from adb, before swapping the PC cable for the ESP32.
CTRL=/data/local/tmp/fold5-airdrop/fold5-airdrop.sh
echo "Waiting up to 5 minutes for ESP32 USB-UART"
i=0
while [ "$i" -lt 150 ]; do
    for d in /sys/bus/usb/devices/*; do
        [ -r "$d/idVendor" ] && [ -r "$d/idProduct" ] || continue
        vid=$(cat "$d/idVendor")
        pid=$(cat "$d/idProduct")
        case "$vid:$pid" in
            10c4:ea60|1a86:7523|1a86:5523)
                echo "ESP32 USB-UART detected: $vid:$pid"
                sleep 2
                exec sh "$CTRL" start
                ;;
        esac
    done
    sleep 2
    i=$((i + 1))
done
echo "Timed out waiting for ESP32"
exit 1
