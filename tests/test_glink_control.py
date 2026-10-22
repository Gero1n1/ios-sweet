"""Execute GLINK negotiation over the real C FIFO transport, without MMIO."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'requires a host C compiler')
class GlinkControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        for name in ('glink_smem.c', 'glink_smem.h', 'glink_control.c', 'glink_control.h'):
            shutil.copyfile(ROOT / 'xnu/wcn3990' / name, cls.path / name)
        shutil.copyfile(ROOT / 'tests/c/glink_control_test.c', cls.path / 'test.c')
        cls.binaries = []
        compilers = [('cc', '-std=c11')]
        if shutil.which('c++'):
            compilers.append(('c++', '-std=c++14'))
        for compiler, standard in compilers:
            binary = cls.path / (compiler.replace('+', 'p') + ('.exe' if os.name == 'nt' else ''))
            result = subprocess.run([compiler, standard, '-pedantic', '-Wall', '-Wextra',
                                     '-Werror', '-O2', 'glink_smem.c', 'glink_control.c',
                                     'test.c', '-o', str(binary)],
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

    def test_initialization_and_exact_wire_vectors(self): self.case('vectors')
    def test_two_endpoints_converge_on_shared_fifo_transport(self): self.case('peers')
    def test_features_and_newer_versions_get_counter_offers(self): self.case('counteroffer')
    def test_blocked_reply_survives_until_space_is_available(self): self.case('blocked')
    def test_read_notification_follows_consumption(self): self.case('read-notif')
    def test_rejection_unknown_commands_and_corrupt_indices(self): self.case('errors')
    def test_wrapping_headers_at_every_aligned_fifo_position(self): self.case('wrap')
    def test_invalid_or_aliased_state_does_not_modify_shared_memory(self): self.case('arguments')
