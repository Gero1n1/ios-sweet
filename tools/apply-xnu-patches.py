#!/usr/bin/env python3
"""Apply the build patch set; every project patch is mandatory, unlike DXB defaults."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def apply_patches(source, patch_dir, manifest, report):
    # Invalidate any older success report before validating this invocation.
    revision = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=source,
                              capture_output=True, text=True)
    evidence = {'complete': False,
                'source_revision': revision.stdout.strip() if revision.returncode == 0 else None,
                'patches': []}
    report.write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    required = manifest.read_text(encoding='utf-8').splitlines()
    if not required or len(required) != len(set(required)) or any(
            re.fullmatch(r'[0-9]{4}-[^/\\]+\.patch', name) is None for name in required):
        raise ValueError('required manifest must contain unique numbered patch filenames')
    required = set(required)
    patches = sorted(patch_dir.glob('*.patch'))
    missing = required - {path.name for path in patches}
    if missing:
        raise ValueError('required patches missing: ' + ', '.join(sorted(missing)))
    try:
        for path in patches:
            item = {'name': path.name, 'required': path.name in required,
                    'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
            evidence['patches'].append(item)
            check = subprocess.run(['git', 'apply', '--check', str(path.resolve())],
                                   cwd=source, capture_output=True, text=True)
            if check.returncode != 0:
                item['status'] = 'failed' if item['required'] else 'skipped'
                item['diagnostic'] = check.stdout + check.stderr
                print(f"[{item['status'].upper()}] {path.name}: {item['diagnostic']}", file=sys.stderr)
                if item['required']:
                    return 1
                continue
            result = subprocess.run(['git', 'apply', str(path.resolve())],
                                    cwd=source, capture_output=True, text=True)
            if result.returncode != 0:
                item['status'] = 'failed'
                item['diagnostic'] = result.stdout + result.stderr
                print(f"[FAILED] {path.name}: {item['diagnostic']}", file=sys.stderr)
                return 1
            item['status'] = 'applied'
            print(f"[APPLIED] {path.name} sha256={item['sha256']}")
        evidence['complete'] = True
        return 0
    finally:
        report.write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path, default=Path.cwd())
    parser.add_argument('--patch-dir', type=Path, required=True)
    parser.add_argument('--required-manifest', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    try:
        return apply_patches(args.source_dir.resolve(), args.patch_dir.resolve(),
                             args.required_manifest.resolve(), args.report.resolve())
    except (OSError, ValueError) as error:
        print(f'[FAILED] {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
