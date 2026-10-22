#!/usr/bin/env python3
"""Read a single flattened device tree and plan SWEET's contiguous upper RAM.

XNU's current SWEET boot path accepts one physical range. Never extend it
across a hole or a Qualcomm reserved pool merely to report more RAM.
"""
from dataclasses import dataclass, field
import struct


U64_END = 1 << 64
FOUR_GIB = 1 << 32
BLOCK = 0x2000000              # start.s uses 32 MiB blocks with 16 KiB pages
PAGE = 0x4000


@dataclass
class Node:
    path: str
    props: dict = field(default_factory=dict)

    @property
    def enabled(self):
        return self.props.get('status', b'okay\0') in (b'okay\0', b'ok\0')

    def cells(self, key, default):
        value = self.props.get(key)
        if value is None:
            return default
        if len(value) != 4:
            raise ValueError(f'{self.path}: malformed {key}')
        count, = struct.unpack('>I', value)
        if count not in (1, 2):
            raise ValueError(f'{self.path}: unsupported {key}={count}')
        return count


def _range(base, size, source):
    if size == 0 or base < 0 or base + size > U64_END:
        raise ValueError(f'{source}: invalid physical range')
    return base, base + size


def ranges(data, address_cells, size_cells, source):
    width = 4 * (address_cells + size_cells)
    if not data or len(data) % width:
        raise ValueError(f'{source}: malformed address/size tuples')
    return [_range(int.from_bytes(data[i:i + 4 * address_cells], 'big'),
                   int.from_bytes(data[i + 4 * address_cells:i + width], 'big'), source)
            for i in range(0, len(data), width)]


def parse(blob):
    """Return ({absolute_path: Node}, memreserve_ranges); reject malformed DTBs."""
    if len(blob) < 40:
        raise ValueError('truncated FDT header')
    magic, total, off_struct, off_strings, off_reserve, version, compat, _, nstrings, nstruct = struct.unpack_from('>10I', blob)
    if magic != 0xd00dfeed or version < 17 or compat > 17:
        raise ValueError('expected a version-17-compatible FDT')
    if total > len(blob) or total < 40:
        raise ValueError('invalid FDT total size')
    if any(blob[total:]):
        raise ValueError('expected one DTB; concatenated DTBs are not supported')
    blocks = [(off_struct, off_struct + nstruct), (off_strings, off_strings + nstrings)]
    if off_struct % 4 or off_reserve % 8:
        raise ValueError('unaligned FDT block')
    if any(a < 40 or b > total or b < a for a, b in blocks):
        raise ValueError('FDT block outside total size')
    if max(blocks[0][0], blocks[1][0]) < min(blocks[0][1], blocks[1][1]):
        raise ValueError('overlapping FDT structure and strings')
    reservations, pos = [], off_reserve
    if pos < 40:
        raise ValueError('invalid FDT reservation offset')
    while True:
        if pos + 16 > total or any(pos < b and pos + 16 > a for a, b in blocks):
            raise ValueError('unterminated FDT reservation table')
        base, size = struct.unpack_from('>QQ', blob, pos)
        pos += 16
        if base == 0 and size == 0:
            break
        reservations.append(_range(base, size, 'FDT memreserve'))
    strings = blob[off_strings:off_strings + nstrings]
    nodes, stack = {}, []
    pos, end = off_struct, off_struct + nstruct

    def string(data, start, limit):
        stop = data.find(b'\0', start, limit)
        if stop < 0:
            raise ValueError('unterminated FDT string')
        try:
            value = data[start:stop].decode('ascii')
        except UnicodeDecodeError as error:
            raise ValueError('non-ASCII FDT name') from error
        return value, stop + 1

    while pos + 4 <= end:
        token, = struct.unpack_from('>I', blob, pos)
        pos += 4
        if token == 1:  # FDT_BEGIN_NODE
            name, pos = string(blob, pos, end)
            pos = (pos + 3) & ~3
            if '/' in name or (not stack and (name or nodes)) or (stack and not name):
                raise ValueError('invalid FDT node name')
            path = (stack[-1].path.rstrip('/') + '/' + name) if stack else '/'
            if path in nodes:
                raise ValueError(f'duplicate FDT node {path}')
            node = Node(path)
            nodes[path] = node
            stack.append(node)
        elif token == 2:  # FDT_END_NODE
            if not stack:
                raise ValueError('unbalanced FDT_END_NODE')
            stack.pop()
        elif token == 3:  # FDT_PROP
            if not stack or pos + 8 > end:
                raise ValueError('FDT property without a node or header')
            length, nameoff = struct.unpack_from('>II', blob, pos)
            pos += 8
            if pos + length > end or nameoff >= len(strings):
                raise ValueError('FDT property outside block')
            key, _ = string(strings, nameoff, len(strings))
            if not key or key in stack[-1].props:
                raise ValueError('empty or duplicate FDT property')
            stack[-1].props[key] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:  # FDT_NOP
            continue
        elif token == 9:  # FDT_END
            if stack or '/' not in nodes:
                raise ValueError('unbalanced or empty FDT')
            return nodes, reservations
        else:
            raise ValueError(f'unknown FDT token {token}')
    raise ValueError('missing FDT_END')


