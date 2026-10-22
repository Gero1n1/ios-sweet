"""QEMU host protocol and Mach-O parsing tests; no emulator/phone required."""
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('qemu_sweet', ROOT / 'tools/qemu-sweet.py')
qemu = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qemu)


def kernel(names=(b'_iso_font',), kinds=None):
    strings = bytearray(b'\0')
    entries = bytearray()
    for i, name in enumerate(names):
        index = len(strings)
        strings.extend(name + b'\0')
        entries.extend(struct.pack('<IBBHQ', index, kinds[i] if kinds else 0x0f,
                                   1, 0, 0x100000000 + i * 0x1000))
    return (struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 1, 24, 0, 0) +
            struct.pack('<6I', 2, 24, 56, len(names), 56 + len(entries), len(strings)) +
            entries + strings)


class QemuFontTests(unittest.TestCase):
    def test_defined_font_without_external_tools(self):
        self.assertEqual(qemu.kernel_font_address(kernel()), 0x100000000)
        self.assertEqual(qemu.kernel_font_address(kernel((b'other', b'_iso_font'))),
                         0x100001000)

    def test_undefined_and_debug_entries_do_not_count(self):
        self.assertEqual(qemu.kernel_font_address(kernel(
            (b'_iso_font', b'_iso_font', b'_iso_font'), (1, 0xef, 0x0f))), 0x100002000)
        for data in (kernel((b'other',)), kernel((b'_iso_font', b'_iso_font')),
                     kernel(kinds=(1,))):
            with self.subTest(data=data), self.assertRaises(ValueError):
                qemu.kernel_font_address(data)

    def test_every_truncated_prefix_is_rejected(self):
        data = kernel()
        for size in range(len(data)):
            with self.subTest(size=size), self.assertRaises(ValueError):
                qemu.kernel_font_address(data[:size])

    def test_malformed_command_and_symbol_spans_are_rejected(self):
        for offset, value in ((0, 0), (4, 0), (16, 99), (20, 0xffffffff),
                              (36, 0), (36, 16), (36, 25), (40, 0xffffffff),
                              (44, 0xffffffff), (48, 0xffffffff), (52, 0xffffffff),
                              (56, 0xffffffff)):
            data = bytearray(kernel())
            struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                qemu.kernel_font_address(data)
        data = bytearray(kernel())
        data[-1] = 1
        with self.assertRaises(ValueError):
            qemu.kernel_font_address(data)


class QemuMonitorTests(unittest.TestCase):
    def exchange(self, mode):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(5)
        address = listener.getsockname()
        commands, errors = [], []

        def server():
            try:
                with listener, listener.accept()[0] as connection:
                    connection.settimeout(5)
                    with connection.makefile('rwb') as stream:
                        def send(value):
                            stream.write(json.dumps(value).encode() + b'\n')
                            stream.flush()
                        send({'QMP': {}})
                        while True:
                            line = stream.readline()
                            if not line:
                                return
                            command = json.loads(line)
                            commands.append(command)
                            if len(commands) == 1:
                                send({'return': {}})
                            elif mode == 'closed':
                                return
                            elif mode == 'error':
                                send({'error': {'class': 'GenericError', 'desc': 'test failure'}})
                                return
                            else:
                                send({'event': 'STOP', 'data': {}})
                                send({'return': 'registers' if command['execute'] ==
                                      'human-monitor-command' else {}})
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=server, daemon=True)
        thread.start()
        try:
            if mode == 'ok':
                self.assertEqual(qemu.qmp(address, 'info registers',
                    pmemsave=(0x9c000000, 4096, 'E:/qemu-test/fb.raw')), ['registers'])
            else:
                with self.assertRaises(RuntimeError):
                    qemu.qmp(address, 'info registers')
        finally:
            thread.join(timeout=6)
            self.assertFalse(thread.is_alive(), 'QMP test server did not stop')
            self.assertEqual(errors, [])
        return commands

    def test_tcp_qmp_events_and_native_memory_save(self):
        commands = self.exchange('ok')
        self.assertEqual([c['execute'] for c in commands],
                         ['qmp_capabilities', 'human-monitor-command', 'pmemsave'])
        self.assertEqual(commands[-1]['arguments'],
                         {'val': 0x9c000000, 'size': 4096, 'filename': 'E:/qemu-test/fb.raw'})

    def test_qmp_error_fails_the_capture(self): self.exchange('error')
    def test_closed_qmp_fails_without_read_loop(self): self.exchange('closed')

    def test_snapshot_status_does_not_infer_boot_success(self):
        for screen, unknown, expected in (('SWEET init: still alive', 0, 'no_panic_observed'),
                                         ('', 0, 'blank_screen'), ('?', 1, 'incomplete_decode'),
                                         ('panic(cpu 0 caller 0): test', 0, 'panic')):
            result = qemu.screen_result(screen, unknown)
            self.assertEqual(result['status'], expected)
            self.assertTrue(result['snapshot_only'])


class QemuCliTests(unittest.TestCase):
    def run_cli(self, *arguments):
        return subprocess.run([sys.executable, str(ROOT / 'tools/qemu-sweet.py'),
                               *arguments], capture_output=True, text=True, timeout=10)

    def test_bad_time_or_log_limits_fail_before_launching_qemu(self):
        for arguments in (('--secs', 'nan'), ('--secs', 'inf'), ('--secs', '0'),
                          ('--secs', '-1'), ('--max-log-mib', '0')):
            with self.subTest(arguments=arguments):
                result = self.run_cli('missing-payload', 'unused-output', *arguments)
                self.assertEqual(result.returncode, 2)
                self.assertIn('positive and finite', result.stderr)

    def test_previous_output_is_preserved_before_launching_qemu(self):
        with tempfile.TemporaryDirectory() as directory:
            previous = Path(directory) / 'fb.txt'
            previous.write_text('previous run', encoding='utf-8')
            # tempfile's root may contain non-ASCII user names on Windows;
            # that rejection also occurs before opening or deleting anything.
            result = self.run_cli('missing-payload', directory)
            self.assertEqual(result.returncode, 2)
            expected = 'ASCII output path' if os.name == 'nt' and not directory.isascii() else 'new or empty'
            self.assertIn(expected, result.stderr)
            self.assertEqual(previous.read_text(encoding='utf-8'), 'previous run')
