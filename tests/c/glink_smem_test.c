/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "glink_smem.h"

static uint32_t descriptor[8];
static uint8_t tx[64], rx[64];
static struct sweet_glink_smem link;
static unsigned notifications;
static uint32_t expected_head;
static const uint8_t *expected_data;
static size_t expected_length;
static uint32_t expected_start;

static void notify(void *context)
{
    size_t i;
    assert(context == &notifications);
    /* Notification must follow both payload and head publication. */
    assert(descriptor[1] == expected_head);
    if (expected_data) {
        size_t padded = (expected_length + 7u) & ~(size_t)7u;
        for (i = 0; i < padded; ++i)
            assert(tx[(expected_start + i) % 64] ==
                (i < expected_length ? expected_data[i] : 0));
    }
    ++notifications;
}

static void reset(void)
{
    memset(descriptor, 0, sizeof(descriptor));
    memset(tx, 0xa5, sizeof(tx));
    memset(rx, 0x5a, sizeof(rx));
    memset(&link, 0, sizeof(link));
    notifications = 0;
    expected_head = 0;
    expected_data = NULL;
    expected_length = 0;
    expected_start = 0;
    assert(sweet_glink_smem_bind(&link, descriptor, sizeof(descriptor),
        tx, sizeof(tx), rx, sizeof(rx), notify, &notifications) == SWEET_GLINK_OK);
}

static void binding(void)
{
    struct sweet_glink_smem before;
    reset();
    before = link;
    assert(sweet_glink_smem_bind(&link, descriptor, 16, tx, 64, rx, 64,
        notify, &notifications) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_bind(&link, descriptor, 32, tx, 63, rx, 64,
        notify, &notifications) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_bind(&link, descriptor, 32, tx, 16, rx, 64,
        notify, &notifications) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_bind(&link, descriptor, 32, tx, 64, tx, 64,
        notify, &notifications) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_bind(&link, descriptor, 32, descriptor, 64, rx, 64,
        notify, &notifications) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_bind(&link, descriptor, 32, tx, 64, rx, 64,
        NULL, NULL) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_bind(NULL, descriptor, 32, tx, 64, rx, 64,
        notify, &notifications) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_bind(&link, (volatile uint32_t *)(tx + 1),
        32, tx, 64, rx, 64, notify, &notifications) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_bind(&link, descriptor, 32, &link, 64, rx, 64,
        notify, &notifications) == SWEET_GLINK_ARGUMENT);
    assert(memcmp(&before, &link, sizeof(link)) == 0);
    descriptor[3] = 64;
    assert(sweet_glink_smem_bind(&link, descriptor, 32, tx, 64, rx, 64,
        notify, &notifications) == SWEET_GLINK_CORRUPT);
    assert(memcmp(&before, &link, sizeof(link)) == 0);
    assert(notifications == 0 && descriptor[3] == 64);
}

static void transmit(void)
{
    static const uint8_t frame[13] = {0, 0, 1, 0, 0, 0, 0, 0, 0x44, 0x55, 0x66, 0x77, 0x88};
    size_t room = 999;
    reset();
    assert(sweet_glink_smem_tx_room(&link, &room) == SWEET_GLINK_OK && room == 48);
    expected_head = 16;
    expected_data = frame;
    expected_length = sizeof(frame);
    assert(sweet_glink_smem_send(&link, frame, sizeof(frame)) == SWEET_GLINK_OK);
    assert(notifications == 1 && descriptor[0] == 0 && descriptor[2] == 0 && descriptor[3] == 0);
    assert(tx[16] == 0xa5);
    assert(sweet_glink_smem_tx_room(&link, &room) == SWEET_GLINK_OK && room == 32);

    reset();
    descriptor[1] = 56;
    descriptor[0] = 24;
    expected_head = 8;
    expected_start = 56;
    expected_data = frame;
    expected_length = sizeof(frame);
    assert(sweet_glink_smem_send(&link, frame, sizeof(frame)) == SWEET_GLINK_OK);
    assert(notifications == 1 && tx[8] == 0xa5);
}

static void backpressure(void)
{
    uint8_t before[64], frame[48];
    static const uint8_t request[8] = {13, 0, 0, 0, 0, 0, 0, 0};
    reset();
    memset(frame, 7, sizeof(frame));
    expected_head = 48;
    assert(sweet_glink_smem_send(&link, frame, 48) == SWEET_GLINK_OK);
    memcpy(before, tx, sizeof(tx));
    assert(sweet_glink_smem_send(&link, frame, 8) == SWEET_GLINK_AGAIN);
    assert(memcmp(before, tx, sizeof(tx)) == 0 && descriptor[1] == 48 && notifications == 1);
    expected_head = 56;
    expected_start = 48;
    expected_data = request;
    expected_length = 8;
    assert(sweet_glink_smem_request_space(&link) == SWEET_GLINK_OK);
    assert(notifications == 2 && descriptor[1] == 56);
    assert(sweet_glink_smem_request_space(&link) == SWEET_GLINK_OK);
    assert(notifications == 2); /* no repeated READ_NOTIF flood */
    assert(sweet_glink_smem_send(&link, frame, 8) == SWEET_GLINK_AGAIN);
    descriptor[0] = 48; /* remote consumes */
    expected_data = frame;
    expected_length = 8;
    expected_start = 56;
    expected_head = 0;
    assert(sweet_glink_smem_send(&link, frame, 8) == SWEET_GLINK_OK);
    assert(!link.space_requested && notifications == 3);
}

