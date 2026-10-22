"""Host regression tests for RAM selection and actual payload metadata bounds.

The fixtures are synthetic DTBs/Mach-O files, never device dumps. No ARM code
is executed; passing these tests says nothing about a successful device boot.
"""
import contextlib
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import adt
import fdt
import mkadt
import mkpayload


def cells(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def reg(*spans):
    return b''.join(struct.pack('>QQ', address, size) for address, size in spans)


def dtb(upper_size=0xfcc00000, reserved=(), memreserve=(), memory_props=None,
        root_cells=(2, 2), memory_reg=None, chosen=()):
    """Encode a version-17 FDT using its documented wire format."""
    names = bytearray()
    offsets = {}

    def encode_node(name, props, children=()):
        out = bytearray(cells(1) + name.encode() + b'\0')
        out += b'\0' * (-len(out) % 4)
        for key, value in props:
            if key not in offsets:
                offsets[key] = len(names)
                names.extend(key.encode() + b'\0')
            out += cells(3, len(value), offsets[key]) + value
            out += b'\0' * (-len(out) % 4)
        for child in children:
            out += encode_node(*child)
        return bytes(out + cells(2))

    children = [('memory@80000000', [
        ('device_type', b'memory\0'),
        ('reg', memory_reg if memory_reg is not None else reg(
            (0x80000000, 0x80000000), (0x100000000, upper_size))),
    ] + list(memory_props or []))]
    if reserved:
        children.append(('reserved-memory', [('#address-cells', cells(2)),
                                             ('#size-cells', cells(2)), ('ranges', b'')], reserved))
    if chosen:
        children.append(('chosen', chosen))
    structure = encode_node('', [('#address-cells', cells(root_cells[0])),
                                  ('#size-cells', cells(root_cells[1]))], children) + cells(9)
    reservation_table = reg(*memreserve) + bytes(16)
    off_struct = 40 + len(reservation_table)
    off_strings = off_struct + len(structure)
    total = off_strings + len(names)
    header = cells(0xd00dfeed, total, off_struct, off_strings, 40,
                   17, 16, 0, len(names), len(structure))
    return header + reservation_table + structure + names


class MemoryPlanTests(unittest.TestCase):
    def test_six_and_eight_gib_variants_use_detected_upper_bank(self):
        for size in (0xfcc00000, 0x17cc00000):
            with self.subTest(size=size):
                plan = fdt.upper_memory_plan(dtb(upper_size=size))
                self.assertEqual(plan['selected'], (0x100000000, 0x100000000 + size))
                self.assertEqual(plan['detected_bytes'], 0x80000000 + size)

    def test_static_hole_never_enters_kernel_range(self):
        plan = fdt.upper_memory_plan(dtb(reserved=[
            ('secure@140000000', [('reg', reg((0x140000000, 0x2000000)))])]))
        self.assertEqual(plan['selected'], (0x142000000, 0x1fcc00000))

    def test_fdt_memreserve_table_is_honored(self):
        plan = fdt.upper_memory_plan(dtb(memreserve=[(0x100000000, 0x18000000)]))
        self.assertEqual(plan['selected'][0], 0x118000000)

    def test_unaligned_reservation_does_not_leak_into_aligned_range(self):
        plan = fdt.upper_memory_plan(dtb(memreserve=[(0x100000000, 0x2000001)]))
        self.assertEqual(plan['selected'][0], 0x104000000)

    def test_dynamic_pools_confined_below_four_gib_leave_upper_ram(self):
        plan = fdt.upper_memory_plan(dtb(reserved=[('camera-pool', [
            ('size', struct.pack('>Q', 0x2000000)),
            ('alloc-ranges', reg((0x80000000, 0x80000000))),
            ('reusable', b'')])]))
        self.assertEqual(plan['selected'], (0x100000000, 0x1fcc00000))

    def test_unbounded_dynamic_pool_fails_instead_of_guessing(self):
        with self.assertRaisesRegex(ValueError, 'no alloc-ranges'):
            fdt.upper_memory_plan(dtb(reserved=[('pool', [('size', struct.pack('>Q', 4096))])]))

    def test_dynamic_upper_alloc_range_is_conservatively_excluded(self):
        plan = fdt.upper_memory_plan(dtb(reserved=[('pool', [
            ('size', struct.pack('>Q', 0x100000)),
            ('alloc-ranges', reg((0x100000000, 0x20000000)))])]))
        self.assertEqual(plan['selected'][0], 0x120000000)

    def test_disabled_reserved_node_does_not_remove_ram(self):
        plan = fdt.upper_memory_plan(dtb(reserved=[('unused', [
            ('reg', reg((0x100000000, 0xfcc00000))), ('status', b'disabled\0')])]))
        self.assertEqual(plan['selected'][0], 0x100000000)

    def test_kexec_usable_limit_is_honored(self):
        plan = fdt.upper_memory_plan(dtb(chosen=[
            ('linux,usable-memory-range', reg((0x140000000, 0x20000000)))]))
        self.assertEqual(plan['selected'], (0x140000000, 0x160000000))

    def test_overlapping_memory_banks_rejected(self):
        with self.assertRaisesRegex(ValueError, 'overlapping FDT memory'):
            fdt.upper_memory_plan(dtb(memory_reg=reg((0x100000000, 0x40000000),
                                                    (0x120000000, 0x40000000))))

    def test_missing_upper_bank_is_error(self):
        with self.assertRaisesRegex(ValueError, 'no safe contiguous upper RAM'):
            fdt.upper_memory_plan(dtb(memory_reg=reg((0x80000000, 0x80000000))))

    def test_one_cell_root_is_parsed(self):
        # With 32-bit addresses, no upper-bank region is representable.
        with self.assertRaisesRegex(ValueError, 'no safe contiguous upper RAM'):
            fdt.upper_memory_plan(dtb(root_cells=(1, 1), memory_reg=cells(0x80000000, 0x70000000)))

    def test_truncated_or_concatenated_dtb_is_rejected(self):
        blob = dtb()
        for sample in (blob[:10], blob[:-1], blob + blob):
            with self.subTest(length=len(sample)), self.assertRaises(ValueError):
                fdt.upper_memory_plan(sample)

    def test_corrupt_structure_length_and_reservation_table_are_rejected(self):
        blob = bytearray(dtb())
        struct.pack_into('>I', blob, 36, len(blob))
        with self.assertRaises(ValueError):
            fdt.parse(blob)
        blob = bytearray(dtb())
        struct.pack_into('>QQ', blob, 40, 0x100000000, 4096)
        with self.assertRaisesRegex(ValueError, 'reservation table'):
            fdt.parse(blob)

    def test_adt_carves_panic_log_once_and_exposes_eight_cpus(self):
        with contextlib.redirect_stderr(io.StringIO()):
            tree = mkadt.build(seed=bytes(64), memory_region=(0x100000000, 0x17cc00000))
        chosen = next(n for n in tree.children if n.name == 'chosen')
        panic = next(n for n in tree.children if n.name == 'pram')
        cpus = next(n for n in tree.children if n.name == 'cpus')
        self.assertEqual(struct.unpack('<Q', chosen.prop('dram-size'))[0], 0x17cb00000)
        self.assertEqual(struct.unpack('<QQ', panic.prop('reg')), (0x27cb00000, 0x100000))
        self.assertEqual(len(cpus.children), 8)
        self.assertEqual([struct.unpack('<I', cpu.prop('reg'))[0] for cpu in cpus.children],
                         list(range(0, 0x800, 0x100)))

    def test_cli_generates_matching_adt_and_memory_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'input.dtb').write_bytes(dtb(upper_size=0x17cc00000))
            result = subprocess.run([sys.executable, str(Path(mkadt.__file__)), str(path / 'out.adt'),
                                     '--fdt', str(path / 'input.dtb'), '--memory-report', str(path / 'ram.json')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            base, size = mkpayload.adt_dram((path / 'out.adt').read_bytes())
            self.assertEqual((base, size), (0x100000000, 0x17cb00000))
            import json
            report = json.loads((path / 'ram.json').read_text())
            self.assertEqual(report['managed_bytes'], size)

    def test_cli_cannot_overwrite_its_input_dtb(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'live.dtb'
            original = dtb()
            path.write_bytes(original)
            result = subprocess.run([sys.executable, str(Path(mkadt.__file__)), str(path),
                                     '--fdt', str(path)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('different paths', result.stderr)
            self.assertEqual(path.read_bytes(), original)


class PayloadBoundsTests(unittest.TestCase):
    VIRT = 0xfffffe0000000000

    def check(self, **overrides):
        args = dict(pieces=[(self.VIRT, 0x4000, bytes(0x100))], virt_base=self.VIRT,
                    entry=self.VIRT, kernel_end=self.VIRT + 0x4000,
                    phys_base=0x100000000, mem_size=0x1000000, adt_size=4096)
        args.update(overrides)
        return mkpayload.check_memory_layout(**args)

    def test_metadata_placement_accounts_for_both_adt_and_bootargs(self):
        layout = self.check()
        self.assertEqual(layout['adt_phys'], '0x100004000')
        self.assertEqual(layout['boot_args_phys'], '0x100008000')
        self.assertEqual(layout['top_of_kernel_data'], '0x10000c000')
        self.check(mem_size=0xc000)  # exact fit is valid

    def test_adt_and_bootargs_overflow_is_rejected_even_when_kernel_fits(self):
        with self.assertRaisesRegex(ValueError, 'do not fit'):
            self.check(mem_size=0x8000)

    def test_ramdisk_overflow(self):
        with self.assertRaisesRegex(ValueError, 'exceeds'):
            self.check(pieces=[(self.VIRT, 0x4000, bytes(256)),
                               (self.VIRT + 0x4000, 0x1000000, b'rd')])

    def test_overlapping_segments_and_unloaded_entry(self):
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            self.check(pieces=[(self.VIRT, 0x8000, bytes(256)),
                               (self.VIRT + 0x4000, 0x4000, bytes(256))])
        with self.assertRaisesRegex(ValueError, 'entry point'):
            self.check(entry=self.VIRT + 0x2000)

    def test_physical_uint64_overflow_or_unaligned_base(self):
        with self.assertRaisesRegex(ValueError, 'RAM range'):
            self.check(phys_base=(1 << 64) - 0x2000000, mem_size=0x4000000)
        with self.assertRaisesRegex(ValueError, 'RAM range'):
            self.check(phys_base=0x100004000)


class PayloadIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.base = 0xfffffe0000000000
        segment = struct.pack('<II16sQQQQIIII', mkpayload.LC_SEGMENT_64, 72, b'__TEXT',
                              self.base, 0x4000, 0, 392, 5, 5, 0, 0)
        thread = bytearray(struct.pack('<4I', mkpayload.LC_UNIXTHREAD, 288, 6, 68) + bytes(272))
        struct.pack_into('<Q', thread, 16 + 32 * 8, self.base + 0x100)
        header = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 2, 360, 0, 0)
        (self.path / 'kernel').write_bytes(header + segment + thread)
        self.write_adt()

    def write_adt(self, qemu=False):
        with contextlib.redirect_stderr(io.StringIO()):
            tree = mkadt.build(seed=bytes(64), qemu=qemu)
        (self.path / 'tree.adt').write_bytes(adt.serialize(tree))

    def test_phone_and_qemu_default_cpu_arguments_follow_the_tree(self):
        for qemu, count in ((False, 8), (True, 1)):
            with self.subTest(qemu=qemu):
                self.write_adt(qemu)
                blob, info = mkpayload.build(self.path / 'kernel', self.path / 'tree.adt')
                command = blob[0x48:0x2a8].split(b'\0')[0].decode()
                self.assertIn(f'cpus={count}', command)
                self.assertEqual(command, info['cmdline'])
                self.assertEqual(struct.unpack_from('<Q', blob, 0x40)[0],
                                 mkpayload.adt_dram((self.path / 'tree.adt').read_bytes())[1])

    def test_ramdisk_and_metadata_are_included_in_the_validated_range(self):
        (self.path / 'root.img').write_bytes(bytes(17000))
        blob, info = mkpayload.build(self.path / 'kernel', self.path / 'tree.adt',
                                    'cpus=1 rd=md0', self.path / 'root.img')
        _, _, nsegments = struct.unpack_from('<8sII', blob)
        self.assertEqual(nsegments, 2)
        offset, size = struct.unpack_from('<QQ', blob, 0x20)
        tree, _ = adt.parse(blob[offset:offset + size])
        chosen = next(node for node in tree.children if node.name == 'chosen')
        mmap = next(node for node in chosen.children if node.name == 'memory-map')
        self.assertEqual(struct.unpack('<QQ', mmap.prop('RAMDisk')), (0xb0004000, 0x8000))
        self.assertLessEqual(int(info['top_of_kernel_data'], 16),
                             int(info['phys_base'], 16) + int(info['mem_size'], 16))

    def test_oversized_command_line_is_not_silently_truncated(self):
        with self.assertRaisesRegex(ValueError, 'at most 607'):
            mkpayload.build(self.path / 'kernel', self.path / 'tree.adt', 'x' * 608)

    def test_truncated_macho_segment_is_not_zero_filled_and_accepted(self):
        binary = (self.path / 'kernel').read_bytes()
        (self.path / 'kernel').write_bytes(binary[:-1])
        with self.assertRaisesRegex(ValueError, 'Mach-O file'):
            mkpayload.build(self.path / 'kernel', self.path / 'tree.adt')

    def test_entry_into_appended_ramdisk_is_rejected(self):
        binary = bytearray((self.path / 'kernel').read_bytes())
        struct.pack_into('<Q', binary, 32 + 72 + 16 + 32 * 8, self.base + 0x4000)
        (self.path / 'kernel').write_bytes(binary)
        (self.path / 'root.img').write_bytes(b'not kernel code')
        with self.assertRaisesRegex(ValueError, 'file-backed kernel segments'):
            mkpayload.build(self.path / 'kernel', self.path / 'tree.adt',
                            ramdisk_path=self.path / 'root.img')


if __name__ == '__main__':
    unittest.main()
