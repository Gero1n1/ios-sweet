# Small kernel test on Windows

This test runs XNU on QEMU `virt`, with a static test program as PID 1.
It does not run Apple launchd, SpringBoard or the complete iOS filesystem.
QEMU `virt` does not model sweet's WCN3990, A618, UFS, touch or DSI panel.
It cannot establish that those drivers work or that flashing is appropriate.

Use a Windows `qemu-system-aarch64` build in PATH, Python 3 and an unstripped
SWEET kernel from this project's CI. For the small PID 1, assemble
`tools/sweet-init.S` with an ARM64 iOS compiler. For example, with Zig:

```powershell
zig cc -target aarch64-ios.16.0 -g0 -c tools/sweet-init.S -o research/sweet-init.o
python tools/mkinit.py research/sweet-init.o research/sweet-init
python tools/mkadt.py research/qemu.adt --qemu
python tools/mkpayload.py xnu/out/kernel.development.sweet research/qemu.adt research/qemu-payload.bin --ramdisk research/sweet-init --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0 sweetusb=0"
python tools/qemu-sweet.py research/qemu-payload.bin E:/ios-sweet-qemu-test --kernel xnu/out/kernel.development.sweet --secs 90 --fail-on-panic
```

Create `research/` first if it does not exist. PowerShell may require quoting
paths containing spaces. Output must be a **new or empty ASCII directory**:
this Windows QEMU build cannot open Cyrillic file paths. The repository and
input paths may remain in their existing location. The font is read directly
from the matching Mach-O; external LLVM tools are unnecessary.

The QEMU guest reserves 4 GiB RAM for this layout; the generated RAM-loading
file is about 39 MB, not a 4 GiB disk image. Debug logs stop the test at 64 MiB
by default (`--max-log-mib` changes the cap). A captured framebuffer, decoded
text, CPU registers, exact command and result JSON remain in the output
directory. Only this invocation's generated `ram.bin` is removed on exit.
No phone connection or full IPSW download is required for this test.

## Reading results

- `panic` is a failure; keep `fb.txt`, `qemu.log` and `regs.txt`.
- `no_panic_observed` means only that the final circular-screen snapshot has
  no visible panic. It is not a complete boot log or a stability verdict.
- Look separately for `init running as PID 1` and subsequent `still alive`
  messages to establish execution of the minimal test program.
- Blank or undecodable snapshots fail with `--fail-on-panic`; reaching the
  debug-log cap also returns a failure code.
- Keep kernel/payload SHA-256 and clock mode from `result.json`. Different
  clock modes are different experiments and must be reported separately.

An initial 90-second run on Windows 10 reached PID 1; another run of the same
kernel/payload panicked in the interrupt-disabled watchdog (43.5 ms measured,
13.6 ms limit). This has **not** established a stable emulator boot. Host
descheduling/TCG clock behavior is a hypothesis, not a confirmed cause.

For an explicit instruction-clock experiment, append `--icount-shift 0`.
This selects single-thread TCG, one virtual nanosecond per guest instruction,
and jumps virtual time to the next timer deadline when the guest sleeps.
It changes neither the kernel's watchdog nor the phone boot arguments.
Its elapsed guest time is not wall time, and it cannot measure real CPU speed,
interrupt latency or display refresh. See the QEMU documentation for
[instruction counting](https://www.qemu.org/docs/master/devel/tcg-icount.html)
and the [clock options](https://www.qemu.org/docs/master/system/invocation.html).

The same 0086 kernel/payload ran for another 91.03 host seconds with shift 0:
the final screen contained 73 test-program `still alive` lines, zero unknown
glyphs and no visible panic; the log cap was not reached. This supports using
the instruction clock for further kernel experiments. It does not resolve
the default-clock failure or prove the host scheduling hypothesis.
