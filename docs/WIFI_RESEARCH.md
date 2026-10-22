# Wi-Fi research for sweet

## Device facts

The user's Redmi Note 10 Pro reports Android 13, SoC SM7150 / Snapdragon 732G, WLAN interfaces `wlan0` and `wlan1`, and platform device `18800000.qcom,icnss`. The WLAN interface is attached to the Linux `icnss` platform driver. The device tree also exposes `slim@62e40000/wcn3990` with `qcom,btfmslim_slave`; that node describes the Bluetooth Slimbus function, not the Wi-Fi host interface.

The detected radio is Qualcomm WCN3990. The radio family supports dual-band 802.11ac, including 5 GHz. Actual usable channels depend on the board data and regulatory configuration. The phone's vendor image contains `wlanmdsp.mbn`, `WCNSS_qcom_cfg.ini`, and many `bdwlan.*` board-data files. These calibration/board files must remain device-local; do not put them in source control.

## A useful same-radio bring-up reference

A public mainline Linux bring-up for Xiaomi Redmi Note 8 (`ginkgo`, SM6125) reports WCN3990 Wi-Fi associated to a 5 GHz VHT80 access point at channel 36, 5180 MHz, with DHCP and a measured 292.5 Mbit/s VHT rate:

- Project: <https://github.com/Huabin1010/ginkgo-mainline-linux>
- Detailed Wi-Fi report: <https://github.com/Huabin1010/ginkgo-mainline-linux/blob/main/docs/ginkgo-wifi-complete-2026-08-18.md>
- Firmware extraction notes: <https://github.com/Huabin1010/ginkgo-mainline-linux/blob/main/firmware/ginkgo/README.md>
- Mainline Linux driver's SNOC path: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/wireless/ath/ath10k/snoc.c?h=v7.0>

That report uses the Linux `ath10k_snoc` driver and the device's `WLAN.HL.3.0.2-00656` firmware. It identifies several important firmware-start stages: MPSS/remoteproc and QRTR/WLFW, a 1 MiB MSA with correct hypervisor assignment, matching board data, QMI calibration download/report semantics, and WCN3990-specific WMI behavior. It also documents a successful 5 GHz VHT80 association.

This is strong evidence that the same WCN3990 family can use 5 GHz with Linux. It is not a drop-in driver for `sweet`: the reference is a different phone/SoC, the device-tree memory and power resources differ, and the host driver is Linux-specific. The Redmi Note 10 Pro must use its own live device tree and board data.

## What ios-sweet still needs

The current project is XNU-based and contains no WCN3990 or Wi-Fi implementation. Linux `ath10k_snoc` cannot be loaded into XNU. A working port must implement the radio's power/firmware/QMI/transport path in XNU and provide the driver/control interface expected by iOS Wi-Fi services, then prove scanning, association, DHCP, and real traffic on both 2.4 and 5 GHz.

Apple's IO80211 implementation is not present in the public XNU source checked for this project. The public macOS reverse-engineering work is a possible source of interface clues, but targets newer macOS and does not establish iOS 16 compatibility:

- <https://github.com/crangel660/macOS-80211-WiFi-research>
- <https://github.com/al3xtjames/IO80211Family-Headers>
- <https://github.com/OpenIntelWireless/itlwm> (GPLv2 macOS driver; useful evidence of the IO80211Controller interface shape, not code to copy into ios-sweet)

The community macOS driver subclasses `IO80211Controller` and provides an `IO80211Interface`, Apple 802.11 requests, DMA stop/queue management, country-code and feature handling. That establishes a possible macOS driver integration model. Its current headers and implementation target macOS and do not prove the same ABI or class set exists in iOS 16. The driver source is GPLv2 and is not being incorporated into this project.

Do not claim Wi-Fi support from a registry-only IOKit object or a visible `en0`/`wlan0` name. The radio must boot, scan, authenticate, associate, obtain an address, and pass traffic.

## Current build constraint

As of 2026-10-22, local GCC/Bash host checks work and GitHub CLI authentication is available for the user's fork. The previous environment's HTTP 401 no longer describes this workspace. Full XNU build/link succeeded in run `39768956686`, and its generated build tree is available locally. Sweet boot verification remains pending. No device partition has been changed.

## Implemented protocol milestone (2026-10-22)

