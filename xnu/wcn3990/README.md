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
starts a radio or sends messages yet. Full XNU build/link with the codec passed
in [run 39768956686](https://github.com/Gero1n1/ios-sweet/actions/runs/39768956686);
the build log confirms `wlfw_wire.c` compilation. This is not a phone test.
The staged translation unit was cross-compiled to a Mach-O ARM64 object with
the actual XNU headers using Zig 0.17.0's Clang frontend (`KERNEL` and
`ARM64_BOARD_CONFIG_SWEET`, freestanding, no SDK includes). Its seven public
functions are present with no undefined symbols. That was a single-file check;
the later full build is recorded above.
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

## GLINK mapped-ring transport

`glink_smem.c` now moves frames through already-mapped shared-memory FIFOs.
Patch 0084 adds it to the SWEET kernel build. It binds the 32-byte descriptor
from SMEM item 478 and TX/RX items 479/480, with AP-owned TX head and RX tail;
it never resets the remote-owned indices. It implements:

- wrapped TX/RX copying and 8-byte frame alignment;
- 8-byte full/empty reserve plus 8 bytes for `READ_NOTIF` while blocked;
- publication of payload/head before a caller-supplied mailbox notification;
- separate RX peek and consume, malformed-index rejection and bounded access;
- outer-shareable ARM64 barriers (`dmb osh`) for the AP/DSP memory boundary.

The platform must supply shared non-cacheable mappings, SMEM allocation under
the correct remote lock, mailbox and IRQ handling, and restart/lifetime
synchronization. The implementation is not bound to physical hardware yet.
Native GLINK channel/intent negotiation, QRTR and remote-processor
startup remain missing. It cannot yet carry WLFW messages on the handset.
No caller has sent a command to Android's live firmware.

Protocol layout was checked against the pinned sweet
[GLINK SMEM transport](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/rpmsg/qcom_glink_smem.c).
The implementation is independent MIT code; Linux implementation code is not
incorporated. Host tests exercise the actual C/C++ transport using memory-backed
rings, including every aligned head/tail pair in a 64-byte FIFO, frame lengths
8–65, padding, saturation, notification reserve, corruption and callback order.
Those tests do not verify cache attributes or ordering against a real DSP.

`smem_view.c` (patch 0085) now reads existing uncached private-heap items from
an AP/remote partition of a primary SMEM v11/v12 mapping. It validates the
partition table, host pair, partition size/free offsets, entry canaries,
padding, item boundaries and duplicate items before returning a view.
`sweet_glink_smem_bind_existing()` resolves items 478–480 and binds those actual
views to the FIFO transport without resetting shared state. Tests exercise the
whole lookup/bind/send path on a synthetic SMEM heap, plus malformed metadata.

It does not allocate missing SMEM items, handle cached/global/auxiliary heaps,
or acquire the hardware allocation lock. NOT_FOUND means no matching uncached
item was found in the selected private partition, not that Android lacks SMEM
or the WLAN service. Platform mapping, restart exclusion and mailbox/IRQ binding
are still required before any on-device use.

Format reference: pinned sweet
[SMEM implementation](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/b2dab9baf54310ed2987689f8398a75836f3bd22/drivers/soc/qcom/smem.c).

## GLINK version negotiation

`glink_control.c` (patch 0087) connects GLINK v1 VERSION/ACK negotiation to the
actual FIFO transport. It advertises zero optional features: intent reuse and
intentless transfers must not be enabled before their implementations exist.
It handles version counter-offers, duplicate messages and READ_NOTIF, publishing
the RX tail before ringing the mailbox. Unknown channel commands remain in RX.
A blocked reply stays queued locally across retries; receiving it is never
mistaken for successful transmission. Each call performs bounded work.

Eight C11/C++14 test groups include two connected peers, exact wire vectors,
every aligned ring position, TX saturation and later recovery, version-zero
rejection, unsupported features, malformed indices and aliased state rejection.
These use shared arrays, not a DSP. Platform code still needs IRQ scheduling,
negotiation deadlines and restart detection; a VERSION packet cannot establish
whether a remote processor restarted. `ready` means only the local version offer
was acknowledged. No channel, intent, QRTR service or Wi-Fi interface is created.

Protocol facts were checked against the public
[GLINK native implementation, Linux v6.12](https://github.com/torvalds/linux/blob/v6.12/drivers/rpmsg/qcom_glink_native.c).
The new implementation is independent MIT code; Linux implementation code was
not incorporated.
