/* SPDX-License-Identifier: MIT
 * Independent GLINK v1 negotiation, using only public wire-format facts.
 * See README for the protocol references and hardware-binding limitations.
 */
#if !defined(KERNEL) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "glink_control.h"

static bool separate(const volatile void *a, size_t na,
    const volatile void *b, size_t nb)
{
    uintptr_t aa = (uintptr_t)a, bb = (uintptr_t)b;
    return na <= UINTPTR_MAX - aa && nb <= UINTPTR_MAX - bb &&
        (aa + na <= bb || bb + nb <= aa);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void queue_version(struct sweet_glink_control *state, bool ack)
{
    unsigned i;
    for (i = 0; i < 8; ++i)
        state->pending[i] = 0;
    state->pending[0] = ack ? 1u : 0u;
    state->pending[2] = 1u;
    state->has_pending = true;
}

int sweet_glink_control_init(struct sweet_glink_control *state,
    struct sweet_glink_smem *link)
{
    struct sweet_glink_control candidate;
    size_t ignored;
    int result;
    if (!state || !link)
        return SWEET_GLINK_ARGUMENT;
    result = sweet_glink_smem_rx_available(link, &ignored);
    if (result != SWEET_GLINK_OK)
        return result;
    result = sweet_glink_smem_tx_room(link, &ignored);
    if (result != SWEET_GLINK_OK)
        return result;
    if (!separate(state, sizeof(*state), link, sizeof(*link)) ||
        !separate(state, sizeof(*state), link->descriptor, SWEET_GLINK_DESCRIPTOR_BYTES) ||
        !separate(state, sizeof(*state), link->tx, link->tx_size) ||
        !separate(state, sizeof(*state), link->rx, link->rx_size))
        return SWEET_GLINK_ARGUMENT;
    candidate.link = link;
    candidate.local_version_sent = false;
    candidate.ready = false;
    candidate.failed = false;
    queue_version(&candidate, false);
    *state = candidate;
    return SWEET_GLINK_OK;
}

static int flush(struct sweet_glink_control *state)
{
    int result = sweet_glink_smem_send(state->link, state->pending, 8u);
    if (result != SWEET_GLINK_OK)
        return result;
    if (state->pending[0] == 0u) {
        state->local_version_sent = true;
        state->ready = false;
    }
    state->has_pending = false;
    return SWEET_GLINK_OK;
}

int sweet_glink_control_step(struct sweet_glink_control *state)
{
    uint8_t command[8];
    uint16_t kind, version;
    uint32_t features;
    int result;
    if (!state || !state->link)
        return SWEET_GLINK_ARGUMENT;
    if (state->failed)
        return SWEET_GLINK_CONTROL_REJECTED;
    if (state->has_pending)
        return flush(state);
    result = sweet_glink_smem_peek(state->link, 0, command, sizeof(command));
    if (result != SWEET_GLINK_OK)
        return result;
    kind = get16(command);
    version = get16(command + 2);
    features = get32(command + 4);
    if (kind == 13u) {
        if (version || features)
            return SWEET_GLINK_CONTROL_PROTOCOL;
        result = sweet_glink_smem_consume(state->link, 8u);
        if (result == SWEET_GLINK_OK)
            state->link->notify(state->link->context);
        return result;
    }
    if (kind > 1u)
        return SWEET_GLINK_CONTROL_UNHANDLED;
    if (!state->local_version_sent)
        return SWEET_GLINK_CONTROL_PROTOCOL;
    result = sweet_glink_smem_consume(state->link, 8u);
    if (result != SWEET_GLINK_OK)
        return result;
    if (version == 0u) {
        state->ready = false;
        state->failed = true;
        return SWEET_GLINK_CONTROL_REJECTED;
    }
    if (kind == 0u) {
        /* Counter-offer v1; unknown versions have no agreed feature set. */
        queue_version(state, true);
    } else if (version == 1u && features == 0u) {
        state->ready = true;
        return SWEET_GLINK_OK;
    } else {
        /* Never accept features that this implementation did not offer. */
        state->ready = false;
        queue_version(state, false);
    }
    return flush(state);
}
#endif
