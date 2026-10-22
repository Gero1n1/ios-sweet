/* SPDX-License-Identifier: MIT */
#ifndef SWEET_SMEM_VIEW_H
#define SWEET_SMEM_VIEW_H

#include "glink_smem.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sweet_smem_error {
    SWEET_SMEM_ARGUMENT = -10,
    SWEET_SMEM_FORMAT = -11,
    SWEET_SMEM_NOT_FOUND = -12,
    SWEET_SMEM_UNSUPPORTED = -13
};

struct sweet_smem_view {
    volatile uint8_t *data;
    size_t size;
};

/* Read existing uncached items in an AP(0)/remote private partition of a
 * v11/v12 primary SMEM region. Mapping must be aligned, shared non-cacheable,
 * remain valid, and exclude restart while used. Does not allocate/lock/write
 * shared memory. Cached items and global/auxiliary heaps aren't supported.
 * On failure *out is unchanged. *out must be outside the SMEM mapping.
 */
int sweet_smem_find_uncached(volatile void *mapping, size_t bytes,
    uint16_t remote_host, uint16_t item, struct sweet_smem_view *out);

/* Connect the actual descriptor/FIFO views to the ring transport without
 * resetting any indices. All three items must already exist. Returns either
 * a SMEM error, GLINK error, or zero. Caller supplies the physical mapping and
 * real mailbox callback; this function doesn't initialize a remote processor.
 */
int sweet_glink_smem_bind_existing(struct sweet_glink_smem *out,
    volatile void *mapping, size_t bytes, uint16_t remote_host,
    void (*notify)(void *context), void *context);

#ifdef __cplusplus
}
#endif
#endif
