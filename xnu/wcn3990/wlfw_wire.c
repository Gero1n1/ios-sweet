/* SPDX-License-Identifier: MIT */
#if !defined(KERNEL) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "wlfw_wire.h"

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read64(const uint8_t *p)
{
    return (uint64_t)read32(p) | ((uint64_t)read32(p + 4) << 32);
}

static void write16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write32(uint8_t *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}

static void write64(uint8_t *p, uint64_t value)
{
    write32(p, (uint32_t)value);
    write32(p + 4, (uint32_t)(value >> 32));
}

static int prepare(uint16_t message, uint16_t transaction, uint8_t *out,
    size_t capacity, size_t *written, uint16_t body_size)
{
    if (out == NULL || written == NULL || transaction == 0)
        return WLFW_WIRE_ARGUMENT;
    if (capacity < WLFW_HEADER_SIZE + body_size)
        return WLFW_WIRE_BOUNDS;
    out[0] = 0; /* QMI_REQUEST */
    write16(out + 1, transaction);
    write16(out + 3, message);
    write16(out + 5, body_size);
    *written = WLFW_HEADER_SIZE + body_size;
    return WLFW_WIRE_OK;
}

int wlfw_encode_registration(uint16_t transaction, uint32_t client_id,
    uint8_t *out, size_t capacity, size_t *written)
{
    int error = prepare(WLFW_IND_REGISTER, transaction, out, capacity, written, 19);
    if (error != WLFW_WIRE_OK)
        return error;
    /* Subscribe to firmware-ready, MSA-ready and pin-connect-result. */
    out[7] = 0x10; write16(out + 8, 1); out[10] = 1;
    out[11] = 0x13; write16(out + 12, 1); out[14] = 1;
    out[15] = 0x14; write16(out + 16, 1); out[18] = 1;
    out[19] = 0x15; write16(out + 20, 4); write32(out + 22, client_id);
    return WLFW_WIRE_OK;
}

int wlfw_encode_msa_info(uint16_t transaction, uint64_t address, uint32_t size,
    uint8_t *out, size_t capacity, size_t *written)
{
    int error;
    if (size == 0 || address > UINT64_MAX - size)
        return WLFW_WIRE_ARGUMENT;
    error = prepare(WLFW_MSA_INFO, transaction, out, capacity, written, 18);
    if (error != WLFW_WIRE_OK)
        return error;
    out[7] = 1; write16(out + 8, 8); write64(out + 10, address);
    out[18] = 2; write16(out + 19, 4); write32(out + 21, size);
    return WLFW_WIRE_OK;
}

int wlfw_encode_empty(uint16_t message, uint16_t transaction,
    uint8_t *out, size_t capacity, size_t *written)
{
    if (message != WLFW_CAPABILITY && message != WLFW_MSA_READY)
        return WLFW_WIRE_ARGUMENT;
    return prepare(message, transaction, out, capacity, written, 0);
}

struct cursor { const uint8_t *next; size_t remaining; };
struct tlv { uint8_t type; uint16_t size; const uint8_t *data; };

static int response(const uint8_t *packet, size_t length, uint16_t message,
    uint16_t transaction, struct cursor *cursor)
{
    if (packet == NULL || transaction == 0)
        return WLFW_WIRE_ARGUMENT;
    if (length < WLFW_HEADER_SIZE)
        return WLFW_WIRE_BOUNDS;
    if (packet[0] != 2 || read16(packet + 1) != transaction ||
        read16(packet + 3) != message)
        return WLFW_WIRE_CORRELATION;
    if (length - WLFW_HEADER_SIZE != read16(packet + 5))
        return WLFW_WIRE_PROTOCOL;
    cursor->next = packet + WLFW_HEADER_SIZE;
    cursor->remaining = length - WLFW_HEADER_SIZE;
    return WLFW_WIRE_OK;
}

static int next_tlv(struct cursor *cursor, struct tlv *tlv)
{
    if (cursor->remaining < 3)
        return WLFW_WIRE_BOUNDS;
    tlv->type = cursor->next[0];
    tlv->size = read16(cursor->next + 1);
    if (tlv->size > cursor->remaining - 3)
        return WLFW_WIRE_BOUNDS;
    tlv->data = cursor->next + 3;
    cursor->next += 3 + tlv->size;
    cursor->remaining -= 3 + tlv->size;
    return WLFW_WIRE_OK;
}

static int decode_result(const struct tlv *tlv, bool *seen,
    struct wlfw_result *result)
{
    if (*seen || tlv->size != 4)
        return WLFW_WIRE_PROTOCOL;
    result->result = read16(tlv->data);
    result->error = read16(tlv->data + 2);
    if (result->result > 1 || (result->result == 0 && result->error != 0))
        return WLFW_WIRE_PROTOCOL;
    *seen = true;
    return WLFW_WIRE_OK;
}

