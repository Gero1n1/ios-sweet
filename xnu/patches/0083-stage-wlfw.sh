#!/bin/bash
# Stage the repository's WLAN protocol/transport sources before patching XNU.
set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/sweet-wcn3990"
target=bsd/dev/sweet_wcn3990
[[ -f bsd/conf/files ]] || { echo 'Not an XNU source root' >&2; exit 1; }
[[ ! -e "$target" ]] || { echo "Refusing to overwrite $target" >&2; exit 1; }
sources=(wlfw_wire.c wlfw_wire.h glink_smem.c glink_smem.h smem_view.c smem_view.h glink_control.c glink_control.h)
for source in "${sources[@]}"; do
    [[ -f "$source_dir/$source" ]] || {
        echo "Missing canonical WLAN source: $source" >&2
        exit 1
    }
done
mkdir -p "$target"
for source in "${sources[@]}"; do
    cp "$source_dir/$source" "$target/"
done
echo 'Staged WLFW and GLINK FIFO transport; platform binding/radio startup are not implemented'
