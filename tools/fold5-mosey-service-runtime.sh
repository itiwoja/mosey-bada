#!/system/bin/sh
# Temporary VINTF declaration and service-specific SELinux mapping.
# Run as root in init's mount namespace. Full reboot resets mounts and policy.
set -eu
BASE=/mnt/fold5-mosey-service
SRC=/data/local/tmp/fold5-mosey-native
[ "$(id -u)" = 0 ] || exit 10
[ "$(getprop ro.product.model)" = SM-F946U1 ] || exit 11
[ "$(readlink /proc/self/ns/mnt)" = "$(readlink /proc/1/ns/mnt)" ] || exit 12
case "${1:-}" in
rollback)
  [ -e "$BASE/manifest/manifest_fold5_mosey.xml" ] || exit 0
  umount /apex/apex-info-list.xml
  umount /vendor/etc/vintf/manifest
  umount /vendor/etc/selinux/vendor_service_contexts
  umount "$BASE"
  rmdir "$BASE"
  /data/adb/ksud sepolicy patch 'allow shell fold5_mosey_service service_manager find;'
  echo mounts_removed_policy_resets_on_full_reboot
  exit 0;;
apply) ;;
*) echo "Usage: $0 apply|rollback"; exit 2;;
esac
[ ! -e "$BASE" ] || exit 13
[ -s "$SRC/manifest_mosey.xml" ] || exit 14
# Limit the new service mapping to the root daemon and privileged callers.
/data/adb/ksud sepolicy patch 'type fold5_mosey_service service_manager_type; allow ksu fold5_mosey_service service_manager { add find }; allow priv_app fold5_mosey_service service_manager find; allow shell fold5_mosey_service service_manager find;'
mkdir "$BASE"
mount -t tmpfs -o mode=0755 tmpfs "$BASE"
mkdir "$BASE/manifest"
cp -a /vendor/etc/vintf/manifest/. "$BASE/manifest/"
cp "$SRC/manifest_mosey.xml" "$BASE/manifest/manifest_fold5_mosey.xml"
chcon -R u:object_r:vendor_configs_file:s0 "$BASE/manifest"
cp -a /vendor/etc/selinux/vendor_service_contexts "$BASE/vendor_service_contexts"
printf '\ncom.google.android.moseyservice.IMoseyService/default u:object_r:fold5_mosey_service:s0\n' >> "$BASE/vendor_service_contexts"
chcon u:object_r:vendor_service_contexts_file:s0 "$BASE/vendor_service_contexts"
chmod 0644 "$BASE/manifest/manifest_fold5_mosey.xml" "$BASE/vendor_service_contexts"
mount -o bind "$BASE/vendor_service_contexts" /vendor/etc/selinux/vendor_service_contexts
if ! mount -o bind "$BASE/manifest" /vendor/etc/vintf/manifest; then
  umount /vendor/etc/selinux/vendor_service_contexts
  exit 15
fi
# Android 16 libvintf invalidates the HAL-manifest cache when this file's
# timestamp changes. Preserve its bytes and labels, overlay only the copy.
cp -a /apex/apex-info-list.xml "$BASE/apex-info-list.xml"
chcon u:object_r:apex_info_file:s0 "$BASE/apex-info-list.xml"
touch "$BASE/apex-info-list.xml"
cmp /apex/apex-info-list.xml "$BASE/apex-info-list.xml"
mount -o bind "$BASE/apex-info-list.xml" /apex/apex-info-list.xml
# Access.cpp reloads service_contexts on a policy-load status update.
/data/adb/ksud sepolicy patch 'allow shell fold5_mosey_service service_manager find;'
echo service_declaration_ready_without_service_restart
