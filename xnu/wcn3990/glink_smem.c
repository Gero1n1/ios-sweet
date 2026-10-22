/* SPDX-License-Identifier: MIT
 * Independent implementation of Qualcomm's GLINK SMEM FIFO layout.
 * Protocol facts: pinned sweet drivers/rpmsg/qcom_glink_smem.c; see README.
 */
#if !defined(KERNEL) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "glink_smem.h"

static void shared_barrier(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    /* Remote DSPs are outside the AP's inner-shareable domain. */
    __asm__ volatile("dmb osh" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static uint32_t little32(uint32_t value)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

static bool disjoint(const volatile void *a, size_t na,
    const volatile void *b, size_t nb)
{
    uintptr_t aa = (uintptr_t)a, bb = (uintptr_t)b;
    if (na > UINTPTR_MAX - aa || nb > UINTPTR_MAX - bb)
        return false;
    return aa + na <= bb || bb + nb <= aa;
}

static bool valid(const struct sweet_glink_smem *link)
{
    return link && link->descriptor && link->tx && link->rx && link->notify &&
        link->tx_size >= 32u && link->rx_size >= 32u &&
        !(link->tx_size & 7u) && !(link->rx_size & 7u) &&
        !((uintptr_t)link->descriptor & 3u);
}

static int indices(const struct sweet_glink_smem *link, bool transmit,
    uint32_t *head, uint32_t *tail)
{
    uint32_t size, h, t;
    if (!valid(link))
        return SWEET_GLINK_ARGUMENT;
    size = transmit ? link->tx_size : link->rx_size;
    t = little32(link->descriptor[transmit ? 0 : 2]);
    h = little32(link->descriptor[transmit ? 1 : 3]);
    shared_barrier();
    if (h >= size || t >= size || (h & 7u) || (t & 7u))
        return SWEET_GLINK_CORRUPT;
    *head = h;
    *tail = t;
    return SWEET_GLINK_OK;
}

int sweet_glink_smem_bind(struct sweet_glink_smem *out,
    volatile uint32_t *descriptor, size_t descriptor_bytes,
    volatile void *tx, size_t tx_bytes, volatile void *rx, size_t rx_bytes,
    void (*notify)(void *context), void *context)
{
    struct sweet_glink_smem candidate;
    uint32_t h, t;
    int result;
    if (!out || !descriptor || !tx || !rx || !notify ||
        descriptor_bytes != SWEET_GLINK_DESCRIPTOR_BYTES ||
        ((uintptr_t)descriptor & 3u) ||
        tx_bytes < 32u || rx_bytes < 32u || tx_bytes > UINT32_MAX || rx_bytes > UINT32_MAX ||
        (tx_bytes & 7u) || (rx_bytes & 7u) ||
        !disjoint(descriptor, descriptor_bytes, tx, tx_bytes) ||
        !disjoint(descriptor, descriptor_bytes, rx, rx_bytes) ||
        !disjoint(tx, tx_bytes, rx, rx_bytes) ||
        !disjoint(out, sizeof(*out), descriptor, descriptor_bytes) ||
        !disjoint(out, sizeof(*out), tx, tx_bytes) ||
        !disjoint(out, sizeof(*out), rx, rx_bytes))
        return SWEET_GLINK_ARGUMENT;
    candidate.descriptor = descriptor;
    candidate.tx = (volatile uint8_t *)tx;
    candidate.rx = (volatile uint8_t *)rx;
    candidate.tx_size = (uint32_t)tx_bytes;
    candidate.rx_size = (uint32_t)rx_bytes;
    candidate.notify = notify;
    candidate.context = context;
    candidate.space_requested = false;
    result = indices(&candidate, true, &h, &t);
    if (result != SWEET_GLINK_OK)
        return result;
    result = indices(&candidate, false, &h, &t);
    if (result != SWEET_GLINK_OK)
        return result;
    *out = candidate;
    return SWEET_GLINK_OK;
}

static uint32_t unused(uint32_t size, uint32_t head, uint32_t tail)
{
    return tail > head ? tail - head : size - head + tail;
}

int sweet_glink_smem_tx_room(const struct sweet_glink_smem *link, size_t *bytes)
{
    uint32_t head, tail, free_bytes;
    int result;
    if (!bytes)
        return SWEET_GLINK_ARGUMENT;
    result = indices(link, true, &head, &tail);
    if (result != SWEET_GLINK_OK)
        return result;
    free_bytes = unused(link->tx_size, head, tail);
    *bytes = free_bytes > 16u ? free_bytes - 16u : 0u;
    return SWEET_GLINK_OK;
}

static int send_frame(struct sweet_glink_smem *link, const void *frame,
    size_t bytes, uint32_t reserve)
{
    const uint8_t *source = (const uint8_t *)frame;
    uint32_t head, tail, position, free_bytes;
    size_t padded, i;
    int result;
    if (!frame || bytes < 8u || bytes > UINT32_MAX - 7u)
        return SWEET_GLINK_ARGUMENT;
    result = indices(link, true, &head, &tail);
    if (result != SWEET_GLINK_OK)
        return result;
    padded = (bytes + 7u) & ~(size_t)7u;
    free_bytes = unused(link->tx_size, head, tail);
    if (free_bytes < reserve || padded > free_bytes - reserve)
        return SWEET_GLINK_AGAIN;
    position = head;
    for (i = 0; i < padded; ++i) {
        link->tx[position] = i < bytes ? source[i] : 0u;
        if (++position == link->tx_size)
            position = 0;
    }
    shared_barrier();
    link->descriptor[1] = little32(position);
    /* Finish the shared head publication before ringing the MMIO mailbox. */
    shared_barrier();
    link->notify(link->context);
    return SWEET_GLINK_OK;
}

int sweet_glink_smem_send(struct sweet_glink_smem *link, const void *frame, size_t bytes)
{
    int result = send_frame(link, frame, bytes, 16u);
    if (result == SWEET_GLINK_OK)
        link->space_requested = false;
    return result;
}

int sweet_glink_smem_request_space(struct sweet_glink_smem *link)
{
    static const uint8_t command[8] = {13u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    int result;
    if (!valid(link))
        return SWEET_GLINK_ARGUMENT;
    if (link->space_requested)
        return SWEET_GLINK_OK;
    result = send_frame(link, command, sizeof(command), 8u);
    if (result == SWEET_GLINK_OK)
        link->space_requested = true;
    return result;
}

int sweet_glink_smem_rx_available(const struct sweet_glink_smem *link, size_t *bytes)
{
    uint32_t head, tail;
    int result;
    if (!bytes)
        return SWEET_GLINK_ARGUMENT;
    result = indices(link, false, &head, &tail);
    if (result != SWEET_GLINK_OK)
        return result;
    *bytes = head >= tail ? head - tail : link->rx_size - tail + head;
    return SWEET_GLINK_OK;
}

int sweet_glink_smem_peek(const struct sweet_glink_smem *link,
    size_t offset, void *out, size_t bytes)
{
    uint32_t head, tail, available, position;
    uint8_t *target = (uint8_t *)out;
    size_t i;
    int result;
    if (!out || !bytes)
        return SWEET_GLINK_ARGUMENT;
    result = indices(link, false, &head, &tail);
    if (result != SWEET_GLINK_OK)
        return result;
    available = head >= tail ? head - tail : link->rx_size - tail + head;
    if (offset > available || bytes > available - offset)
        return SWEET_GLINK_AGAIN;
    position = (uint32_t)(((uint64_t)tail + offset) % link->rx_size);
    for (i = 0; i < bytes; ++i) {
        target[i] = link->rx[position];
        if (++position == link->rx_size)
            position = 0;
    }
    return SWEET_GLINK_OK;
}

int sweet_glink_smem_consume(struct sweet_glink_smem *link, size_t bytes)
{
    uint32_t head, tail, available, position;
    int result;
    if (!bytes || (bytes & 7u))
        return SWEET_GLINK_ARGUMENT;
    result = indices(link, false, &head, &tail);
    if (result != SWEET_GLINK_OK)
        return result;
    available = head >= tail ? head - tail : link->rx_size - tail + head;
    if (bytes > available)
        return SWEET_GLINK_AGAIN;
    position = (uint32_t)(((uint64_t)tail + bytes) % link->rx_size);
    shared_barrier();
    link->descriptor[2] = little32(position);
    shared_barrier();
    return SWEET_GLINK_OK;
}
#endif