[`xnu/wcn3990`](../xnu/wcn3990/README.md) contains an independent C WLFW/QMI wire
codec, staged into XNU by patch 0083. It encodes the initial registration and
MSA requests, parses replies, and validates firmware-proposed regions against
the caller's live-DT MSA reservation. Host tests compile and execute the actual
code as C and C++, including malformed packets and memory-boundary cases.
The driver does not call it to start the radio yet. Patch 0084 now adds an
independent GLINK mapped-ring transport: wrapped FIFO I/O, reserved notification
space, bounded indices and outer-shareable barriers. Its C/C++ tests use memory
backed rings, not the phone. Physical SMEM allocation/mapping, mailbox/IRQ binding,
native GLINK channel/intent negotiation, QRTR, power,
hypervisor assignment, firmware/board-data loading, CE/WMI/HTT and the iOS ABI
remain implementation work. No initialization request was sent to the Android
service, which already owns the radio.

## Live Android band report from sweet

The user's `dumpsys wifi` output reports 2.4 GHz channels 1–13 and 5 GHz channels 36, 40, 44, 48, 52, 56, 60, and 64; 6 GHz/60 GHz channel lists are empty. Android reports driver country code `AZ`. This confirms the currently running Android Wi-Fi stack advertises 5 GHz on this exact handset. It does not confirm which firmware build string is embedded in `wlanmdsp.mbn`, nor does it establish channel availability under iOS until its regulatory path is implemented.

### Rooted live inventory over Termux SSH (2026-10-22)

A read-only script was run through SSH as Termux with KernelSU root on the user's still-stock MIUI device. This verifies the inventory against the running handset without installing or booting ios-sweet:

- Android reports `wlan0`, `wlan1`, and `p2p0` on `phy0`; all three resolve to the ICNSS platform driver at `/sys/devices/platform/soc/18800000.qcom,icnss`. `wlan0` was up and associated at capture time.
- `iw phy` listed 2.4 GHz channels 1–13; channel 14 was disabled. It listed 5 GHz channels 36–64, with 52–64 marked for radar detection; channels 100–173 were disabled in the active regulatory state. This is the Android driver's current channel table, not a promise of iOS regulatory behavior or a test of every channel.
- Read-only filename listing confirmed `/vendor/firmware_mnt/image/wlanmdsp.mbn`, `bdwlan.bin`, and multiple `bdwlan.*` files. File contents and calibration values were not read or copied. No exact firmware release was extracted from the binary.
- The live tree contains `/soc/qcom,icnss@18800000`, `/soc/slim@62e40000/wcn3990`, `/soc/bt_wcn3990`, and `/reserved-memory/wlan_msa_region@97500000`. The ICNSS path and WLAN MSA facts corroborate the earlier FDT inventory.
- All eight Android CPUs (0–7) were online; `MemTotal` was 5,635,476 kB. These Android resource figures do not establish the memory map or CPU support available to XNU.
- `ro.build.version.release` returned `16`, while the build fingerprint and user-reported ROM identify Android 13 (`...sweet:13/...`). Preserve this property inconsistency as an observation; do not revise the ROM identification on that basis alone.
- The filtered root kernel-message query returned no matching ICNSS/WCN/QMI lines during this read. That absence does not establish firmware or driver failure.

## Sweet ICNSS resources in the public device tree

The public ArrowOS sweet kernel device tree gives a more useful hardware map than the generic `qcom,icnss` compatible string. In `arch/arm64/boot/dts/qcom/atoll.dtsi`, ICNSS has an 8 MiB register window at `0x18800000`, SMMU IOVA windows at `0xa0000000` and `0xb0000000`, and twelve Copy Engine interrupts (CE0–CE11). It uses the WLAN fixed MSA region, SMMU stream `0xC0`, and five supplies. The Xiaomi common overlay changes the WLAN MSA allocation to base `0x97d00000`, size `0x180000` (1.5 MiB). The matching source locations are:

