#!/usr/bin/env python3
"""Replace DXB's permissive patch loop at the workflow-pinned DXB revision."""
import argparse
from pathlib import Path
import sys

OLD_LOOP = '''        for PATCH in "${PATCH_DIR}"/*.patch; do
            if APPLY_CHECK=$(git apply --check "$PATCH" 2>&1); then
                running "Applying patch: ${PATCH}"
                git apply "$PATCH"
            else
                warning "Skipping patch ${PATCH} (already upstream, previously applied, or malformed): ${APPLY_CHECK}"
            fi
        done'''

NEW_LOOP = '''        python3 "${WORK_DIR}/sweet-apply-patches.py" \\
            --patch-dir "${PATCH_DIR}" \\
            --required-manifest "${WORK_DIR}/sweet-required-patches.txt" \\
            --report "${WORK_DIR}/sweet-patches.json" || exit 1'''


def prepare(path):
    text = path.read_text(encoding='utf-8')
    if text.count(OLD_LOOP) != 1:
        raise ValueError('DXB patch loop changed or was already replaced; refusing unverified edit')
    path.write_text(text.replace(OLD_LOOP, NEW_LOOP), encoding='utf-8', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('script', type=Path)
    args = parser.parse_args()
    try:
        prepare(args.script)
    except (OSError, ValueError) as error:
        print(f'[FAILED] {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
