/* SPDX-License-Identifier: MIT
 * Independent reader for the Qualcomm SMEM private-heap wire layout.
 * Reference: pinned sweet drivers/soc/qcom/smem.c (format facts only).
 */
#if !defined(KERNEL) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "smem_view.h"

static void shared_barrier(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("dmb osh" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

/* All callers validate a 4-byte-aligned mapping and aligned field offsets. */
static uint32_t read32(volatile void *base, size_t offset)
{
    volatile uint32_t *words = (volatile uint32_t *)base;
    uint32_t value = words[offset / 4u];
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap32(value);
#endif
    return value;
}

static uint16_t read16(volatile uint8_t *base, size_t offset)
{
    return (uint16_t)((uint16_t)base[offset] | (uint16_t)((uint16_t)base[offset + 1u] << 8));
}

static bool magic(volatile uint8_t *base, const char *expected)
{
    size_t i;
    for (i = 0; i < 4u; ++i)
        if (base[i] != (uint8_t)expected[i])
            return false;
    return true;
}

int sweet_smem_find_uncached(volatile void *mapping, size_t bytes,
    uint16_t remote_host, uint16_t item, struct sweet_smem_view *out)
{
    volatile uint8_t *base = (volatile uint8_t *)mapping;
    volatile uint8_t *table, *partition = NULL;
    struct sweet_smem_view found;
    uintptr_t address = (uintptr_t)mapping, output = (uintptr_t)out;
    size_t table_offset, part_size = 0, position, uncached, cached;
    uint32_t version, count, index;
    bool have_item = false;
    if (!mapping || !out || (address & 3u) || bytes < 8192u ||
        (bytes & 4095u) || bytes > UINT32_MAX || bytes > UINTPTR_MAX - address ||
        sizeof(*out) > UINTPTR_MAX - output ||
        (output < address + bytes && address < output + sizeof(*out)) ||
        remote_host == 0u || remote_host >= 20u)
        return SWEET_SMEM_ARGUMENT;
    if (read32(base, 192u) != 1u || read32(base, 204u) != 0u)
        return SWEET_SMEM_FORMAT;
    version = read32(base, 92u); /* proc_comm[4] + version[SBL=7] */
    if (version != 11u && version != 12u)
        return SWEET_SMEM_UNSUPPORTED;
    shared_barrier();
    table_offset = bytes - 4096u;
    table = base + table_offset;
    if (!magic(table, "$TOC") || read32(table, 4u) != 1u)
        return SWEET_SMEM_UNSUPPORTED;
    count = read32(table, 8u);
    if (count > (4096u - 32u) / 48u)
        return SWEET_SMEM_FORMAT;
    for (index = 0; index < count; ++index) {
        size_t entry = 32u + (size_t)index * 48u;
        uint16_t h0 = read16(table, entry + 12u), h1 = read16(table, entry + 14u);
        if ((h0 == 0u && h1 == remote_host) || (h1 == 0u && h0 == remote_host)) {
            size_t offset = read32(table, entry), size = read32(table, entry + 4u);
            if (partition || offset < 208u || (offset & 7u) || size < 32u || (size & 7u) ||
                offset > table_offset || size > table_offset - offset)
                return SWEET_SMEM_FORMAT;
            partition = base + offset;
            part_size = size;
            if (!magic(partition, "$PRT") || read16(partition, 4u) != h0 ||
                read16(partition, 6u) != h1 || read32(partition, 8u) != size)
                return SWEET_SMEM_FORMAT;
        }
    }
    if (!partition)
        return SWEET_SMEM_NOT_FOUND;
    uncached = read32(partition, 12u);
    cached = read32(partition, 16u);
    /* The allocator publishes free offsets after completing item headers. */
    shared_barrier();
    if (uncached < 32u || uncached > part_size || cached < uncached ||
        cached > part_size || (uncached & 7u) || (cached & 7u))
        return SWEET_SMEM_FORMAT;
    position = 32u;
    while (position < uncached) {
        size_t size, header_pad, data_pad, data_offset;
        if (uncached - position < 16u || read16(partition, position) != 0xa5a5u)
            return SWEET_SMEM_FORMAT;
        size = read32(partition, position + 4u);
        data_pad = read16(partition, position + 8u);
        header_pad = read16(partition, position + 10u);
        if ((size & 7u) || (header_pad & 7u) || data_pad > size ||
            header_pad > uncached - position - 16u)
            return SWEET_SMEM_FORMAT;
        data_offset = position + 16u + header_pad;
        if (size > uncached - data_offset)
            return SWEET_SMEM_FORMAT;
        if (read16(partition, position + 2u) == item) {
            if (have_item)
                return SWEET_SMEM_FORMAT;
            found.data = partition + data_offset;
            found.size = size - data_pad;
            have_item = true;
        }
        position = data_offset + size;
    }
    if (!have_item)
        return SWEET_SMEM_NOT_FOUND;
    *out = found;
    return SWEET_GLINK_OK;
}

int sweet_glink_smem_bind_existing(struct sweet_glink_smem *out,
    volatile void *mapping, size_t bytes, uint16_t remote_host,
    void (*notify)(void *context), void *context)
{
    struct sweet_smem_view descriptor, tx, rx;
    uintptr_t address = (uintptr_t)mapping, output = (uintptr_t)out;
    int result;
    /* Lookup writes only stack views; bind output must also stay out of SMEM. */
    if (!out || bytes > UINTPTR_MAX - address || sizeof(*out) > UINTPTR_MAX - output ||
        (output < address + bytes && address < output + sizeof(*out)))
        return SWEET_SMEM_ARGUMENT;
    result = sweet_smem_find_uncached(mapping, bytes, remote_host,
        SWEET_GLINK_DESCRIPTOR_ITEM, &descriptor);
    if (result != SWEET_GLINK_OK)
        return result;
    result = sweet_smem_find_uncached(mapping, bytes, remote_host, SWEET_GLINK_TX_ITEM, &tx);
    if (result != SWEET_GLINK_OK)
        return result;
    result = sweet_smem_find_uncached(mapping, bytes, remote_host, SWEET_GLINK_RX_ITEM, &rx);
    if (result != SWEET_GLINK_OK)
        return result;
    return sweet_glink_smem_bind(out, (volatile uint32_t *)(volatile void *)descriptor.data, descriptor.size,
        tx.data, tx.size, rx.data, rx.size, notify, context);
}
#endif
