"""Apply the framebuffer patch chain, then compile its actual cadence helper."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TARGETS = ('iokit/Kernel/arm/SweetFramebuffer.cpp', 'iokit/Kernel/arm/SweetFramePacing.h')


@unittest.skipUnless(shutil.which('patch') and shutil.which('cc'), 'requires patch and a host C compiler')
class FramePacingTests(unittest.TestCase):
    def test_patch_chain_and_cadence_invariants(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for patch in sorted((ROOT / 'xnu/patches').glob('*.patch')):
                sections = re.split(r'(?m)(?=^--- )', patch.read_text(encoding='latin1'))
                for section in sections:
                    if not any('\n+++ b/' + target + '\n' in section for target in TARGETS):
                        continue
                    result = subprocess.run(['patch', '--batch', '--fuzz=0', '-p1'],
                                            input=section, cwd=path, text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, f'{patch.name}: {result.stdout}{result.stderr}')
            source = path / 'check.c'
            source.write_text(r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include "SweetFramePacing.h"

int main(void)
{
    for (uint32_t requested = 1; requested <= 60; ++requested) {
        uint32_t phase = 0, copies = 0;
        for (uint32_t tick = 1; tick <= 3600; ++tick) {
            copies += sweet_fb_copy_due(requested, &phase);
            /* At every prefix, cumulative timing error is below one copy. */
            assert(copies * 60 <= tick * requested);
            assert(tick * requested - copies * 60 < 60);
            assert(phase < 60);
        }
        assert(copies == requested * 60);
    }
    const uint32_t unthrottled[] = {0, 60, 61, 120, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(unthrottled) / sizeof(unthrottled[0]); ++i) {
        uint32_t phase = 0;
        for (unsigned tick = 0; tick < 3600; ++tick) {
            assert(sweet_fb_copy_due(unthrottled[i], &phase));
            assert(phase == 0);
        }
    }
    return 0;
}
''')
            compilers = [('cc', '-std=c11')]
            if shutil.which('c++'):
                compilers.append(('c++', '-std=c++14'))
            for compiler, standard in compilers:
                with self.subTest(compiler=compiler):
                    binary = path / compiler.replace('+', 'p')
                    result = subprocess.run([compiler, standard, '-Wall', '-Wextra', '-Werror',
                                             '-I', str(path / 'iokit/Kernel/arm'), str(source), '-o', str(binary)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
