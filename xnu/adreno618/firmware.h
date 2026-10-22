/* SPDX-License-Identifier: MIT */
#ifndef SWEET_ADRENO618_FIRMWARE_H
#define SWEET_ADRENO618_FIRMWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A618_GMU_ITCM_OFFSET 0x3000u
#define A618_GMU_ITCM_BYTES 0x8000u
#define A618_GMU_SYSRESET_OFFSET 0x14000u

enum a618_firmware_error {
    A618_FIRMWARE_OK = 0,
    A618_FIRMWARE_ARGUMENT = -1,
    A618_FIRMWARE_RESET = -2,
    A618_FIRMWARE_VERIFY = -3
};

struct a618_sqe_view {
    const uint8_t *code;
    size_t bytes;
    uint32_t file_header; /* Raw first word; do not label it a version. */
};

/* Upload A618's legacy flat GMU firmware into CM3 ITCM, while reset is held.
 * gmu maps the GMU resource (sweet FDT base 0x0506a000), NOT the GPU resource.
 * Caller must already enable the correct rails/clocks, map accessible Device
 * memory and serialize initialization. A register read cannot detect an
 * inaccessible/unpowered mapping without a possible hardware fault.
 * Validates all input bounds before MMIO; requires chip ID 0x060108xx and
 * SYSRESET bit 0 set. Does not change reset, start GMU, or initialize HFI/SMMU.
 * If readback/reset verification fails, ITCM may already have been written;
 * caller must keep reset asserted and abort startup. No automatic boot hook.
 */
int a618_gmu_upload(volatile uint32_t *gmu, size_t mapped_bytes,
    uint32_t chip_id, const void *firmware, size_t firmware_bytes, bool verify);

/* SQE firmware has a four-byte file header; code is copied into a separately
 * owned/mapped GPU buffer by the future submission driver. This function
 * returns a bounded view only, and changes neither firmware nor hardware.
 * *out must be disjoint from firmware; on error *out is unchanged.
 */
int a618_sqe_code(const void *firmware, size_t bytes, struct a618_sqe_view *out);

#ifdef __cplusplus
}
#endif
#endif
