/* SPDX-License-Identifier: MIT
 * Independent implementation of GLINK channel lifecycle wire semantics.
 * Protocol references and hardware limitations are documented in README.
 */
#if !defined(KERNEL) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "glink_channel.h"

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

static void queue(struct sweet_glink_channel *state, uint8_t kind, uint16_t id)
{
    unsigned i;
    for (i = 0; i < sizeof(state->pending); ++i)
        state->pending[i] = 0;
    state->pending[0] = kind;
    state->pending[2] = (uint8_t)id;
    state->pending[3] = (uint8_t)(id >> 8);
    state->pending_bytes = 8u;
    if (kind == 2u) {
        state->pending[4] = state->name_bytes;
        for (i = 0; i < state->name_bytes; ++i)
            state->pending[8u + i] = state->name[i];
        state->pending_bytes = (uint8_t)(8u + state->name_bytes);
    }
}

int sweet_glink_channel_init(struct sweet_glink_channel *state,
    struct sweet_glink_control *control, uint16_t local_id,
    const char *name, size_t name_bytes)
{
    struct sweet_glink_channel candidate;
    struct sweet_glink_smem *link;
    size_t ignored, i;
    int result;
    if (!state || !control || !control->link || !local_id || !name ||
        name_bytes < 2u || name_bytes > SWEET_GLINK_NAME_BYTES)
        return SWEET_GLINK_ARGUMENT;
    if (!control->ready || control->failed || control->has_pending)
        return SWEET_GLINK_CHANNEL_STATE;
    link = control->link;
    result = sweet_glink_smem_tx_room(link, &ignored);
    if (result != SWEET_GLINK_OK)
        return result;
    result = sweet_glink_smem_rx_available(link, &ignored);
    if (result != SWEET_GLINK_OK)
        return result;
    if (!separate(state, sizeof(*state), control, sizeof(*control)) ||
        !separate(state, sizeof(*state), link, sizeof(*link)) ||
        !separate(state, sizeof(*state), link->descriptor, SWEET_GLINK_DESCRIPTOR_BYTES) ||
        !separate(state, sizeof(*state), link->tx, link->tx_size) ||
        !separate(state, sizeof(*state), link->rx, link->rx_size))
        return SWEET_GLINK_ARGUMENT;
    for (i = 0; i < name_bytes; ++i) {
        if ((name[i] == 0) != (i == name_bytes - 1u))
            return SWEET_GLINK_ARGUMENT;
    }
    candidate.control = control;
    candidate.local_id = local_id;
    candidate.remote_id = 0;
    candidate.name_bytes = (uint8_t)name_bytes;
    for (i = 0; i < SWEET_GLINK_NAME_BYTES; ++i)
        candidate.name[i] = i < name_bytes ? (uint8_t)name[i] : 0;
    candidate.open_sent = false;
    candidate.open_acked = false;
    candidate.remote_open_acked = false;
    candidate.closing = false;
    candidate.close_sent = false;
    candidate.close_acked = false;
    candidate.remote_closed = false;
    candidate.remote_close_acked = false;
    candidate.invalidated = false;
    queue(&candidate, 2u, local_id);
    /* Defer OPEN until step can first acknowledge a remote-initiated OPEN. */
    candidate.pending_bytes = 0;
    *state = candidate;
    return SWEET_GLINK_OK;
}

bool sweet_glink_channel_ready(const struct sweet_glink_channel *state)
{
    return state && state->control && state->control->ready &&
        !state->control->failed && !state->control->has_pending &&
        !state->invalidated && !state->closing &&
        state->open_sent && state->open_acked && state->remote_id &&
        state->remote_open_acked;
}

bool sweet_glink_channel_closed(const struct sweet_glink_channel *state)
{
    return state && state->control && state->control->ready && !state->control->failed &&
        !state->invalidated && state->closing && (!state->open_sent || state->close_acked) &&
        state->remote_closed && state->remote_close_acked && !state->pending_bytes;
}

static int flush(struct sweet_glink_channel *state)
{
    int result = sweet_glink_smem_send(state->control->link,
        state->pending, state->pending_bytes);
    if (result != SWEET_GLINK_OK)
        return result;
    switch (state->pending[0]) {
    case 2u: state->open_sent = true; break;
    case 4u: state->remote_open_acked = true; break;
    case 3u: state->close_sent = true; break;
    case 11u: state->remote_close_acked = true; break;
    }
    state->pending_bytes = 0;
    return SWEET_GLINK_OK;
}

