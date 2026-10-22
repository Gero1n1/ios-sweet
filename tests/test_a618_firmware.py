"""Run A618 firmware primitives on inert memory, never on a device."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'requires a host C compiler')
class A618FirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        for name in ('firmware.c', 'firmware.h'):
            shutil.copyfile(ROOT / 'xnu/adreno618' / name, cls.path / name)
        shutil.copyfile(ROOT / 'tests/c/a618_firmware_test.c', cls.path / 'test.c')
        cls.binaries = []
        compilers = [('cc', '-std=c11')]
        if shutil.which('c++'):
            compilers.append(('c++', '-std=c++14'))
        for compiler, standard in compilers:
            binary = cls.path / (compiler.replace('+', 'p') + ('.exe' if os.name == 'nt' else ''))
            result = subprocess.run([compiler, standard, '-pedantic', '-Wall', '-Wextra',
                                     '-Werror', '-O2', 'firmware.c', 'test.c', '-o', str(binary)],
                                    cwd=cls.path, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries.append(binary)

    def case(self, name):
        for binary in self.binaries:
            with self.subTest(compiler=binary.name):
                result = subprocess.run([str(binary), name], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), 'PASS')

    def test_gmu_upload_stays_in_itcm_and_preserves_reset(self): self.case('upload')
    def test_bounds_wrong_chip_and_running_gmu_rejected_before_writes(self): self.case('validation')
    def test_sqe_header_strip_and_output_bounds(self): self.case('sqe')
