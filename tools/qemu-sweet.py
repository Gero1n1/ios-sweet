#!/usr/bin/env python3
"""Boot a SWEETXNU payload (tools/mkpayload.py) in QEMU's virt board.

  qemu-sweet.py PAYLOAD OUTDIR [--secs N] [--gdb] [--live-serial]

Does what shim/loader.c does on the phone, but on the host: places the
kernel segments, ADT and boot_args at their physical addresses and starts
the CPU at a 6-instruction stub that jumps to the entry with x0 = boot_args.
virt RAM starts at 0x40000000; with 4G it covers the phone's framebuffer
(0x9c000000) and the kernel's phys_base (0xb0000000) as plain memory.

After N seconds (or when the guest stops) it writes into OUTDIR:
  fb.png     the 1080x2400 framebuffer (what the phone screen would show)
  regs.txt   QEMU's view of CPU state
  qemu.log   exceptions with ESR/FAR/ELR (-d int,guest_errors)

With --kernel KERNEL (the matching unstripped Mach-O), also decode the
framebuffer using its iso_font into fb.txt and write result.json. This is a
snapshot of the circular screen, not a complete boot log. --fail-on-panic
returns 2 when a panic is visible; absence of a panic does not prove boot.
An undecodable or blank screen returns 3 with --fail-on-panic.
Windows uses loopback TCP QMP and needs an ASCII output path. No llvm-nm is
required. The default debug-log cap is 64 MiB. --icount-shift is an explicit
instruction-clock experiment, not a performance or hardware timing test.
"""
import argparse, hashlib, json, math, os, shutil, socket, struct, subprocess, sys, time, zlib

FB_BASE, FB_W, FB_H = 0x9c000000, 1080, 2400
FB_STRIDE = FB_W * 4
PAGE = 0x4000
STUB_PA = 0x40200000	# above the dtb QEMU places at RAM base
BOOT_LINE_LENGTH = 608


def check_qemu_dependency(cpu='cortex-a76'):
    """Verify qemu-system-aarch64 binary and CPU model support."""
    qemu_bin = shutil.which('qemu-system-aarch64')
    if not qemu_bin:
        print('[!] Error: qemu-system-aarch64 not found in PATH.', file=sys.stderr)
        print('    Install instructions:', file=sys.stderr)
        print('      Arch Linux:    sudo pacman -S qemu-system-aarch64', file=sys.stderr)
        print('      Ubuntu/Debian: sudo apt install qemu-system-arm', file=sys.stderr)
        print('      Fedora:         sudo dnf install qemu-system-aarch64', file=sys.stderr)
        print('      macOS:          brew install qemu', file=sys.stderr)
        return False
    try:
        ver = subprocess.check_output([qemu_bin, '--version'], text=True).splitlines()[0]
        print(f'[*] Found QEMU: {ver}', file=sys.stderr)
    except Exception as e:
        print(f'[!] Warning: Failed to query QEMU version: {e}', file=sys.stderr)
        return False

    try:
        cpu_list = subprocess.check_output([qemu_bin, '-M', 'virt', '-cpu', 'help'], text=True)
        if cpu in cpu_list:
            print(f'[*] CPU model {cpu} supported', file=sys.stderr)
        else:
            print(f'[!] Warning: CPU model {cpu} not explicitly listed in qemu-system-aarch64 -cpu help', file=sys.stderr)
    except Exception:
        pass
    return True


def check_kernel_tools(kernel_path):
    """Font lookup reads LC_SYMTAB directly, without external LLVM tools."""
    if not os.path.isfile(kernel_path):
        print(f'[!] Error: kernel file does not exist: {kernel_path}', file=sys.stderr)
        return False
    return True


