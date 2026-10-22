/* Schema-derived fixtures, not packet captures or a radio integration test. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wlfw_wire.h"

static const uint8_t registration_request[] = {
    0x00,0x34,0x12,0x20,0x00,0x13,0x00,
    0x10,0x01,0x00,0x01, 0x13,0x01,0x00,0x01,
    0x14,0x01,0x00,0x01, 0x15,0x04,0x00,0x4c,0x45,0x4e,0x4b
};
static const uint8_t msa_request[] = {
    0x00,0x34,0x12,0x2d,0x00,0x12,0x00,
    0x01,0x08,0x00,0x00,0x00,0xd0,0x97,0x00,0x00,0x00,0x00,
    0x02,0x04,0x00,0x00,0x00,0x18,0x00
};
static const uint8_t registration_response[] = {
    0x02,0x34,0x12,0x20,0x00,0x12,0x00,
    0x02,0x04,0x00,0x00,0x00,0x00,0x00,
    0x10,0x08,0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88
};
static const uint8_t msa_response[] = {
    0x02,0x34,0x12,0x2d,0x00,0x25,0x00,
    0x02,0x04,0x00,0x00,0x00,0x00,0x00,
    0x03,0x1b,0x00,0x02,
    0x00,0x00,0xd0,0x97,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x01,
    0x00,0x00,0xe0,0x97,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00
};
static const uint8_t ready_response[] = {
    0x02,0x34,0x12,0x2e,0x00,0x07,0x00,
    0x02,0x04,0x00,0x00,0x00,0x00,0x00
};

static void encoding(void)
{
    uint8_t out[64], before[64];
    size_t written, capacity;
    const uint8_t empty[] = {0,0x34,0x12,0x2e,0,0,0};
    memset(out, 0xa5, sizeof(out));
    written = 99;
    assert(wlfw_encode_registration(0x1234, WLFW_HOST_CLIENT_ID,
        out, sizeof(out), &written) == 0);
    assert(written == sizeof(registration_request));
    assert(memcmp(out, registration_request, written) == 0);
    assert(out[written] == 0xa5);
    assert(wlfw_encode_msa_info(0x1234, UINT64_C(0x97d00000), 0x180000,
        out, sizeof(out), &written) == 0);
    assert(written == sizeof(msa_request));
    assert(memcmp(out, msa_request, written) == 0);
    assert(wlfw_encode_empty(WLFW_MSA_READY, 0x1234, out, sizeof(out), &written) == 0);
    assert(written == sizeof(empty) && memcmp(out, empty, written) == 0);
    assert(wlfw_encode_empty(WLFW_CAPABILITY, 0x1234, out, sizeof(out), &written) == 0);
    assert(out[3] == 0x24 && written == 7);
    for (capacity = 0; capacity < sizeof(registration_request); ++capacity) {
        memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
        written = 99;
        assert(wlfw_encode_registration(1, 0, out, capacity, &written) == WLFW_WIRE_BOUNDS);
        assert(written == 99 && memcmp(out, before, sizeof(out)) == 0);
        if (capacity < sizeof(msa_request)) {
            assert(wlfw_encode_msa_info(1, 0x97d00000, 0x180000,
                out, capacity, &written) == WLFW_WIRE_BOUNDS);
            assert(written == 99 && memcmp(out, before, sizeof(out)) == 0);
        }
    }
    assert(wlfw_encode_empty(0xffff, 1, out, sizeof(out), &written) == WLFW_WIRE_ARGUMENT);
    assert(wlfw_encode_empty(WLFW_CAPABILITY, 0, out, sizeof(out), &written) == WLFW_WIRE_ARGUMENT);
    assert(wlfw_encode_registration(1, 0, NULL, sizeof(out), &written) == WLFW_WIRE_ARGUMENT);
    assert(wlfw_encode_registration(1, 0, out, sizeof(out), NULL) == WLFW_WIRE_ARGUMENT);
    assert(wlfw_encode_msa_info(1, UINT64_MAX - 10, 11, out, sizeof(out), &written) == WLFW_WIRE_ARGUMENT);
    assert(wlfw_encode_msa_info(1, 0, 0, out, sizeof(out), &written) == WLFW_WIRE_ARGUMENT);
}

static void decoding(void)
{
    struct wlfw_registration registration;
    struct wlfw_msa_response msa;
    struct wlfw_result status;
    uint8_t packet[64];
    assert(wlfw_decode_registration(registration_response,
        sizeof(registration_response), 0x1234, &registration) == 0);
    assert(registration.status.result == 0 && registration.status.error == 0);
    assert(registration.has_fw_status && registration.fw_status == UINT64_C(0x8877665544332211));
    assert(wlfw_decode_msa_info(msa_response, sizeof(msa_response), 0x1234, &msa) == 0);
    assert(msa.count == 2 && msa.regions[0].address == UINT64_C(0x97d00000));
    assert(msa.regions[0].size == 0x100000 && msa.regions[0].secure == 1);
    assert(msa.regions[1].address == UINT64_C(0x97e00000));
    assert(msa.regions[1].size == 0x80000 && msa.regions[1].secure == 0);
    assert(wlfw_msa_regions_safe(&msa, 0x97d00000, 0x180000));
    assert(wlfw_decode_status(ready_response, sizeof(ready_response),
        WLFW_MSA_READY, 0x1234, &status) == 0 && status.result == 0);
    /* Failure replies need not contain successful-result memory fields. */
    memcpy(packet, ready_response, sizeof(ready_response));
    packet[3] = 0x2d; packet[10] = 1; packet[12] = 0x34;
    assert(wlfw_decode_msa_info(packet, sizeof(ready_response), 0x1234, &msa) == 0);
    assert(msa.status.result == 1 && msa.status.error == 0x34 && msa.count == 0);
    assert(!wlfw_msa_regions_safe(&msa, 0x97d00000, 0x180000));
    /* Well-formed future optional TLVs can be ignored. */
    memcpy(packet, ready_response, sizeof(ready_response));
    packet[5] = 12; packet[14] = 0x80; packet[15] = 2; packet[16] = 0;
    packet[17] = 0xaa; packet[18] = 0xbb;
    assert(wlfw_decode_status(packet, 19, WLFW_MSA_READY, 0x1234, &status) == 0);
    packet[14] = 1; /* Unknown mandatory TLV must not be ignored. */
    assert(wlfw_decode_status(packet, 19, WLFW_MSA_READY, 0x1234, &status) == WLFW_WIRE_PROTOCOL);
    /* Firmware may reorder fields: result is not necessarily first. */
    memcpy(packet, msa_response, 7);
    memcpy(packet + 7, msa_response + 14, 30);
    memcpy(packet + 37, msa_response + 7, 7);
    assert(wlfw_decode_msa_info(packet, sizeof(msa_response), 0x1234, &msa) == 0);
    assert(wlfw_msa_regions_safe(&msa, 0x97d00000, 0x180000));
}

