/* SPDX-License-Identifier: MIT */
#ifndef SWEET_GLINK_CHANNEL_H
#define SWEET_GLINK_CHANNEL_H

#include "glink_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWEET_GLINK_NAME_BYTES 32u
#define SWEET_GLINK_CHANNEL_STATE (-30)

struct sweet_glink_channel {
    struct sweet_glink_control *control;
    uint16_t local_id;
    uint16_t remote_id;
    uint8_t name[SWEET_GLINK_NAME_BYTES];
    uint8_t name_bytes;
    uint8_t pending[8u + SWEET_GLINK_NAME_BYTES];
    uint8_t pending_bytes;
    bool open_sent;
    bool open_acked;
    bool remote_open_acked;
    bool closing;
    bool close_sent;
    bool close_acked;
    bool remote_closed;
    bool remote_close_acked;
    bool invalidated;
};

/* One named endpoint on a negotiated v1/zero-feature edge. name_bytes includes
 * exactly one trailing NUL, length 2..32. local_id is nonzero and allocated
 * exclusively by the caller. Schedules OPEN without sending it yet. If a
 * peer OPEN is already queued, acknowledge it before sending our own OPEN.
 * State, parent/link and shared mappings must be disjoint. All operations
 * are serialized by the edge owner; no reentrant mailbox callbacks.
 */
int sweet_glink_channel_init(struct sweet_glink_channel *state,
    struct sweet_glink_control *control, uint16_t local_id,
    const char *name, size_t name_bytes);

/* Bounded work: one queued TX or one RX command. Uses the real SMEM transport.
 * Incoming edge control messages are delegated to glink_control. Unknown or
 * other-channel commands remain in RX (CONTROL_UNHANDLED); an edge dispatcher
 * must route them. Handles OPEN/OPEN_ACK/CLOSE/CLOSE_ACK only: no intents,
 * TX_DATA, QRTR payloads, firmware startup or platform memory mapping.
 * Replies survive TX backpressure; AGAIN may follow RX consumption.
 * Caller supplies deadlines, IRQ scheduling and DSP restart exclusion.
 */
int sweet_glink_channel_step(struct sweet_glink_channel *state);
bool sweet_glink_channel_ready(const struct sweet_glink_channel *state);

/* Queue a graceful CLOSE for a ready channel. A peer CLOSE also initiates our
 * CLOSE automatically. closed requires both sides' close handshakes; no ID,
 * shared memory, intent or buffer is freed by these functions. Do not reuse
 * IDs or reinitialize live channel state without edge-level lifetime control.
 * Loss of parent negotiation invalidates this state until explicit reinit.
 */
int sweet_glink_channel_close(struct sweet_glink_channel *state);
bool sweet_glink_channel_closed(const struct sweet_glink_channel *state);

#ifdef __cplusplus
}
#endif
#endif