static void receive(void)
{
    uint8_t output[32], before[32];
    size_t available = 999;
    unsigned i;
    reset();
    for (i = 0; i < 64; ++i) rx[i] = (uint8_t)i;
    descriptor[2] = 56;
    descriptor[3] = 16;
    assert(sweet_glink_smem_rx_available(&link, &available) == SWEET_GLINK_OK && available == 24);
    memset(output, 0xcc, sizeof(output));
    memcpy(before, output, sizeof(output));
    assert(sweet_glink_smem_peek(&link, 4, output, 10) == SWEET_GLINK_OK);
    for (i = 0; i < 10; ++i) assert(output[i] == (60 + i) % 64);
    assert(output[10] == 0xcc && descriptor[2] == 56);
    assert(sweet_glink_smem_peek(&link, SIZE_MAX, output, 1) == SWEET_GLINK_AGAIN);
    assert(sweet_glink_smem_peek(&link, 24, output, 1) == SWEET_GLINK_AGAIN);
    assert(sweet_glink_smem_consume(&link, 7) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_consume(&link, 32) == SWEET_GLINK_AGAIN);
    assert(sweet_glink_smem_consume(&link, 16) == SWEET_GLINK_OK && descriptor[2] == 8);
    assert(sweet_glink_smem_rx_available(&link, &available) == SWEET_GLINK_OK && available == 8);
    assert(sweet_glink_smem_consume(&link, 8) == SWEET_GLINK_OK && descriptor[2] == 16);
    assert(sweet_glink_smem_rx_available(&link, &available) == SWEET_GLINK_OK && available == 0);
    memcpy(output, before, sizeof(output));
    assert(sweet_glink_smem_peek(&link, 0, output, 1) == SWEET_GLINK_AGAIN);
    assert(memcmp(before, output, sizeof(output)) == 0 && notifications == 0);
}

static void corruption(void)
{
    uint8_t frame[16] = {0}, output[8];
    uint32_t bad[] = {1, 7, 9, 63, 64, UINT32_MAX};
    size_t value;
    unsigned index, b;
    for (index = 0; index < 4; ++index) {
        for (b = 0; b < sizeof(bad) / sizeof(bad[0]); ++b) {
            reset();
            descriptor[index] = bad[b];
            value = 0x112233;
            if (index < 2) {
                assert(sweet_glink_smem_tx_room(&link, &value) == SWEET_GLINK_CORRUPT);
                assert(sweet_glink_smem_send(&link, frame, sizeof(frame)) == SWEET_GLINK_CORRUPT);
            } else {
                memset(output, 0xa5, sizeof(output));
                assert(sweet_glink_smem_rx_available(&link, &value) == SWEET_GLINK_CORRUPT);
                assert(sweet_glink_smem_peek(&link, 0, output, 8) == SWEET_GLINK_CORRUPT);
                assert(sweet_glink_smem_consume(&link, 8) == SWEET_GLINK_CORRUPT);
                assert(output[0] == 0xa5);
            }
            assert(value == 0x112233 && descriptor[index] == bad[b] && notifications == 0);
        }
    }
    reset();
    assert(sweet_glink_smem_send(NULL, frame, 8) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_send(&link, frame, SIZE_MAX) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_send(&link, frame, 7) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_tx_room(&link, NULL) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_smem_consume(&link, SIZE_MAX) == SWEET_GLINK_ARGUMENT);
}

static void exhaustive(void)
{
    uint8_t frame[80], before[64], output[64];
    unsigned h, t, n, i;
    size_t room, available;
    for (i = 0; i < sizeof(frame); ++i) frame[i] = (uint8_t)(i + 1);
    for (h = 0; h < 64; h += 8) for (t = 0; t < 64; t += 8) {
        for (n = 8; n <= 65; ++n) {
            unsigned distance = t > h ? t - h : 64 - h + t;
            unsigned padded = (n + 7) & ~7u;
            bool fits = distance >= 16 && padded <= distance - 16;
            reset();
            descriptor[0] = t;
            descriptor[1] = h;
            memcpy(before, tx, sizeof(tx));
            assert(sweet_glink_smem_tx_room(&link, &room) == SWEET_GLINK_OK);
            assert(room == (distance > 16 ? distance - 16 : 0));
            expected_head = (h + padded) % 64;
            expected_start = h;
            expected_length = n;
            expected_data = frame;
            assert(sweet_glink_smem_send(&link, frame, n) == (fits ? SWEET_GLINK_OK : SWEET_GLINK_AGAIN));
            assert(notifications == (fits ? 1u : 0u));
            assert(descriptor[0] == t && descriptor[2] == 0 && descriptor[3] == 0);
            if (!fits) assert(memcmp(tx, before, sizeof(tx)) == 0 && descriptor[1] == h);
        }
        reset();
        descriptor[2] = t;
        descriptor[3] = h;
        for (i = 0; i < sizeof(rx); ++i) rx[i] = (uint8_t)i;
        assert(sweet_glink_smem_rx_available(&link, &available) == SWEET_GLINK_OK);
        assert(available == (h >= t ? h - t : 64 - t + h));
        if (available) {
            assert(sweet_glink_smem_peek(&link, 0, output, available) == SWEET_GLINK_OK);
            for (i = 0; i < available; ++i) assert(output[i] == (t + i) % 64);
            assert(sweet_glink_smem_consume(&link, available) == SWEET_GLINK_OK);
            assert(descriptor[2] == h);
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "binding")) binding();
    else if (!strcmp(argv[1], "transmit")) transmit();
    else if (!strcmp(argv[1], "backpressure")) backpressure();
    else if (!strcmp(argv[1], "receive")) receive();
    else if (!strcmp(argv[1], "corruption")) corruption();
    else if (!strcmp(argv[1], "exhaustive")) exhaustive();
    else return 2;
    puts("PASS");
    return 0;
}
