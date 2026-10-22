#!/bin/bash
# Stage canonical A618 firmware primitives. Never accesses a device.
set -euo pipefail
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/sweet-adreno618"
target=bsd/dev/sweet_adreno618
[[ -f bsd/conf/files ]] || { echo 'Not an XNU source root' >&2; exit 1; }
[[ ! -e "$target" ]] || { echo "Refusing to overwrite $target" >&2; exit 1; }
[[ -f "$source_dir/firmware.c" && -f "$source_dir/firmware.h" ]] || {
    echo 'Missing canonical A618 firmware sources' >&2
    exit 1
}
mkdir -p "$target"
cp "$source_dir/firmware.c" "$source_dir/firmware.h" "$target/"
echo 'Staged A618 firmware primitives; no GPU startup caller or device access'
