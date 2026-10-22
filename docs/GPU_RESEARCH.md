# Adreno 618: hardware and iOS integration evidence

## A concrete alternative to implementing Metal first

The [ios-sailfish fork](https://github.com/joranv1/ios-sailfish/tree/d7a74344828c87d37c64f87f237525cb65b7e5f5)
reports CoreAnimation GPU rendering on Pixel 1 / Adreno 530 through an EAGL
shim and Mesa freedreno. Its README explicitly excludes Metal applications
and still describes an experimental system. Those are the author's results,
not reproduced results for sweet.

Source inspection at that revision confirms more than an interface stub:

- `tools/sweetgl/eagl.c` intercepts EAGL context/dispatch behavior used by
  CoreAnimation's GLES renderer; it also contains image/shader workarounds.
- `tools/sweetgl/sweet_drm.c` implements the subset of the MSM/libdrm API
  used by freedreno over a `SweetGPU` IOKit user client. It allocates/imports
  buffer objects, submits command buffers and waits for fences.
- `tools/sweetgl/mesa.patch` connects BO mappings to that user client and
  permits building the freedreno DRM backend for Darwin.
- Kernel patches implement A530 power, SMMU, firmware, queues and presentation.

This changes the research target: accelerate CoreAnimation through GLES first,
while keeping Metal explicitly unsupported. It does not establish that the
same frontend works unchanged with the iPhone X userland used by sweet.

## Why the Pixel implementation cannot simply be enabled on sweet

The inspected libdrm emulation hardcodes `MSM_PARAM_GPU_ID` to **530**. The
Mesa patch includes an A5xx-specific linear-stride adjustment. Its kernel
register programming, clocks, power controller, microcode and presentation
path target MSM8996/A530. Changing that reported ID to 618 would not provide
an A618 driver.

The Mesa cross/build scripts also reference the author's private Xcode and
Mesa directories, and the Mesa source revision is not pinned there. No code
from this fork has been incorporated. Source license notices must be resolved
for any future reuse; currently it serves as an implementation reference.

## Actual sweet hardware map

Read from this handset's live FDT, not inferred from the Pixel:

| Resource | Live value |
| --- | --- |
| GPU chip ID | `0x06010800` (Adreno 618 revision encoding) |
| KGSL GPU registers | `0x05000000 / 0x40000` |
| GPU SMMU | `0x05040000 / 0x10000`, `qcom,kgsl-smmu-v2` |
| GPU clock controller | `0x05090000 / 0x9000`, `qcom,gpucc-sdmmagpie` |
| CX power domain | `0x0509106c / 4` |
| GX power domain | `0x0509100c / 4` |
| GMU registers | `0x0506a000 / 0x31000` |
| GMU PDC config / sequence | `0x0b290000 / 0x10000`, `0x0b490000 / 0x10000` |

These addresses alone do not establish that the blocks are powered or safe to
access. No GPU registers have been written or mapped by this workspace.

Hardware reference sources:

- [Sweet A6xx driver](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/gpu/msm/adreno_a6xx.c)
- [Sweet GMU driver](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/gpu/msm/adreno_a6xx_gmu.c)
- [Linux A6xx GMU implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/msm/adreno/a6xx_gmu.c)

## Implementation sequence and evidence required

1. Implement A618-specific GPUCC/GDSC and GMU power/firmware/HFI startup with
   bounded waits and rollback. Resolve exact microcode from the handset without
   committing firmware or device calibration.
2. Implement GPU/GMU SMMU context banks, owned buffer mappings, command/ring
   submission, interrupts and fences; prove a small GPU-rendered buffer on sweet.
3. Expose a real IOKit BO/submit/fence interface and connect Mesa's A6xx backend.
   Never report A530 merely to satisfy the existing frontend.
4. Build the iOS GLES/EAGL integration against pinned Mesa and an available
   Apple SDK; verify IOSurface import, stride, synchronization and compositing
   using the actual iPhone X userland.
5. Connect presentation to the sweet display path. Physical 120 Hz separately
   requires panel/DSI/DSC mode programming and device measurements.

Currently no A618 GPU driver or iOS GPU integration is implemented here.