static void malformed(void)
{
    uint8_t packet[80];
    struct wlfw_msa_response msa, before;
    struct wlfw_registration registration;
    struct wlfw_result status;
    size_t n;
    unsigned value;
    memset(&msa, 0xa5, sizeof(msa)); memcpy(&before, &msa, sizeof(msa));
    for (n = 0; n < sizeof(msa_response); ++n) {
        assert(wlfw_decode_msa_info(msa_response, n, 0x1234, &msa) != 0);
        assert(memcmp(&msa, &before, sizeof(msa)) == 0);
    }
    for (value = 0; value <= UINT16_MAX; ++value) {
        /* Exhaust the 16-bit declared length fields at fixed packet length. */
        memcpy(packet, msa_response, sizeof(msa_response));
        packet[5] = (uint8_t)value; packet[6] = (uint8_t)(value >> 8);
        if (value != 37)
            assert(wlfw_decode_msa_info(packet, sizeof(msa_response), 0x1234, &msa) != 0);
        memcpy(packet, msa_response, sizeof(msa_response));
        packet[15] = (uint8_t)value; packet[16] = (uint8_t)(value >> 8);
        if (value != 27)
            assert(wlfw_decode_msa_info(packet, sizeof(msa_response), 0x1234, &msa) != 0);
    }
    for (value = 0; value <= UINT8_MAX; ++value) {
        memcpy(packet, msa_response, sizeof(msa_response)); packet[17] = (uint8_t)value;
        if (value != 2)
            assert(wlfw_decode_msa_info(packet, sizeof(msa_response), 0x1234, &msa) != 0);
    }
    assert(wlfw_decode_msa_info(msa_response, sizeof(msa_response), 1, &msa) == WLFW_WIRE_CORRELATION);
    memcpy(packet, msa_response, sizeof(msa_response)); packet[0] = 4;
    assert(wlfw_decode_msa_info(packet, sizeof(msa_response), 0x1234, &msa) == WLFW_WIRE_CORRELATION);
    assert(wlfw_decode_msa_info(NULL, 0, 1, &msa) == WLFW_WIRE_ARGUMENT);
    assert(wlfw_decode_msa_info(msa_response, sizeof(msa_response), 0x1234, NULL) == WLFW_WIRE_ARGUMENT);
    /* Duplicate common result, missing result, and bad result size. */
    memcpy(packet, ready_response, 14); memcpy(packet + 14, ready_response + 7, 7); packet[5] = 14;
    assert(wlfw_decode_status(packet, 21, WLFW_MSA_READY, 0x1234, &status) == WLFW_WIRE_PROTOCOL);
    packet[5] = 0;
    assert(wlfw_decode_status(packet, 7, WLFW_MSA_READY, 0x1234, &status) == WLFW_WIRE_PROTOCOL);
    memcpy(packet, ready_response, 14); packet[5] = 6; packet[8] = 3;
    assert(wlfw_decode_status(packet, 13, WLFW_MSA_READY, 0x1234, &status) == WLFW_WIRE_PROTOCOL);
    /* Duplicate optional status must not override the original value. */
    memcpy(packet, registration_response, 25); memcpy(packet + 25, registration_response + 14, 11);
    packet[5] = 29;
    assert(wlfw_decode_registration(packet, 36, 0x1234, &registration) == WLFW_WIRE_PROTOCOL);
    /* Success without mandatory regions and success with error are rejected. */
    memcpy(packet, ready_response, 14); packet[3] = 0x2d;
    assert(wlfw_decode_msa_info(packet, 14, 0x1234, &msa) == WLFW_WIRE_PROTOCOL);
    memcpy(packet, ready_response, 14); packet[12] = 1;
    assert(wlfw_decode_status(packet, 14, WLFW_MSA_READY, 0x1234, &status) == WLFW_WIRE_PROTOCOL);
    /* A valid packet on an unaligned address must also decode safely. */
    memcpy(packet + 1, msa_response, sizeof(msa_response));
    assert(wlfw_decode_msa_info(packet + 1, sizeof(msa_response), 0x1234, &msa) == 0);
}

