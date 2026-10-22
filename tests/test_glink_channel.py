"""Run the real GLINK channel lifecycle over host-backed transport rings."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'requires a host C compiler')
class GlinkChannelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        for stem in ('glink_smem', 'glink_control', 'glink_channel'):
            for extension in ('.c', '.h'):
                name = stem + extension
                shutil.copyfile(ROOT / 'xnu/wcn3990' / name, cls.path / name)
        shutil.copyfile(ROOT / 'tests/c/glink_channel_test.c', cls.path / 'test.c')
        cls.binaries = []
        compilers = [('cc', '-std=c11')]
        if shutil.which('c++'):
            compilers.append(('c++', '-std=c++14'))
        for compiler, standard in compilers:
            binary = cls.path / (compiler.replace('+', 'p') + ('.exe' if os.name == 'nt' else ''))
            result = subprocess.run([compiler, standard, '-pedantic', '-Wall', '-Wextra',
                                     '-Werror', '-O2', 'glink_smem.c', 'glink_control.c',
                                     'glink_channel.c', 'test.c', '-o', str(binary)],
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

    def test_open_wire_vectors_and_both_acknowledgements(self): self.case('open')
    def test_two_peers_negotiate_open_and_close(self): self.case('peers')
    def test_blocked_open_and_reply_are_not_lost(self): self.case('blocked')
    def test_local_and_remote_close_lifecycles(self): self.case('close')
    def test_partial_malformed_and_other_channel_commands(self): self.case('malformed')
    def test_parent_renegotiation_invalidates_channel_state(self): self.case('control')
    def test_all_names_and_aligned_fifo_positions(self): self.case('wrap')
    def test_initialization_bounds_aliases_and_output_preservation(self): self.case('arguments')