/* These response shapes share the mandatory common result TLV. */
static int decode(const uint8_t *packet, size_t length, uint16_t message,
    uint16_t transaction, struct wlfw_registration *registration,
    struct wlfw_msa_response *msa)
{
    struct cursor cursor;
    struct tlv tlv;
    struct wlfw_result *status = registration ? &registration->status : &msa->status;
    bool have_result = false, have_regions = false;
    unsigned i;
    int error = response(packet, length, message, transaction, &cursor);
    if (error != WLFW_WIRE_OK)
        return error;
    while (cursor.remaining != 0) {
        error = next_tlv(&cursor, &tlv);
        if (error != WLFW_WIRE_OK)
            return error;
        if (tlv.type == 2) {
            error = decode_result(&tlv, &have_result, status);
            if (error != WLFW_WIRE_OK)
                return error;
        } else if (tlv.type == 0x10 && message == WLFW_IND_REGISTER) {
            if (registration->has_fw_status || tlv.size != 8)
                return WLFW_WIRE_PROTOCOL;
            registration->has_fw_status = true;
            registration->fw_status = read64(tlv.data);
        } else if (tlv.type == 3 && message == WLFW_MSA_INFO) {
            if (have_regions || tlv.size < 1)
                return WLFW_WIRE_PROTOCOL;
            have_regions = true;
            msa->count = tlv.data[0];
            if (msa->count > WLFW_MAX_MSA_REGIONS ||
                tlv.size != 1u + 13u * msa->count)
                return WLFW_WIRE_PROTOCOL;
            for (i = 0; i < msa->count; ++i) {
                const uint8_t *p = tlv.data + 1 + 13 * i;
                msa->regions[i].address = read64(p);
                msa->regions[i].size = read32(p + 8);
                msa->regions[i].secure = p[12];
                if (p[12] > 1)
                    return WLFW_WIRE_PROTOCOL;
            }
        } else if (tlv.type < 0x10) {
            return WLFW_WIRE_PROTOCOL;
        }
    }
    if (!have_result || (message == WLFW_MSA_INFO &&
        status->result == 0 && !have_regions))
        return WLFW_WIRE_PROTOCOL;
    return WLFW_WIRE_OK;
}

int wlfw_decode_registration(const uint8_t *packet, size_t length,
    uint16_t transaction, struct wlfw_registration *out)
{
    struct wlfw_registration value = { {0, 0}, false, 0 };
    int error;
    if (out == NULL)
        return WLFW_WIRE_ARGUMENT;
    error = decode(packet, length, WLFW_IND_REGISTER, transaction, &value, NULL);
    if (error == WLFW_WIRE_OK)
        *out = value;
    return error;
}

int wlfw_decode_msa_info(const uint8_t *packet, size_t length,
    uint16_t transaction, struct wlfw_msa_response *out)
{
    struct wlfw_msa_response value = { {0, 0}, 0, {{0, 0, 0}, {0, 0, 0}} };
    int error;
    if (out == NULL)
        return WLFW_WIRE_ARGUMENT;
    error = decode(packet, length, WLFW_MSA_INFO, transaction, NULL, &value);
    if (error == WLFW_WIRE_OK)
        *out = value;
    return error;
}

int wlfw_decode_status(const uint8_t *packet, size_t length,
    uint16_t message, uint16_t transaction, struct wlfw_result *out)
{
    struct wlfw_registration value = { {0, 0}, false, 0 };
    int error;
    if (out == NULL || (message != WLFW_MSA_READY && message != WLFW_CAPABILITY))
        return WLFW_WIRE_ARGUMENT;
    error = decode(packet, length, message, transaction, &value, NULL);
    if (error == WLFW_WIRE_OK)
        *out = value.status;
    return error;
}

bool wlfw_msa_regions_safe(const struct wlfw_msa_response *response,
    uint64_t reservation_base, uint32_t reservation_size)
{
    unsigned i, j;
    if (response == NULL || reservation_size == 0 ||
        reservation_base > UINT64_MAX - reservation_size ||
        response->status.result != 0 || response->status.error != 0 ||
        response->count == 0 || response->count > WLFW_MAX_MSA_REGIONS)
        return false;
    for (i = 0; i < response->count; ++i) {
        const struct wlfw_msa_region *r = &response->regions[i];
        uint64_t offset;
        if (r->secure > 1 || r->size == 0 || r->address < reservation_base)
            return false;
        offset = r->address - reservation_base;
        if (offset >= reservation_size || r->size > reservation_size - offset)
            return false;
        for (j = 0; j < i; ++j) {
            const struct wlfw_msa_region *previous = &response->regions[j];
            if (r->address < previous->address + previous->size &&
                previous->address < r->address + r->size)
                return false;
        }
    }
    return true;
}
#endif