def kernel_font_address(kernel):
    """Find the one defined _iso_font in a bounded ARM64 Mach-O symbol table."""
    def span(offset, size):
        if offset > len(kernel) or size > len(kernel) - offset:
            raise ValueError('truncated Mach-O font metadata')

    span(0, 32)
    magic, cpu, _, _, count, size = struct.unpack_from('<6I', kernel)
    if magic != 0xfeedfacf or cpu != 0x100000c or count > size // 8:
        raise ValueError('expected a 64-bit ARM Mach-O kernel')
    span(32, size)
    pos, end, tables = 32, 32 + size, []
    for _ in range(count):
        if end - pos < 8:
            raise ValueError('truncated Mach-O load command')
        cmd, length = struct.unpack_from('<2I', kernel, pos)
        if length < 8 or length % 8 or length > end - pos:
            raise ValueError('invalid Mach-O load command size')
        if cmd == 2:
            if length != 24:
                raise ValueError('invalid LC_SYMTAB size')
            tables.append(struct.unpack_from('<4I', kernel, pos + 8))
        pos += length
    if pos != end or len(tables) != 1:
        raise ValueError('kernel needs exactly one LC_SYMTAB')
    symbols, number, strings, string_bytes = tables[0]
    span(symbols, number * 16)
    span(strings, string_bytes)
    found = []
    for i in range(number):
        name, kind, section, _, address = struct.unpack_from('<IBBHQ', kernel, symbols + i * 16)
        if kind & 0xe0 or kind & 0x0e != 0x0e or not section:
            continue  # Debug and undefined entries are not font definitions.
        if name >= string_bytes:
            raise ValueError('symbol name is outside Mach-O string table')
        stop = kernel.find(b'\0', strings + name, strings + string_bytes)
        if stop < 0:
            raise ValueError('unterminated Mach-O symbol name')
        if kernel[strings + name:stop] == b'_iso_font':
            found.append(address)
    if len(found) != 1:
        raise ValueError('kernel must contain exactly one defined _iso_font symbol')
    return found[0]


def build_image(payload):
    """Return (phys_base, ram_image, boot_args_pa, entry_pa)."""
    magic, ver, nsegs = struct.unpack_from('<8sII', payload, 0)
    assert magic == b'SWEETXNU' and ver == 1, 'not a SWEETXNU v1 payload'
    virt_base, entry_va, adt_off, adt_size, kernel_end, phys_base, mem_size = \
        struct.unpack_from('<7Q', payload, 0x10)
    cmdline = payload[0x48:0x48 + BOOT_LINE_LENGTH]
    delta = phys_base - virt_base
    adt_pa = kernel_end + delta
    ba_pa = (adt_pa + adt_size + PAGE - 1) & ~(PAGE - 1)
    top = ba_pa + PAGE
    img = bytearray(top - phys_base)
    for i in range(nsegs):
        vmaddr, vmsize, foff, fsize = struct.unpack_from('<4Q', payload, 0x2a8 + 32 * i)
        o = vmaddr + delta - phys_base
        img[o:o + fsize] = payload[foff:foff + fsize]
    img[adt_pa - phys_base:adt_pa - phys_base + adt_size] = payload[adt_off:adt_off + adt_size]
    ba = struct.pack('<HH4x4Q6QI4xQI', 2, 2, virt_base, phys_base, mem_size, top,
                     FB_BASE, 1, FB_STRIDE, FB_W, FB_H, 32, 0, adt_pa - delta, adt_size)
    ba += cmdline + b'\0' * 4 + struct.pack('<QQ', 0, mem_size)
    assert len(ba) == 736
    img[ba_pa - phys_base:ba_pa - phys_base + len(ba)] = ba
    return phys_base, bytes(img), ba_pa, entry_va + delta


def stub(ba_pa, entry_pa):
    # ldr x0, =ba ; ldr x1, =entry ; br x1 ; nop ; .quad ba ; .quad entry
    return struct.pack('<4I2Q', 0x58000080, 0x580000a1, 0xd61f0020, 0xd503201f, ba_pa, entry_pa)


