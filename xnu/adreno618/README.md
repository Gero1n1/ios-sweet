# A618 GPU driver components

`firmware.c` is the first hardware-facing A618 component. Patch 0086 stages it
into the SWEET XNU build. It has no startup caller and is not a working GPU
driver or a registered graphics service.

`a618_gmu_upload()` writes legacy flat GMU firmware into the CM3 ITCM window of
an already-powered, mapped GMU. Before any MMIO access it checks chip family,
mapping bounds, firmware length/alignment and disjoint memory ranges. It then
requires `SYSRESET` bit 0 to remain asserted, uploads at most 32 KiB, optionally
verifies every word, and leaves reset asserted. It never enables clocks, votes
power rails, releases reset or starts HFI. `dsb sy` completes ARM64 MMIO writes.

Caller responsibilities are substantial: valid Device mappings of the actual
GMU resource, enabled CX domain/clocks, serialized initialization, correct
firmware selection and a complete power/HFI startup/rollback sequence. Reading
an unpowered hardware block may fault; the reset check does not make that safe.
No caller should invoke this function until those prerequisites are implemented.

Offsets are relative to the live sweet GMU resource at `0x0506a000`:

| Register/window | Relative offset | Extent |
| --- | --- | --- |
| CM3 ITCM | `0x3000` | `0x8000` bytes |
| CM3 SYSRESET | `0x14000` | 4 bytes |

`a618_sqe_code()` returns the SQE code after the four-byte firmware file header.
It does not allocate a buffer, map it into the GPU, or configure the SQE engine.
The stripped word is reported as a raw file header, not an invented version.

Format/register facts were checked against the pinned sweet Android sources:

- [A618 firmware selection](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/gpu/msm/adreno-gpulist.h)
- [A615-family includes A618](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/gpu/msm/adreno.h)
- [Legacy GMU load](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/gpu/msm/adreno_a6xx_gmu.c)
- [GMU limit](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/gpu/msm/kgsl_gmu.h)
- [Register offsets](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/gpu/msm/a6xx_reg.h)
- [GMU/GPU offset conversion](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/gpu/msm/kgsl_gmu_core.c)
- [SQE header handling](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/gpu/msm/adreno_a6xx.c)

This is independent MIT code; Linux implementation code and firmware are not
included. Host tests execute the same code as C11/C++14 on inert register RAM,
checking ITCM-only writes, reset preservation, invalid inputs, wrong chips and
SQE slicing. The handset's actual 32,768-byte `a618_gmu.bin` and 32,136-byte
`a630_sqe.fw` were also tested locally against inert RAM and a bounded SQE view.
No microcode was executed and no handset GPU register was accessed.

Remaining: GPUCC/GDSC power and clocks, GMU HFI, TrustZone zap handling, SMMU,
owned BOs, CP rings/interrupts/fences, IOSurface and an iOS GLES frontend. See
[GPU research](../../docs/GPU_RESEARCH.md).