def subtract(available, excluded):
    for lo, hi in excluded:
        remaining = []
        for start, end in available:
            if hi <= start or lo >= end:
                remaining.append((start, end))
            else:
                if start < lo:
                    remaining.append((start, lo))
                if hi < end:
                    remaining.append((hi, end))
        available = remaining
    return available


def upper_memory_plan(blob):
    """Choose the largest safe contiguous span above 4 GiB, before panic carveout.

    Every static reservation is excluded, including reusable pools. Dynamic
    reserved-memory pools may use their entire alloc-ranges; without such a
    bound no upper DDR range can be proven safe and selection fails.
    """
    nodes, reservations = parse(blob)
    root = nodes['/']
    ac, sc = root.cells('#address-cells', 2), root.cells('#size-cells', 1)
    banks = []
    for node in nodes.values():
        if node.path.count('/') != 1 or not node.enabled:
            continue
        if node.props.get('device_type') != b'memory\0':
            continue
        bank = ranges(node.props.get('reg', b''), ac, sc, node.path)
        if 'linux,usable-memory' in node.props:
            usable = ranges(node.props['linux,usable-memory'], ac, sc, node.path)
            bank = [(max(a, c), min(b, d)) for a, b in bank for c, d in usable
                    if max(a, c) < min(b, d)]
        banks.extend(bank)
    banks.sort()
    if not banks:
        raise ValueError('FDT contains no enabled root memory nodes')
    if any(banks[i][0] < banks[i - 1][1] for i in range(1, len(banks))):
        raise ValueError('overlapping FDT memory banks')
    available = [(max(a, FOUR_GIB), b) for a, b in banks if b > FOUR_GIB]
    chosen = nodes.get('/chosen')
    if chosen and 'linux,usable-memory-range' in chosen.props:
        usable = ranges(chosen.props['linux,usable-memory-range'], ac, sc, '/chosen')
        available = [(max(a, c), min(b, d)) for a, b in available for c, d in usable
                     if max(a, c) < min(b, d)]
    reserved = nodes.get('/reserved-memory')
    if reserved and reserved.enabled:
        if reserved.props.get('ranges', b''):
            raise ValueError('translated reserved-memory ranges are not supported')
        rac = reserved.cells('#address-cells', ac)
        rsc = reserved.cells('#size-cells', sc)
        for node in nodes.values():
            if not node.path.startswith('/reserved-memory/') or node.path.count('/') != 2 or not node.enabled:
                continue
            if 'reg' in node.props:
                reservations.extend(ranges(node.props['reg'], rac, rsc, node.path))
            elif 'size' in node.props:
                if len(node.props['size']) != 4 * rsc or int.from_bytes(node.props['size'], 'big') == 0:
                    raise ValueError(f'{node.path}: invalid dynamic pool size')
                if 'alloc-ranges' not in node.props:
                    raise ValueError(f'{node.path}: dynamic pool has no alloc-ranges; upper RAM is unproven')
                reservations.extend(ranges(node.props['alloc-ranges'], rac, rsc, node.path))
            else:
                raise ValueError(f'{node.path}: reserved-memory child has neither reg nor size')
    available = subtract(available, reservations)
    # Adjacent tuples can describe one physical span, but never join a hole.
    merged = []
    for start, end in sorted(available):
        if merged and merged[-1][1] == start:
            merged[-1] = (merged[-1][0], end)
        else:
            merged.append((start, end))
    aligned = [(((a + BLOCK - 1) // BLOCK) * BLOCK, b // PAGE * PAGE) for a, b in merged]
    aligned = [(a, b) for a, b in aligned if b - a >= 0x10000000]
    if not aligned:
        raise ValueError('no safe contiguous upper RAM span of at least 256 MiB')
    selected = max(aligned, key=lambda span: (span[1] - span[0], -span[0]))
    return dict(banks=banks, excluded=sorted(reservations), available=merged,
                selected=selected, detected_bytes=sum(b - a for a, b in banks),
                selected_bytes=selected[1] - selected[0])
