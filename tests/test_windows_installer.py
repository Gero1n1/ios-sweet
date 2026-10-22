"""Run the BAT against an inert native fastboot double. Never run real fastboot."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMPILER = shutil.which('cc') or shutil.which('clang')


@unittest.skipUnless(os.name == 'nt' and COMPILER, 'requires Windows and C compiler')
class WindowsInstallerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        cls.build_path = Path(cls.build.name)
        shutil.copyfile(ROOT / 'tests/c/fake_fastboot.c', cls.build_path / 'fake.c')
        cls.fake = cls.build_path / 'fastboot.exe'
        result = subprocess.run([COMPILER, '-std=c11', '-O2', 'fake.c', '-o', str(cls.fake)],
                                cwd=cls.build_path, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.kit = self.path / 'install kit'
        self.kit.mkdir()
        shutil.copyfile(ROOT / 'install-windows10.bat', self.kit / 'install-windows10.bat')
        self.bin = self.path / 'inert tools'
        self.bin.mkdir()
        shutil.copyfile(self.fake, self.bin / 'fastboot.exe')
        self.temp = self.path / 'temporary logs'
        self.temp.mkdir()
        self.log = self.path / 'fake-calls.txt'
        # Test placeholders are never sent to a real device or parsed as firmware.
        (self.kit / 'ios16-root.simg').write_bytes(b'installer test placeholder')
        (self.kit / 'shim-boot.img').write_bytes(b'installer test placeholder')

    def run_installer(self, scenario='success', confirmation='ERASE userdata'):
        env = os.environ.copy()
        system = Path(os.environ['SystemRoot']) / 'System32'
        # Only the inert stub and Windows system tools can be resolved.
        env['PATH'] = os.pathsep.join(map(str, [self.bin, system, system / 'WindowsPowerShell/v1.0']))
        env['TEMP'] = str(self.temp)
        env['SWEET_TEST_LOG'] = str(self.log)
        env['SWEET_TEST_SCENARIO'] = scenario
        result = subprocess.run('"' + str(self.kit / 'install-windows10.bat') + '"',
                                shell=True, env=env, input=confirmation + '\n',
                                capture_output=True, text=True, timeout=40)
        calls = [line.split('\t') for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, calls

    def assert_no_write(self, scenario, confirmation='ERASE userdata'):
        result, calls = self.run_installer(scenario, confirmation)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(any('flash' in call or 'boot' in call for call in calls), calls)

    def test_success_pins_serial_and_rechecks_identity_before_write(self):
        result, calls = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sum('product' in c for c in calls), 2)
        self.assertEqual(sum('unlocked' in c for c in calls), 2)
        self.assertEqual(sum('is-userspace' in c for c in calls), 2)
        self.assertEqual(calls[0], ['devices'])
        self.assertTrue(all(c[:2] == ['-s', 'MOCKSWEET001'] for c in calls[1:]))
        flash = next(c for c in calls if 'flash' in c)
        self.assertEqual(flash[2:6], ['-S', '512M', 'flash', 'userdata'])
        self.assertIn('boot', calls[-1])

    def test_multiple_devices_blocked(self): self.assert_no_write('multiple')
    def test_no_device_blocked(self): self.assert_no_write('none')
    def test_wrong_product_blocked(self): self.assert_no_write('wrongproduct')
    def test_locked_bootloader_blocked(self): self.assert_no_write('locked')
    def test_fastbootd_blocked(self): self.assert_no_write('fastbootd')
    def test_failed_query_even_with_valid_text_blocked(self): self.assert_no_write('queryfail')
    def test_changed_identity_after_confirmation_blocked(self): self.assert_no_write('changed')
    def test_declined_confirmation_blocked(self): self.assert_no_write('success', 'cancel')
    def test_confirmation_requires_exact_case(self): self.assert_no_write('success', 'erase userdata')

    def test_quoted_confirmation_does_not_execute_batch_input(self):
        result, calls = self.run_installer('success', '" & echo CONFIRM_INJECTED & rem "')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('CONFIRM_INJECTED', result.stdout)
        self.assertFalse(any('flash' in c or 'boot' in c for c in calls))

    def test_missing_root_image_stops_before_device_access(self):
        (self.kit / 'ios16-root.simg').rename(self.kit / 'root.not-installed')
        result, calls = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_missing_boot_image_stops_before_device_access(self):
        (self.kit / 'shim-boot.img').rename(self.kit / 'boot.not-installed')
        result, calls = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_flash_failure_prevents_boot(self):
        result, calls = self.run_installer('flashfail')
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(any('flash' in c for c in calls))
        self.assertFalse(any('boot' in c for c in calls))

    def test_boot_failure_reported(self):
        result, calls = self.run_installer('bootfail')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('boot', calls[-1])


if __name__ == '__main__':
    unittest.main()
