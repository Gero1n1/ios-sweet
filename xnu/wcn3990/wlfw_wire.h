/* SPDX-License-Identifier: MIT
 * WLFW/QMI wire codec. No OS APIs, transport, MMIO, or memory ownership changes.
 */
#ifndef SWEET_WLFW_WIRE_H
#define SWEET_WLFW_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WLFW_SERVICE_ID 0x45u
#define WLFW_SERVICE_VERSION 1u
#define WLFW_HOST_CLIENT_ID UINT32_C(0x4b4e454c)
#define WLFW_HEADER_SIZE 7u
#define WLFW_MAX_MSA_REGIONS 2u
#define WLFW_IND_REGISTER 0x0020u
#define WLFW_FW_READY_IND 0x0021u
#define WLFW_CAPABILITY 0x0024u
#define WLFW_MSA_READY_IND 0x002bu
#define WLFW_MSA_INFO 0x002du
#define WLFW_MSA_READY 0x002eu

enum wlfw_wire_error {
    WLFW_WIRE_OK = 0,
    WLFW_WIRE_ARGUMENT = -1,
    WLFW_WIRE_BOUNDS = -2,
    WLFW_WIRE_PROTOCOL = -3,
    WLFW_WIRE_CORRELATION = -4
};

struct wlfw_result {
    uint16_t result; /* 0: success, 1: firmware-reported failure. */
    uint16_t error;
};

struct wlfw_registration {
    struct wlfw_result status;
    bool has_fw_status;
    uint64_t fw_status;
};

struct wlfw_msa_region {
    uint64_t address;
    uint32_t size;
    uint8_t secure;
};

struct wlfw_msa_response {
    struct wlfw_result status;
    uint8_t count;
    struct wlfw_msa_region regions[WLFW_MAX_MSA_REGIONS];
};

/* Emit one complete QMI service message (no QMUX/QRTR envelope).
 * out and written must be non-NULL. Transaction zero is reserved.
 * On error, out and *written are unchanged. Callers must not alias them.
 */
int wlfw_encode_registration(uint16_t transaction, uint32_t client_id,
    uint8_t *out, size_t capacity, size_t *written);
int wlfw_encode_msa_info(uint16_t transaction, uint64_t address, uint32_t size,
    uint8_t *out, size_t capacity, size_t *written);
/* Only CAPABILITY and MSA_READY are supported empty requests. */
int wlfw_encode_empty(uint16_t message, uint16_t transaction,
    uint8_t *out, size_t capacity, size_t *written);

/* Strictly match response type, message, transaction, and complete packet size.
 * Reject truncated/duplicate known TLVs and unknown mandatory TLVs (<0x10).
 * Optional unknown TLVs are skipped. Outputs are unchanged on parser errors.
 * A successful parse may contain a firmware error: inspect status.result.
 */
int wlfw_decode_registration(const uint8_t *packet, size_t length,
    uint16_t transaction, struct wlfw_registration *out);
int wlfw_decode_msa_info(const uint8_t *packet, size_t length,
    uint16_t transaction, struct wlfw_msa_response *out);
int wlfw_decode_status(const uint8_t *packet, size_t length,
    uint16_t message, uint16_t transaction, struct wlfw_result *out);

/* Check firmware-proposed regions against the live-DT MSA reservation before
 * any future hypervisor assignment. Requires nonempty, disjoint, contained
 * regions and successful firmware status. Does not perform an assignment.
 */
bool wlfw_msa_regions_safe(const struct wlfw_msa_response *response,
    uint64_t reservation_base, uint32_t reservation_size);

#ifdef __cplusplus
}
#endif
#endif
