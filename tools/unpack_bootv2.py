#!/usr/bin/env python3
"""Validate and unpack Android boot header v0-v2 images, without device access.

Layout reference: AOSP system/tools/mkbootimg/include/bootimg/bootimg.h
and unpack_bootimg.py. This does not verify signatures or hardware suitability.
"""
import argparse
from pathlib import Path
import struct
import sys


HEADER_SIZES = (1632, 1648, 1660)
PAGE_SIZES = (2048, 4096, 8192, 16384)


def parse_boot_image(data):
    """Return metadata and bounded component slices; reject before writing files."""
    if len(data) < 48 or data[:8] != b'ANDROID!':
        raise ValueError('missing or truncated Android boot header')
    ks, ka, rs, ra, ss, sa, ta, page, version, osv = struct.unpack_from('<10I', data, 8)
    if version not in (0, 1, 2):
        raise ValueError(f'unsupported boot header version {version}; expected v0-v2')
    header_size = HEADER_SIZES[version]
    if len(data) < header_size:
        raise ValueError('truncated boot header')
    if page not in PAGE_SIZES or page < header_size:
        raise ValueError(f'unsupported boot page size {page}')
    if len(data) < page:
        raise ValueError('truncated header page')
    if not ks:
        raise ValueError('empty kernel')

    def cstr(raw):
        return raw.split(b'\0', 1)[0].decode('utf-8', errors='backslashreplace')

    info = dict(kernel_size=ks, kernel_addr=hex(ka), ramdisk_size=rs,
                ramdisk_addr=hex(ra), second_size=ss, second_addr=hex(sa),
                tags_addr=hex(ta), page_size=page, header_version=version,
                os_version=hex(osv), name=cstr(data[48:64]),
                cmdline=cstr(data[64:576]) + cstr(data[608:1632]))
    components = {}
    spans = [(0, page, 'header')]

    def rounded(size):
        return (size + page - 1) // page * page

    def component(name, offset, size):
        info[name + '_offset'] = offset
        if not size:
            return
        if offset % page:
            raise ValueError(f'{name} offset is not page aligned')
        if offset > len(data) or size > len(data) - offset:
            raise ValueError(f'truncated {name}')
        end = offset + size
        for lo, hi, other in spans:
            if offset < hi and lo < end:
                raise ValueError(f'{name} overlaps {other}')
        spans.append((offset, end, name))
        components[name] = data[offset:end]

    offset = page
    for name, size in [('kernel', ks), ('ramdisk', rs), ('second', ss)]:
        component(name, offset, size)
        offset += rounded(size)
    recovery_size = 0
    if version >= 1:
        recovery_size, recovery_offset, reported_size = struct.unpack_from('<IQI', data, 1632)
        if reported_size != header_size:
            raise ValueError(f'header_size {reported_size} does not match v{version}')
        info.update(header_size=reported_size, recovery_dtbo_size=recovery_size)
        component('recovery_dtbo', recovery_offset, recovery_size)
    if version == 2:
        dtb_size, dtb_addr = struct.unpack_from('<IQ', data, 1648)
        info.update(dtb_size=dtb_size, dtb_addr=hex(dtb_addr))
        # DTB position follows the page counts, as in AOSP unpack_bootimg.
        component('dtb', offset + rounded(recovery_size), dtb_size)
    return info, components


def unpack(src, out):
    info, components = parse_boot_image(Path(src).read_bytes())
    target = Path(out)
    if target.exists() and (not target.is_dir() or any(target.iterdir())):
        raise ValueError('output must be a new or empty directory; refusing stale files')
    target.mkdir(parents=True, exist_ok=True)
    for name, contents in components.items():
        (target / name).write_bytes(contents)
    summary = ''.join(f'{key}: {value}\n' for key, value in info.items())
    (target / 'header.txt').write_text(summary, encoding='utf-8')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('src', help='local Android boot image')
    parser.add_argument('out', help='new or empty output directory')
    args = parser.parse_args()
    try:
        print(unpack(args.src, args.out), end='')
    except (OSError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
