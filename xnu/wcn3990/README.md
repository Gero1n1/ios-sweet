# WCN3990 protocol components

`wlfw_wire.c` implements the initial QMI service-message codec for the WLAN
firmware service (service `0x45`, version 1). It has no OS dependencies,
allocation, packed-struct casts, MMIO access, or firmware side effects.

Implemented messages:

- Indication registration (`0x20`): firmware-ready, MSA-ready,
  pin-connect-result and host client ID; decode common result and firmware status.
- MSA information (`0x2d`): encode the address/size supplied by the caller;
  decode up to two firmware-proposed memory regions.
- Empty capability (`0x24`) and MSA-ready (`0x2e`) requests; decode common result.

Response parsing checks the complete QMI message length, transaction and message
IDs, result TLV, field sizes, array counts, duplicate known TLVs, and unknown
mandatory fields. Unknown optional fields are skipped. Parser success only means
a structurally valid reply; the caller must also check the firmware result.

`wlfw_msa_regions_safe()` validates a successful reply against a caller-supplied
live-DT reservation. It rejects empty, zero-size, overlapping, overflowing or
out-of-reservation regions. It does not map memory or change VMID permissions.
The caller must complete the correct hypervisor assignment before sending
MSA-ready, and handle cleanup if subsequent initialization fails.

Patch 0083 adds the C file to the XNU build, and `0083-stage-wlfw.sh` copies these
canonical sources into `bsd/dev/sweet_wcn3990` during CI staging. The C code is
enabled for the SWEET kernel only (and for standalone host tests). No caller
starts a radio or sends messages yet. Full XNU compilation is still unverified.
The staged translation unit was cross-compiled to a Mach-O ARM64 object with
the actual XNU headers using Zig 0.17.0's Clang frontend (`KERNEL` and
`ARM64_BOARD_CONFIG_SWEET`, freestanding, no SDK includes). Its seven public
functions are present with no undefined symbols. This is a single-file check,
not a full XNU build or device test.
Host compilation and schema fixtures do not test a radio, firmware boot, QRTR
transport or Apple's Wi-Fi ABI. The
remaining driver needs power/remoteproc, SMEM/GLINK or the applicable IPC
transport, QRTR service discovery and transaction lifecycle, VMID assignment,
board-data transfer, CE/WMI/HTT data paths, DMA/SMMU, interrupts, and iOS network
integration. Do not send these initialization requests to Android's already
running WLAN service: Android owns that hardware and memory.

The wire definitions were checked against the public sweet kernel at commit
`b2dab9baf54310ed2987689f8398a75836f3bd22`:

- [QMI service header and message types](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/include/linux/soc/qcom/qmi.h)
- [WLFW schema](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/soc/qcom/wlan_firmware_service_v01.c)
- [ICNSS initialization requests and MSA bounds](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/soc/qcom/icnss_qmi.c)

This is an independent implementation of those wire formats; Linux driver code
is not included. Tests compile the actual implementation as C11 and C++14,
check fixed schema-derived byte vectors, exhaust 16-bit length fields, and test
truncation, duplicate fields, correlation, memory bounds and overlap:

```sh
python3 -m unittest discover -s tests -v
```

This is a codec milestone, not working Wi-Fi support.
