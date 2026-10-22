#!/system/bin/sh
# Temporary system-app overlay only. No radio, firmware, or SELinux policy changes.
# Run as root inside init's mount namespace. Full reboot removes all mounts.
set -eu
BASE=/mnt/fold5-mosey-runtime
INPUT=/data/local/tmp/fold5-mosey
check_root() {
  [ "$(id -u)" = 0 ] || exit 10
  [ "$(getprop ro.product.model)" = SM-F946U1 ] || exit 11
  [ "$(readlink /proc/self/ns/mnt)" = "$(readlink /proc/1/ns/mnt)" ] || exit 12
}
rollback() {
  if grep -q " $BASE " /proc/self/mounts; then
    # Only remove target mounts if our staged Mosey directory is visible.
    if [ -d /product/priv-app/Fold5Mosey ]; then
      umount /product/etc/permissions || return 1
      # Recursive bind creates a second mount for every preserved app.
      for d in /product/priv-app/*; do
        [ "${d##*/}" = Fold5Mosey ] && continue
        if grep -Fq " $d " /proc/self/mounts; then
          umount "$d" || return 1
        fi
      done
      umount /product/priv-app || return 1
    fi
    for d in "$BASE"/priv-app/*; do
      [ "${d##*/}" = Fold5Mosey ] && continue
      umount "$d" || return 1
    done
    umount "$BASE"
    rmdir "$BASE"
  fi
}
check_root
case "${1:-}" in
  rollback) rollback; exit 0 ;;
  apply) ;;
  *) echo "Usage: $0 apply|rollback"; exit 2 ;;
esac
[ ! -e /product/priv-app/Fold5Mosey ] || exit 13
[ ! -e "$BASE" ] || exit 14
for a in base split_config.arm64_v8a split_config.en split_config.xxhdpi; do
  [ -s "$INPUT/$a.apk" ] || exit 15
done
[ -s "$INPUT/privapp-permissions-fold5-mosey.xml" ] || exit 16
mkdir "$BASE"
mount -t tmpfs -o mode=0755 tmpfs "$BASE"
# Preserve every existing application by bind mount; no APK copy/replacement.
mkdir "$BASE/priv-app" "$BASE/permissions"
chcon u:object_r:system_file:s0 "$BASE" "$BASE/priv-app" "$BASE/permissions"
for d in /product/priv-app/*; do
  [ -d "$d" ] || exit 17
  mkdir "$BASE/priv-app/${d##*/}"
  mount -o bind "$d" "$BASE/priv-app/${d##*/}"
done
cp -a /product/etc/permissions/. "$BASE/permissions/"
mkdir "$BASE/priv-app/Fold5Mosey"
cp "$INPUT/"*.apk "$BASE/priv-app/Fold5Mosey/"
cp "$INPUT/privapp-permissions-fold5-mosey.xml" "$BASE/permissions/"
chown -R 0:0 "$BASE/priv-app/Fold5Mosey"
chcon -R u:object_r:system_file:s0 "$BASE/priv-app/Fold5Mosey" "$BASE/permissions"
chmod 0755 "$BASE/priv-app/Fold5Mosey"
chmod 0644 "$BASE/priv-app/Fold5Mosey/"*.apk "$BASE/permissions/privapp-permissions-fold5-mosey.xml"
mount -o bind "$BASE/permissions" /product/etc/permissions
if ! mount -o rbind "$BASE/priv-app" /product/priv-app; then
  umount /product/etc/permissions
  exit 18
fi
# A plain bind would hide all nested original-app mounts behind empty dirs.
# Verify every preserved APK before any framework reload is allowed.
count=0
for apk in "$BASE"/priv-app/*/*.apk; do
  [ -f "$apk" ] || continue
  relative="${apk#"$BASE/priv-app/"}"
  if [ ! -f "/product/priv-app/$relative" ]; then
    echo "preserved_apk_missing=$relative"
    rollback
    exit 19
  fi
  count=$((count + 1))
done
[ "$count" -gt 1 ] || { rollback; exit 20; }
echo "visible_apks_verified=$count"
echo runtime_system_app_overlay_ready
# Framework restart is deliberately a separate operation after inspection.
