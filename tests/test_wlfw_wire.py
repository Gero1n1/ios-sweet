"""Compile and exercise the actual portable WLFW codec as C and C++."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'requires a host C compiler')
class WlfwWireTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.path = Path(cls.directory.name)
        for name in ('wlfw_wire.c', 'wlfw_wire.h'):
            shutil.copyfile(ROOT / 'xnu/wcn3990' / name, cls.path / name)
        shutil.copyfile(ROOT / 'tests/c/wlfw_wire_test.c', cls.path / 'test.c')
        cls.binaries = []
        compilers = [('cc', '-std=c11')]
        if shutil.which('c++'):
            compilers.append(('c++', '-std=c++14'))
        for compiler, standard in compilers:
            name = compiler.replace('+', 'p') + ('.exe' if os.name == 'nt' else '')
            binary = cls.path / name
            result = subprocess.run([compiler, standard, '-pedantic', '-Wall', '-Wextra',
                                     '-Werror', '-O2', 'wlfw_wire.c', 'test.c', '-o', str(binary)],
                                    cwd=cls.path, capture_output=True, text=True)
            if result.returncode != 0:
                raise AssertionError(f'{compiler}: {result.stdout}{result.stderr}')
            cls.binaries.append(binary)

    def run_case(self, case):
        for binary in self.binaries:
            with self.subTest(compiler=binary.name):
                result = subprocess.run([str(binary), case], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.strip(), 'PASS')

    def test_schema_golden_request_bytes_and_output_bounds(self):
        self.run_case('encoding')

    def test_response_fields_errors_and_optional_tlvs(self):
        self.run_case('decoding')

    def test_truncation_exhaustive_lengths_duplicates_and_correlation(self):
        self.run_case('malformed')

    def test_msa_reservation_containment_overflow_and_overlap(self):
        self.run_case('regions')


if __name__ == '__main__':
    unittest.main()
