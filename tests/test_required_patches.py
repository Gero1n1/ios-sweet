"""Exercise actual git application and mandatory failure behavior in build tooling."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('git'), 'requires git')
class RequiredPatchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.source = self.path / 'source'
        self.patches = self.path / 'patches'
        self.source.mkdir()
        self.patches.mkdir()
        (self.source / 'file').write_text('original\n', encoding='utf-8', newline='\n')
        self.manifest = self.path / 'required.txt'
        self.report = self.path / 'report.json'

    def patch(self, name, old, new):
        value = f'--- a/file\n+++ b/file\n@@ -1 +1 @@\n-{old}\n+{new}\n'
        path = self.patches / name
        path.write_text(value, encoding='utf-8', newline='\n')
        return path

    def run_apply(self, names):
        self.manifest.write_text('\n'.join(names) + '\n', encoding='utf-8')
        return subprocess.run([sys.executable, str(ROOT / 'tools/apply-xnu-patches.py'),
                               '--source-dir', str(self.source), '--patch-dir', str(self.patches),
                               '--required-manifest', str(self.manifest), '--report', str(self.report)],
                              capture_output=True, text=True)

    def test_success_records_hashes_and_optional_failure(self):
        patch = self.patch('0001-required.patch', 'original', 'changed')
        self.patch('optional.patch', 'absent-context', 'unused')
        result = self.run_apply(['0001-required.patch'])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(self.report.read_text())
        self.assertTrue(report['complete'])
        self.assertEqual([p['status'] for p in report['patches']], ['applied', 'skipped'])
        self.assertEqual(report['patches'][0]['sha256'], hashlib.sha256(patch.read_bytes()).hexdigest())
        self.assertEqual((self.source / 'file').read_text(), 'changed\n')

    def test_required_context_failure_stops_later_patches(self):
        self.patch('0001-required.patch', 'original', 'step-one')
        self.patch('0002-required.patch', 'wrong-context', 'step-two')
        self.patch('0003-required.patch', 'step-one', 'must-not-apply')
        result = self.run_apply(['0001-required.patch', '0002-required.patch', '0003-required.patch'])
        self.assertNotEqual(result.returncode, 0)
        report = json.loads(self.report.read_text())
        self.assertFalse(report['complete'])
        self.assertEqual([p['status'] for p in report['patches']], ['applied', 'failed'])
        self.assertEqual((self.source / 'file').read_text(), 'step-one\n')

    def test_missing_required_file_invalidates_older_success(self):
        self.report.write_text('{"complete":true}')
        result = self.run_apply(['0001-missing.patch'])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('required patches missing', result.stderr)
        self.assertFalse(json.loads(self.report.read_text())['complete'])
        self.assertEqual((self.source / 'file').read_text(), 'original\n')

    def test_already_applied_required_patch_fails_instead_of_skipping(self):
        self.patch('0001-required.patch', 'original', 'changed')
        self.assertEqual(self.run_apply(['0001-required.patch']).returncode, 0)
        self.assertNotEqual(self.run_apply(['0001-required.patch']).returncode, 0)
        report = json.loads(self.report.read_text())
        self.assertFalse(report['complete'])
        self.assertEqual(report['patches'][0]['status'], 'failed')

    def test_duplicate_manifest_fails_before_source_changes(self):
        self.patch('0001-required.patch', 'original', 'changed')
        result = self.run_apply(['0001-required.patch', '0001-required.patch'])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.source / 'file').read_text(), 'original\n')
        self.assertFalse(json.loads(self.report.read_text())['complete'])


if __name__ == '__main__':
    unittest.main()
