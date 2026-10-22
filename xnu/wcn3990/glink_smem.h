/* SPDX-License-Identifier: MIT */
#ifndef SWEET_GLINK_SMEM_H
#define SWEET_GLINK_SMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWEET_GLINK_DESCRIPTOR_ITEM 478u
#define SWEET_GLINK_TX_ITEM 479u
#define SWEET_GLINK_RX_ITEM 480u
#define SWEET_GLINK_DESCRIPTOR_BYTES 32u

enum sweet_glink_error {
    SWEET_GLINK_OK = 0,
    SWEET_GLINK_ARGUMENT = -1,
    SWEET_GLINK_CORRUPT = -2,
    SWEET_GLINK_AGAIN = -3
};

/* One AP producer and one AP consumer; caller serializes each direction.
 * Mappings must remain valid throughout use and be shared, non-cacheable RAM
 * accessible to the peer. This API does not obtain SMEM items, map physical
 * memory, allocate remote heaps, boot firmware, or reset shared indices.
 * notify must ring the edge's actual mailbox; it runs after publishing TX.
 * Do not bind Android-owned SMEM from another running operating system.
 */
struct sweet_glink_smem {
    volatile uint32_t *descriptor;
    volatile uint8_t *tx;
    volatile uint8_t *rx;
    uint32_t tx_size;
    uint32_t rx_size;
    void (*notify)(void *context);
    void *context;
    bool space_requested;
};

/* On bind failure, *out and shared memory are unchanged. All mapped ranges
 * and *out must be disjoint. Descriptor is exactly 32 bytes, aligned to 4.
 * FIFOs have lengths divisible by 8, at least 32 bytes, at most UINT32_MAX.
 */
int sweet_glink_smem_bind(struct sweet_glink_smem *out,
    volatile uint32_t *descriptor, size_t descriptor_bytes,
    volatile void *tx, size_t tx_bytes, volatile void *rx, size_t rx_bytes,
    void (*notify)(void *context), void *context);

/* Normal TX always reserves 8 bytes to distinguish full/empty, plus 8 for a
 * READ_NOTIF command. Every frame advances by its size rounded up to 8.
 * AGAIN/corruption leaves shared memory unchanged. Caller data must not
 * alias shared mappings or the link state. TX frames include their GLINK
 * command header (at least 8 bytes); this layer does not parse commands.
 */
int sweet_glink_smem_tx_room(const struct sweet_glink_smem *link, size_t *bytes);
int sweet_glink_smem_send(struct sweet_glink_smem *link, const void *frame, size_t bytes);
/* Send command 13 from the extra reserve once while blocked. This asks the
 * peer to notify us when it consumes data. Caller still handles timeout/SSR.
 */
int sweet_glink_smem_request_space(struct sweet_glink_smem *link);

/* Peek never publishes consumption; only consume changes AP-owned RX tail.
 * Bounds errors leave destination/indices unchanged. Consume takes a padded
 * frame length divisible by 8. The native-command layer handles READ_NOTIF
 * by calling its mailbox notifier after consumption; consume itself doesn't.
 */
int sweet_glink_smem_rx_available(const struct sweet_glink_smem *link, size_t *bytes);
int sweet_glink_smem_peek(const struct sweet_glink_smem *link,
    size_t offset, void *out, size_t bytes);
int sweet_glink_smem_consume(struct sweet_glink_smem *link, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