int sweet_glink_channel_close(struct sweet_glink_channel *state)
{
    if (!state || !state->control)
        return SWEET_GLINK_ARGUMENT;
    if (state->invalidated || !state->control->ready || state->control->failed)
        return SWEET_GLINK_CHANNEL_STATE;
    if (state->closing)
        return SWEET_GLINK_OK;
    if (!sweet_glink_channel_ready(state) || state->pending_bytes)
        return SWEET_GLINK_CHANNEL_STATE;
    state->closing = true;
    queue(state, 3u, state->local_id);
    return SWEET_GLINK_OK;
}

int sweet_glink_channel_step(struct sweet_glink_channel *state)
{
    uint8_t header[8], name[SWEET_GLINK_NAME_BYTES];
    uint16_t kind, id;
    uint32_t param;
    size_t length = 8u, i;
    int result;
    struct sweet_glink_smem *link;
    if (!state || !state->control || !state->control->link)
        return SWEET_GLINK_ARGUMENT;
    if (state->invalidated || !state->control->ready || state->control->failed) {
        state->invalidated = true;
        return SWEET_GLINK_CHANNEL_STATE;
    }
    if (state->control->has_pending)
        return sweet_glink_control_step(state->control);
    if (state->pending_bytes)
        return flush(state);
    if (state->closing && state->open_sent && !state->close_sent) {
        queue(state, 3u, state->local_id);
        return flush(state);
    }
    link = state->control->link;
    result = sweet_glink_smem_peek(link, 0, header, sizeof(header));
    if (result != SWEET_GLINK_OK) {
        if (result == SWEET_GLINK_AGAIN && !state->open_sent && !state->closing) {
            queue(state, 2u, state->local_id);
            return flush(state);
        }
        return result;
    }
    kind = get16(header);
    id = get16(header + 2);
    param = get32(header + 4);
    if (kind == 0u || kind == 1u || kind == 13u) {
        result = sweet_glink_control_step(state->control);
        if (!state->control->ready || state->control->failed)
            state->invalidated = true;
        return result;
    }
    if (kind != 2u && kind != 3u && kind != 4u && kind != 11u)
        return SWEET_GLINK_CONTROL_UNHANDLED;
    if (!id)
        return SWEET_GLINK_CONTROL_PROTOCOL;
    if (kind == 2u) {
        if (param < 2u || param > SWEET_GLINK_NAME_BYTES)
            return SWEET_GLINK_CONTROL_PROTOCOL;
        length = (8u + param + 7u) & ~(size_t)7u;
        /* Require padding to be available, but do not assume its contents. */
        result = sweet_glink_smem_peek(link, length - 1u, name, 1u);
        if (result != SWEET_GLINK_OK)
            return result;
        result = sweet_glink_smem_peek(link, 8u, name, param);
        if (result != SWEET_GLINK_OK)
            return result;
        for (i = 0; i < param; ++i) {
            if ((name[i] == 0) != (i == param - 1u))
                return SWEET_GLINK_CONTROL_PROTOCOL;
        }
        if (param != state->name_bytes)
            return SWEET_GLINK_CONTROL_UNHANDLED;
        for (i = 0; i < param; ++i) {
            if (name[i] != state->name[i])
                return SWEET_GLINK_CONTROL_UNHANDLED;
        }
        if (state->closing || (state->remote_id && state->remote_id != id))
            return SWEET_GLINK_CONTROL_PROTOCOL;
    } else {
        if (param)
            return SWEET_GLINK_CONTROL_PROTOCOL;
        if (kind == 3u) {
            if (id != state->remote_id)
                return SWEET_GLINK_CONTROL_UNHANDLED;
        } else if (id != state->local_id) {
            return SWEET_GLINK_CONTROL_UNHANDLED;
        } else if ((kind == 4u && !state->open_sent) ||
                   (kind == 11u && !state->close_sent)) {
            return SWEET_GLINK_CONTROL_PROTOCOL;
        }
    }
    result = sweet_glink_smem_consume(link, length);
    if (result != SWEET_GLINK_OK)
        return result;
    switch (kind) {
    case 2u:
        state->remote_id = id;
        state->remote_open_acked = false;
        queue(state, 4u, id);
        break;
    case 4u:
        state->open_acked = true;
        return SWEET_GLINK_OK;
    case 3u:
        state->closing = true;
        state->remote_closed = true;
        state->remote_close_acked = false;
        queue(state, 11u, id);
        break;
    case 11u:
        state->close_acked = true;
        return SWEET_GLINK_OK;
    }
    return flush(state);
}
#endif