- [Atoll ICNSS node](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/arrow-13.1/arch/arm64/boot/dts/qcom/atoll.dtsi#L2973-L3008)
- [Sweet WLAN MSA reservation](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/arrow-13.1/arch/arm64/boot/dts/qcom/xiaomi/xiaomi-sdmmagpie-common.dtsi#L315-L322)

The user's active FDT confirms three `reg` ranges: `0x18800000` / `0x800000`, `0xa0000000` / `0x10000000`, and `0xb0000000` / `0x10000`. It reports twelve interrupt entries with numbers 414–425, and an `iommus` property encoded as cells `0x30, 0x240, 1`. The fixed-region phandle points to `/reserved-memory/wlan_msa_region@97500000`, whose actual `reg` is `0x97d00000` / `0x180000` (the node name's unit address differs from the `reg` value). The live ICNSS node exposes supplies for `vdd-cx-mx`, `vdd-1.8-xo`, `vdd-1.3-rfa`, and `vdd-3.3-ch0`; the queried `vdd-3.3-ch1` property and `qcom,hyp_disabled` are absent.

There are real differences from the public DTS: the public `iommus` tuple uses `0xC0` where the active FDT has `0x240`; the public source declares level-high IRQ flags while the active FDT's third cell for each interrupt is zero; and the public source has a `vdd-3.3-ch1-supply` absent from the active node. The user resolved IOMMU phandle `0x30` to `/soc/apps-smmu@0x15000000`, compatible `qcom,qsmmu-v500`, with `#iommu-cells = 2`; the raw ICNSS `iommus` tuple is `0x30, 0x240, 1`. Root interrupt-parent phandle `0x1` resolves to `/soc/interrupt-controller@b220000`, compatible `qcom,pdc-sdmmagpie`, with `#interrupt-cells = 3`. The PDC's parent phandle `0x1a` resolves to `/soc/interrupt-controller@17a00000`, compatible `arm,gic-v3`, with `#interrupt-cells = 3`. Thus each live ICNSS interrupt entry is a three-cell tuple, with middle cells 414–425 and outer cells zero. The public Atoll DTS uses a similar PDC controller with compatible `qcom,pdc-atoll` and also sets it as the root interrupt parent, though it lists level-high IRQ flags for ICNSS. The active reserved-memory phandle for WLAN MSA is `0xb7`, resolving to `/reserved-memory/wlan_msa_region@97500000` with `reg = 0x97d00000/0x180000`. The live FDT, not the public branch, must be the source of truth for any hardware implementation.

These resources make the first hardware milestone concrete: a native XNU implementation would need the right MMIO mapping, CE interrupt handling, DMA/IOMMU setup, rail control, and a way to load and communicate with the firmware using the assigned WLAN memory. Merely copying a Linux `ath10k_snoc` network interface would skip those services and cannot run under XNU.

## Linux ICNSS driver sequence (reference only)

The public sweet branch's [ICNSS host driver](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/arrow-13.1/drivers/soc/qcom/icnss.c) confirms the hardware-facing sequence. Its five voltage rails are all optional in the referenced driver, so the live absence of `vdd-3.3-ch1-supply` alone does not explain Wi-Fi failure. It maps the 8 MiB `membase`, records CE IRQ resources, maps the fixed MSA region, and reads the SMMU IOVA windows. With `qcom,hyp_disabled` absent, the zero-initialized flag remains false and Linux uses `hyp_assign_phys()` to assign MSA access among HLOS and WLAN/MSS/CE VMIDs before firmware startup.

The paired [ICNSS QMI implementation](https://github.com/ArrowOS-Devices/android_kernel_xiaomi_sweet/blob/arrow-13.1/drivers/soc/qcom/icnss_qmi.c) connects to the WLFW remote service over QRTR/QMI, exchanges capability and firmware-version messages, provides MSA information, handles MSA-ready/FW-ready indications, and sends WLAN mode/configuration requests. The QCA CLD WLAN stack then handles WMI/HTT over Copy Engines. These sources rely on Linux regulator, DMA/IOMMU, IRQ, QRTR/QMI, remote-processor and hypervisor APIs. They document required behavior but cannot be compiled as XNU code.

The consolidated live inventory also confirms that Android binds `wlan0` to `icnss`; it lists `wlanmdsp.mbn`, `WCNSS_qcom_cfg.ini`, and local `bdwlan.*` files. The available exact WCN/ICNSS kernel-log filter contains only `cnss-daemon: Invalid mac address` / `failed to update gateway` messages, not firmware startup logs. Do not infer a missing firmware build from the unsuccessful string search in `wlanmdsp.mbn`; its binary firmware version remains unknown.
