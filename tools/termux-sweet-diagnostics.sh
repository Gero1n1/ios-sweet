#!/data/data/com.termux/files/usr/bin/sh
# Read-only Android inventory for ios-sweet hardware bring-up.
# Run from Termux or through SSH. Does not read firmware contents or write device data.

set -u

section() { printf '\n=== %s ===\n' "$1"; }
redact() { sed -E 's/([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}/<MAC>/g'; }

section 'Android and kernel'
for key in ro.product.device ro.product.vendor.device ro.board.platform ro.soc.model \
           ro.build.version.release ro.build.version.security_patch ro.build.fingerprint; do
    printf '%s=' "$key"
    getprop "$key" 2>/dev/null | head -n 1
done
uname -a

section 'CPU topology and memory'
for f in /sys/devices/system/cpu/possible /sys/devices/system/cpu/present \
         /sys/devices/system/cpu/online; do
    [ ! -r "$f" ] || printf '%s: %s\n' "$f" "$(cat "$f")"
done
grep -E '^(MemTotal|MemAvailable|CmaTotal|CmaFree):' /proc/meminfo 2>/dev/null

section 'WLAN interfaces and bound driver'
ip -brief link 2>/dev/null | redact || true
for n in wlan0 wlan1 p2p0; do
    p="/sys/class/net/$n"
    [ -e "$p" ] || continue
    printf '%s driver=' "$n"
    readlink -f "$p/device/driver" 2>/dev/null || echo unavailable
    printf '%s device=' "$n"
    readlink -f "$p/device" 2>/dev/null || echo unavailable
done
if command -v iw >/dev/null 2>&1; then
    echo '-- iw dev --'
    iw dev 2>&1 | redact
    echo '-- iw phy band/frequency summary --'
    iw phy 2>&1 | grep -E 'Wiphy|Band [0-9]| MHz \[' | head -n 100
else
    echo 'iw is not installed; no package installation attempted.'
fi

section 'WLAN firmware and board-data filenames'
for d in /vendor/firmware /vendor/firmware_mnt /odm/firmware /system/vendor/firmware; do
    [ -d "$d" ] || continue
    find "$d" -maxdepth 4 -type f \( -iname '*wlan*' -o -iname '*wcnss*' \
        -o -iname 'bdwlan*' -o -iname '*icnss*' \) -print 2>/dev/null | head -n 160
done

section 'ICNSS and remote-service kernel messages (filtered, MACs redacted)'
if command -v su >/dev/null 2>&1; then
    su -c 'dmesg 2>/dev/null' 2>&1 | grep -Ei 'icnss|cnss|ath10k|wcnss|wlfw|qrtr|qmi|firmware.*(wlan|wcn)' \
        | tail -n 180 | redact || echo 'Root dmesg unavailable or no matching messages.'
else
    echo 'su is unavailable; root-only kernel messages skipped.'
fi

section 'Device-tree ICNSS and WLAN memory nodes'
for root in /proc/device-tree /sys/firmware/devicetree/base; do
    [ -d "$root" ] || continue
    find "$root" -maxdepth 7 -type d \( -iname '*icnss*' -o -iname '*wlan*msa*' \
        -o -iname '*wcn3990*' \) -print 2>/dev/null | head -n 80
done

section 'Notes'
echo 'This is the currently running Android kernel/driver inventory, not ios-sweet behavior.'
echo 'No firmware blobs, calibration values, serial numbers, or raw device-tree dump were collected.'