static void regions(void)
{
    struct wlfw_msa_response original, value;
    assert(wlfw_decode_msa_info(msa_response, sizeof(msa_response), 0x1234, &original) == 0);
    assert(!wlfw_msa_regions_safe(NULL, 0, 1));
    assert(!wlfw_msa_regions_safe(&original, UINT64_MAX - 4, 10));
    assert(!wlfw_msa_regions_safe(&original, 0x97d00000, 0));
    value = original; value.count = 3;
    assert(!wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
    value = original; value.count = 0;
    assert(!wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
    value = original; value.status.result = 1;
    assert(!wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
    value = original; value.regions[0].address--;
    assert(!wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
    value = original; value.regions[0].size = 0;
    assert(!wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
    value = original; value.regions[1].size++;
    assert(!wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
    value = original; value.regions[1].address--;
    assert(!wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
    value = original; value.regions[0].secure = 2;
    assert(!wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
    value = original; value.regions[1].address = UINT64_MAX - 4;
    assert(!wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
    /* Adjacent regions are valid, including reversed wire order. */
    value = original; value.regions[0] = original.regions[1]; value.regions[1] = original.regions[0];
    assert(wlfw_msa_regions_safe(&value, 0x97d00000, 0x180000));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "encoding") == 0) encoding();
    else if (strcmp(argv[1], "decoding") == 0) decoding();
    else if (strcmp(argv[1], "malformed") == 0) malformed();
    else if (strcmp(argv[1], "regions") == 0) regions();
    else return 2;
    puts("PASS");
    return 0;
}
