#!/bin/bash
# Stage the repository's portable codec before DXB applies the patch series.
set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/sweet-wcn3990"
target=bsd/dev/sweet_wcn3990
[[ -f bsd/conf/files ]] || { echo 'Not an XNU source root' >&2; exit 1; }
[[ ! -e "$target" ]] || { echo "Refusing to overwrite $target" >&2; exit 1; }
[[ -f "$source_dir/wlfw_wire.c" && -f "$source_dir/wlfw_wire.h" ]] || {
    echo 'Missing canonical WLFW codec sources beside the staging script' >&2
    exit 1
}
mkdir -p "$target"
cp "$source_dir/wlfw_wire.c" "$source_dir/wlfw_wire.h" "$target/"
echo 'Staged WLFW wire codec; no transport or hardware initialization is implemented'