def write_png(path, raw):
    rows = []
    for y in range(FB_H):
        line = raw[y * FB_STRIDE:(y + 1) * FB_STRIDE]
        rgb = bytearray(FB_W * 3)	# a8r8g8b8 little endian = B G R A in memory
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
        rows.append(b'\0' + bytes(rgb))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', FB_W, FB_H, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def payload_data(payload, va, size):
    """Read initialized segment bytes by VA (the payload has no slide)."""
    nsegs = struct.unpack_from('<I', payload, 12)[0]
    for i in range(nsegs):
        vmaddr, _, foff, fsize = struct.unpack_from('<4Q', payload, 0x2a8 + 32 * i)
        if vmaddr <= va and va + size <= vmaddr + fsize:
            offset = foff + va - vmaddr
            return payload[offset:offset + size]
    raise ValueError(f'VA {va:#x}+{size:#x} not in initialized payload segments')


def decode_screen(raw, font):
    """Exact glyph matching for pe_serial.c's 2x, white-on-black fbcon."""
    glyphs = {font[c * 16:(c + 1) * 16]: chr(c) for c in range(32, 127)}
    rows, unknown = [], 0
    for row in range(FB_H // 32):
        chars = []
        for col in range(FB_W // 16):
            glyph = bytearray(16)
            for y in range(16):
                offset = (row * 32 + y * 2) * FB_STRIDE + col * 16 * 4
                for x in range(8):
                    if raw[offset + x * 8:offset + x * 8 + 4] == b'\xff' * 4:
                        glyph[y] |= 1 << x
            key = bytes(glyph)
            char = glyphs.get(key, '?')
            unknown += key not in glyphs
            chars.append(char)
        rows.append(''.join(chars).rstrip())
    return '\n'.join(rows).rstrip() + '\n', unknown


def screen_result(screen, unknown):
    if 'panic(cpu ' in screen or 'SWEET EARLY PANIC:' in screen:
        status = 'panic'
    elif unknown:
        status = 'incomplete_decode'
    elif screen.strip():
        status = 'no_panic_observed'
    else:
        status = 'blank_screen'
    return {'status': status, 'unknown_glyphs': unknown, 'snapshot_only': True}


def qmp(sock_path, *cmds, pmemsave=None):
    """Run QEMU human-monitor commands over QMP (synchronous, unlike the echoing HMP socket)."""
    s = socket.socket(socket.AF_INET if isinstance(sock_path, tuple) else socket.AF_UNIX)
    s.settimeout(10)
    try:
        s.connect(sock_path)
    except BaseException:
        s.close()
        raise
    f = s.makefile('rw')

    def call(execute, **args):
        f.write(json.dumps({'execute': execute, 'arguments': args}) + '\n')
        f.flush()
        while True:
            line = f.readline()
            if not line:
                raise RuntimeError('QMP closed before replying')
            r = json.loads(line)
            if 'return' in r or 'error' in r:
                if 'error' in r:
                    raise RuntimeError(f'QMP {execute}: {r["error"]}')
                return r
    try:
        greeting = json.loads(f.readline())
        if 'QMP' not in greeting:
            raise RuntimeError('missing QMP greeting')
        call('qmp_capabilities')
        out = []
        for c in cmds:
            r = call('human-monitor-command', **{'command-line': c})
            out.append(r['return'])
        if pmemsave:  # native QMP avoids HMP filename quoting problems
            call('pmemsave', val=pmemsave[0], size=pmemsave[1], filename=pmemsave[2])
        return out
    finally:
        f.close()
        s.close()


def stream_serial_file(serial_path, file_pos, out_stream=sys.stdout):
    """Read newly appended content from serial.txt and write directly to stream."""
    if not os.path.exists(serial_path):
        return file_pos
    try:
        with open(serial_path, 'r', errors='replace') as f:
            f.seek(file_pos)
            data = f.read()
            if data:
                out_stream.write(data)
                out_stream.flush()
                return f.tell()
    except Exception:
        pass
    return file_pos


def main():
    ap = argparse.ArgumentParser(
        description="Boot a SWEETXNU payload in QEMU's virt board.",
        epilog="Examples:\n"
               "  qemu-sweet.py payload.bin out/\n"
               "  qemu-sweet.py payload.bin out/ --live-serial\n"
               "  qemu-sweet.py payload.bin out/ --kernel kernel.macho --fail-on-panic\n"
               "  qemu-sweet.py --check-env\n",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument('payload', nargs='?', help='SWEETXNU payload binary (from mkpayload.py)')
    ap.add_argument('outdir', nargs='?', help='Output directory for fb.png, regs.txt, and logs')
    ap.add_argument('--secs', type=float, default=20, help='Duration to run QEMU in seconds (default: 20)')
    ap.add_argument('--gdb', action='store_true', help='start halted, gdb stub on :1234, no timeout')
    ap.add_argument('--hmp', action='append', default=[], help='extra monitor command run after stop (repeatable), e.g. "x /8gx 0xfffffe..."')
    ap.add_argument('--cpu', default='cortex-a76', help='QEMU CPU model (default: cortex-a76)')
    ap.add_argument('--kernel', help='matching unstripped ARM64 Mach-O kernel, for exact framebuffer text decoding')
    ap.add_argument('--max-log-mib', type=int, default=64, help='stop if the QEMU debug log reaches this limit (default: 64 MiB)')
    ap.add_argument('--icount-shift', type=int, choices=range(11),
                    help='TCG instruction-clock experiment (0..10), not hardware timing or performance')
    ap.add_argument('--fail-on-panic', action='store_true', help='exit 2 for a visible panic, 3 for incomplete/blank decoding; requires --kernel')
    ap.add_argument('--live-serial', action='store_true', help='stream serial console output to stdout in real time')
    ap.add_argument('--check-env', action='store_true', help='check QEMU installation and dependencies without running a payload')
    a = ap.parse_args()

    if a.check_env:
        ok = check_qemu_dependency(a.cpu)
        return 0 if ok else 1

    if not a.payload or not a.outdir:
        ap.error('payload and outdir are required unless --check-env is specified')

    if a.fail_on_panic and not a.kernel:
        ap.error('--fail-on-panic requires --kernel')
    if not math.isfinite(a.secs) or a.secs <= 0 or a.max_log_mib <= 0:
        ap.error('--secs and --max-log-mib must be positive and finite')
    out = os.path.abspath(a.outdir)
    if os.name == 'nt' and not out.isascii():
        ap.error('Windows QEMU needs an ASCII output path, e.g. E:/ios-sweet-qemu-test')
    if os.path.exists(out) and (not os.path.isdir(out) or os.listdir(out)):
        ap.error('outdir must be new or empty to preserve previous results')

    if not check_qemu_dependency(a.cpu):
        return 1

    if a.kernel and not check_kernel_tools(a.kernel):
        return 1

    payload = open(a.payload, 'rb').read()
    font = None
    if a.kernel:
        try:
            address = kernel_font_address(open(a.kernel, 'rb').read())
            font = payload_data(payload, address, 256 * 16)
        except ValueError as error:
            ap.error(str(error))
    os.makedirs(a.outdir, exist_ok=True)

    phys_base, img, ba_pa, entry_pa = build_image(payload)
    open(f'{out}/ram.bin', 'wb').write(img)
    open(f'{out}/stub.bin', 'wb').write(stub(ba_pa, entry_pa))
    print(f'phys_base={phys_base:#x} boot_args={ba_pa:#x} entry={entry_pa:#x}', file=sys.stderr)

    if os.name == 'nt':
        # Windows builds need TCP QMP. Bind loopback only, never the LAN.
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            mon = reserve.getsockname()
        qmp_arg = f'tcp:{mon[0]}:{mon[1]},server=on,wait=off'
    else:
        mon = f'{out}/mon.sock'
        qmp_arg = f'unix:{mon},server=on,wait=off'
    # virt RAM starts at 0x40000000: cover the payload's RAM (mkadt.py --highmem needs 8G).
    mem_size = struct.unpack_from('<Q', payload, 0x40)[0]
    ram_gib = max(4, -(-(phys_base + mem_size - 0x40000000) // (1 << 30)))
    cmd = ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', a.cpu, '-smp', '1', '-m', f'{ram_gib}G',
           '-display', 'none', '-monitor', 'none', '-nic', 'none',
           '-serial', f'file:{out}/serial.txt', '-qmp', qmp_arg,
           '-d', 'int,guest_errors,unimp', '-D', f'{out}/qemu.log', '-no-reboot',
           '-device', f'loader,file={out}/ram.bin,addr={phys_base:#x},force-raw=on',
           '-device', f'loader,file={out}/stub.bin,addr={STUB_PA:#x},force-raw=on',
           '-device', f'loader,addr={STUB_PA:#x},cpu-num=0']
    if a.gdb:
        cmd += ['-S', '-gdb', 'tcp:127.0.0.1:1234']
    if a.icount_shift is not None:
        cmd += ['-accel', 'tcg,thread=single', '-icount',
                f'shift={a.icount_shift},align=off,sleep=off', '-rtc', 'clock=vm']
    with open(f'{out}/command.json', 'w', encoding='utf-8') as command_file:
        json.dump(cmd, command_file, indent=2)

    p = None
    serial_file = f'{out}/serial.txt'
    serial_pos = 0

    try:
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                             creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        if a.gdb:
            print('gdb stub on :1234; QMP socket:', mon, file=sys.stderr)
            p.wait()
            return 0

        t0 = time.monotonic()
        log_limited = False
        while p.poll() is None and time.monotonic() - t0 < a.secs:
            log_path = f'{out}/qemu.log'
            if os.path.exists(log_path) and os.path.getsize(log_path) >= a.max_log_mib << 20:
                log_limited = True
                break
            if a.live_serial:
                serial_pos = stream_serial_file(serial_file, serial_pos)
            time.sleep(0.2)

        if a.live_serial:
            stream_serial_file(serial_file, serial_pos)

        result = {'status': 'qemu_exited', 'qemu_returncode': p.returncode}
        if p.poll() is None:
            _, regs, *extra = qmp(mon, 'stop', 'info registers', *a.hmp, pmemsave=(FB_BASE, FB_STRIDE * FB_H, f'{out}/fb.raw'))
            open(f'{out}/regs.txt', 'w').write(regs + ''.join(f'\n(qemu) {c}\n{r}' for c, r in zip(a.hmp, extra)))
            p.kill()
            p.wait()
            raw = open(f'{out}/fb.raw', 'rb').read()
            write_png(f'{out}/fb.png', raw)
            result = {'status': 'not_decoded'}
            if font is not None:
                screen, unknown = decode_screen(raw, font)
                open(f'{out}/fb.txt', 'w').write(screen)
                result = screen_result(screen, unknown)
            os.unlink(f'{out}/fb.raw')
        else:
            print(f'qemu exited early: {p.returncode}', file=sys.stderr)

        result.update(payload_sha256=hashlib.sha256(payload).hexdigest(),
                      wall_seconds=time.monotonic() - t0, log_limit_reached=log_limited,
                      hardware_not_tested=True, icount_shift=a.icount_shift)
        if a.kernel:
            result['kernel_sha256'] = hashlib.sha256(open(a.kernel, 'rb').read()).hexdigest()
        open(f'{out}/result.json', 'w').write(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result), file=sys.stderr)
        print(f'{out}/fb.png  {out}/regs.txt  {out}/qemu.log', file=sys.stderr)
        if a.fail_on_panic and result['status'] == 'panic':
            return 2
        if a.fail_on_panic and result['status'] in ('incomplete_decode', 'blank_screen'):
            return 3
        if result['status'] == 'qemu_exited':
            return 1
        if log_limited:
            return 3
        return 0

    except KeyboardInterrupt:
        print('\n[!] Interrupted by user, stopping QEMU...', file=sys.stderr)
        if p and p.poll() is None:
            p.kill()
            p.wait()
        return 130
    finally:
        if p and p.poll() is None:
            p.kill()
            p.wait()
        ram_file = f'{out}/ram.bin'
        if os.path.exists(ram_file):
            try:
                os.unlink(ram_file)
            except Exception:
                pass


if __name__ == '__main__':
    sys.exit(main())
