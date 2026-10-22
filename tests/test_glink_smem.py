"""Execute the real GLINK mapped-ring transport as C and C++."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'requires a host C compiler')
class GlinkSmemTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        for name in ('glink_smem.c', 'glink_smem.h'):
            shutil.copyfile(ROOT / 'xnu/wcn3990' / name, cls.path / name)
        shutil.copyfile(ROOT / 'tests/c/glink_smem_test.c', cls.path / 'test.c')
        cls.binaries = []
        compilers = [('cc', '-std=c11')]
        if shutil.which('c++'):
            compilers.append(('c++', '-std=c++14'))
        for compiler, standard in compilers:
            binary = cls.path / (compiler.replace('+', 'p') + ('.exe' if os.name == 'nt' else ''))
            result = subprocess.run([compiler, standard, '-pedantic', '-Wall', '-Wextra',
                                     '-Werror', '-O2', 'glink_smem.c', 'test.c', '-o', str(binary)],
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

    def test_bind_rejects_invalid_or_overlapping_shared_mappings(self): self.case('binding')
    def test_tx_wrap_padding_and_publication_before_mailbox(self): self.case('transmit')
    def test_backpressure_and_reserved_read_notification(self): self.case('backpressure')
    def test_rx_peek_bounds_and_explicit_consumption(self): self.case('receive')
    def test_corrupt_indices_and_size_overflow_do_not_change_memory(self): self.case('corruption')
    def test_every_aligned_position_and_frame_size_in_small_fifo(self): self.case('exhaustive')
