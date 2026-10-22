#!/system/bin/sh
# Read-only probe. No su, writes, service changes or radio reconfiguration.
printf '%s\n' 'FOLD5 PORT PROBE v1 (read-only)'
for key in ro.product.model ro.product.device ro.product.manufacturer ro.product.cpu.abi ro.build.version.release ro.build.version.sdk ro.build.version.oneui ro.build.version.security_patch ro.boot.bootloader ro.boot.flash.locked ro.boot.vbmeta.device_state ro.boot.verifiedbootstate ro.oem_unlock_supported sys.oem_unlock_allowed; do
  printf '%s=%s\n' "$key" "$(getprop "$key")"
done
printf '\n%s\n' '== Kernel =='
uname -r
printf '\n%s\n' '== Existing root command (not executed) =='
command -v su || true
printf '\n%s\n' '== Loaded modules =='
cat /proc/modules 2>/dev/null || true
printf '\n%s\n' '== Wi-Fi features =='
pm list features 2>/dev/null | grep -E 'wifi|bluetooth' || true
printf '\n%s\n' '== Candidate drivers and services (names only) =='
for folder in /vendor/lib/modules /vendor_dlkm/lib/modules /odm/lib/modules /system_dlkm/lib/modules /vendor/bin /odm/bin /vendor/lib64 /odm/lib64; do
  printf '%s\n' "$folder"
  ls "$folder" 2>/dev/null | grep -iE 'wlan|wifi|wonder|mosey|bcm|qca|cnss|lowi' || true
done
printf '\n%s\n' '== Kernel configuration availability =='
if [ -r /proc/config.gz ]; then
  zcat /proc/config.gz 2>/dev/null | grep -E '^CONFIG_(MODULES|MODVERSIONS|CFG80211|MAC80211|TUN|KSU)=' || true
else
  printf '%s\n' 'Kernel config inaccessible; this is unknown, not unsupported.'
fi
printf '\n%s\n' 'END: This inventory does not prove AWDL compatibility or root availability.'
