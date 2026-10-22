/* SPDX-License-Identifier: MIT */
#ifndef SWEET_GLINK_CONTROL_H
#define SWEET_GLINK_CONTROL_H

#include "glink_smem.h"

#ifdef __cplusplus
extern "C" {
#endif

enum sweet_glink_control_error {
    /* Leave the command in RX for the channel/intent layer. */
    SWEET_GLINK_CONTROL_UNHANDLED = 1,
    SWEET_GLINK_CONTROL_PROTOCOL = -20,
    SWEET_GLINK_CONTROL_REJECTED = -21
};

struct sweet_glink_control {
    struct sweet_glink_smem *link;
    uint8_t pending[8];
    bool has_pending;
    bool local_version_sent;
    bool ready;
    bool failed;
};

/* Start a new GLINK v1 negotiation on an already bound, exclusively owned
 * edge. No shared-memory writes here; the initial VERSION is queued locally.
 * Advertises zero features: neither intent reuse nor intentless transfers
 * have been implemented. State/link/shared mappings must be disjoint.
 * Caller serializes all calls and excludes restart and mapping teardown.
 * Never initialize this against Android's live edge or reuse old indices
 * after a DSP restart without platform-level recovery.
 */
int sweet_glink_control_init(struct sweet_glink_control *state,
    struct sweet_glink_smem *link);

/* Perform one bounded unit: flush a pending reply OR handle one RX header.
 * OK means progress, not necessarily a negotiated link; inspect ready.
 * AGAIN can mean no input, or a queued reply blocked by TX space. An RX
 * command may already have been consumed when its reply is queued; retry
 * preserves the reply and never consumes that command twice. Caller may
 * request TX space with sweet_glink_smem_request_space(), and must provide
 * timeouts/IRQ scheduling. No allocation, polling loop or hardware startup.
 *
 * READ_NOTIF is consumed before notifying the peer. Unknown commands stay
 * in RX. Version zero fails the negotiation. Repeated VERSION is answered
 * normally; it cannot establish whether the DSP restarted. Platform-level
 * restart detection must invalidate this state and all channel/intent state.
 * A ready link has only agreed on v1/zero features; channels, intents, QRTR
 * and WLAN are not established by this API.
 */
int sweet_glink_control_step(struct sweet_glink_control *state);

#ifdef __cplusplus
}
#endif
#endif
