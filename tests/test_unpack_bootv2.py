"""Synthetic AOSP v0-v2 layouts. No phone or downloaded firmware required."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('unpack_bootv2', ROOT / 'tools/unpack_bootv2.py')
boot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot)


def image(version=2, page=4096, recovery=True):
    data = bytearray(page * 6)
    data[:8] = b'ANDROID!'
    struct.pack_into('<10I', data, 8, 3, 0x80008000, 4, 0x81000000,
                     2, 0x82000000, 0x80000100, page, version, 0)
    data[48:53] = b'sweet'
    data[64:69] = b'first'
    data[608:613] = b' last'
    data[page:page + 3] = b'KER'
    data[2 * page:2 * page + 4] = b'RAMD'
    data[3 * page:3 * page + 2] = b'S2'
    if version >= 1:
        struct.pack_into('<IQI', data, 1632, 3 if recovery else 0,
                         4 * page if recovery else 0, boot.HEADER_SIZES[version])
        if recovery:
            data[4 * page:4 * page + 3] = b'REC'
    if version == 2:
        struct.pack_into('<IQ', data, 1648, 3, 0x83000000)
        start = (5 if recovery else 4) * page
        data[start:start + 3] = b'DTB'
    return data


class BootImageTests(unittest.TestCase):
    def test_versions_and_pages(self):
        for version in range(3):
            for page in boot.PAGE_SIZES:
                with self.subTest(version=version, page=page):
                    info, parts = boot.parse_boot_image(image(version, page))
                    self.assertEqual(info['cmdline'], 'first last')
                    self.assertEqual(info['header_version'], version)
                    self.assertEqual(parts['kernel'], b'KER')
                    self.assertEqual(parts['ramdisk'], b'RAMD')
                    self.assertEqual(parts['second'], b'S2')
                    if version >= 1: self.assertEqual(parts['recovery_dtbo'], b'REC')
                    if version == 2: self.assertEqual(parts['dtb'], b'DTB')

    def test_zero_recovery_has_no_effect_on_dtb_position(self):
        info, parts = boot.parse_boot_image(image(recovery=False))
        self.assertNotIn('recovery_dtbo', parts)
        self.assertEqual(info['dtb_offset'], 4 * 4096)
        self.assertEqual(parts['dtb'], b'DTB')

    def test_recovery_uses_declared_offset(self):
        data = image(version=1)
        struct.pack_into('<Q', data, 1636, 5 * 4096)
        data[5 * 4096:5 * 4096 + 3] = b'ALT'
        self.assertEqual(boot.parse_boot_image(data)[1]['recovery_dtbo'], b'ALT')

    def test_truncated_headers(self):
        data = image()
        for size in (0, 7, 47, 1631, 1647, 1659, 4095):
            with self.subTest(size=size), self.assertRaises(ValueError):
                boot.parse_boot_image(data[:size])

    def test_reject_magic_versions_page_sizes_and_header_sizes(self):
        for position, value in [(40, 3), (40, 4), (40, 0xffffffff),
                                (36, 0), (36, 1), (36, 4095),
                                (36, 0xffffffff), (1644, 0), (1644, 1648)]:
            data = image()
            struct.pack_into('<I', data, position, value)
            with self.subTest(position=position, value=value), self.assertRaises(ValueError):
                boot.parse_boot_image(data)
        data = image()
        data[0] = 0
        with self.assertRaises(ValueError): boot.parse_boot_image(data)

    def test_empty_kernel_and_component_size_overflows(self):
        for position, value in [(8, 0), (8, 0xffffffff), (16, 0xffffffff),
                                (24, 0xffffffff), (1632, 0xffffffff), (1648, 0xffffffff)]:
            data = image()
            struct.pack_into('<I', data, position, value)
            with self.subTest(position=position), self.assertRaises(ValueError):
                boot.parse_boot_image(data)

    def test_truncated_components(self):
        data = image()
        for size in (4096 + 2, 8192 + 3, 12288 + 1, 16384 + 2, 20480 + 2):
            with self.subTest(size=size), self.assertRaises(ValueError):
                boot.parse_boot_image(data[:size])

    def test_recovery_alignment_bounds_and_overlap(self):
        for offset in (0, 4096, 8192, 12288, 20480, 16385, 2**64 - 1):
            data = image()
            struct.pack_into('<Q', data, 1636, offset)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                boot.parse_boot_image(data)

    def test_reject_before_creating_output(self):
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / 'bad.img'
            src.write_bytes(image()[:20481])
            target = Path(directory) / 'output'
            with self.assertRaises(ValueError): boot.unpack(src, target)
            self.assertFalse(target.exists())

    def test_output_components_and_stale_file_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            src, target = Path(directory) / 'boot.img', Path(directory) / 'out'
            src.write_bytes(image())
            summary = boot.unpack(src, target)
            self.assertIn('header_version: 2', summary)
            self.assertEqual((target / 'dtb').read_bytes(), b'DTB')
            with self.assertRaisesRegex(ValueError, 'stale'): boot.unpack(src, target)
            self.assertEqual((target / 'dtb').read_bytes(), b'DTB')

    def test_cli_failure_is_clean(self):
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / 'bad.img'
            src.write_bytes(b'ANDROID!')
            result = subprocess.run([sys.executable, str(ROOT / 'tools/unpack_bootv2.py'),
                                     str(src), str(Path(directory) / 'out')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('error:', result.stderr)
            self.assertNotIn('Traceback', result.stderr)
