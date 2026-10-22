/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "glink_smem.h"
#include "smem_view.h"

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

static uint32_t smem_words[2048];

static void put16(uint8_t *base, size_t offset, uint16_t value)
{
    base[offset] = (uint8_t)value;
    base[offset + 1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *base, size_t offset, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) base[offset + i] = (uint8_t)(value >> (8 * i));
}

static uint8_t *smem_fixture(void)
{
    uint8_t *s = (uint8_t *)smem_words;
    const uint16_t items[] = {478, 479, 480};
    const uint32_t sizes[] = {32, 64, 64};
    size_t entry = 512 + 32;
    unsigned i;
    memset(smem_words, 0, sizeof(smem_words));
    put32(s, 92, 12);
    put32(s, 192, 1);
    memcpy(s + 4096, "$TOC", 4);
    put32(s, 4096 + 4, 1);
    put32(s, 4096 + 8, 1);
    put32(s, 4096 + 32, 512);
    put32(s, 4096 + 36, 2048);
    put16(s, 4096 + 44, 0);
    put16(s, 4096 + 46, 1);
    put32(s, 4096 + 48, 32);
    memcpy(s + 512, "$PRT", 4);
    put16(s, 512 + 4, 0);
    put16(s, 512 + 6, 1);
    put32(s, 512 + 8, 2048);
    put32(s, 512 + 16, 2048);
    for (i = 0; i < 3; ++i) {
        put16(s, entry, 0xa5a5);
        put16(s, entry + 2, items[i]);
        put32(s, entry + 4, sizes[i]);
        entry += 16 + sizes[i];
    }
    put32(s, 512 + 12, (uint32_t)(entry - 512));
    return s;
}

static void smem_notify(void *context)
{
    uint8_t *s = (uint8_t *)smem_words;
    static const uint8_t packet[8] = {0, 0, 1, 0, 0, 0, 0, 0};
    assert(context == &notifications);
    assert(smem_words[560 / 4 + 1] == 8);
    assert(memcmp(s + 608, packet, 8) == 0);
    ++notifications;
}

static void smem(void)
{
    uint8_t *s = smem_fixture();
    struct sweet_smem_view view;
    struct sweet_glink_smem bound;
    static const uint8_t packet[8] = {0, 0, 1, 0, 0, 0, 0, 0};
    assert(sweet_smem_find_uncached(s, 8192, 1, 478, &view) == SWEET_GLINK_OK);
    assert(view.data == s + 560 && view.size == 32);
    assert(sweet_smem_find_uncached(s, 8192, 1, 479, &view) == SWEET_GLINK_OK);
    assert(view.data == s + 608 && view.size == 64);
    assert(sweet_smem_find_uncached(s, 8192, 1, 480, &view) == SWEET_GLINK_OK);
    assert(view.data == s + 688 && view.size == 64);
    notifications = 0;
    assert(sweet_glink_smem_bind_existing(&bound, s, 8192, 1, smem_notify,
        &notifications) == SWEET_GLINK_OK);
    assert(sweet_glink_smem_send(&bound, packet, sizeof(packet)) == SWEET_GLINK_OK);
    assert(notifications == 1);
    s = smem_fixture();
    put16(s, 4096 + 44, 1); put16(s, 4096 + 46, 0);
    put16(s, 512 + 4, 1); put16(s, 512 + 6, 0);
    assert(sweet_smem_find_uncached(s, 8192, 1, 478, &view) == SWEET_GLINK_OK);
    put16(s, 544 + 8, 8); /* allocated 32, logical 24 */
    assert(sweet_smem_find_uncached(s, 8192, 1, 478, &view) == SWEET_GLINK_OK && view.size == 24);
    assert(sweet_glink_smem_bind_existing(&bound, s, 8192, 1, smem_notify,
        &notifications) == SWEET_GLINK_ARGUMENT);
    s = smem_fixture();
    memmove(s + 568, s + 560, 32);
    put16(s, 544 + 10, 8); /* allowed padding between header and data */
    put32(s, 512 + 12, 88); /* only this item is published */
    assert(sweet_smem_find_uncached(s, 8192, 1, 478, &view) == SWEET_GLINK_OK);
    assert(view.data == s + 568 && view.size == 32);
}

static void smem_malformed(void)
{
    /* Mutate bounds/canaries after and before the requested item too. */
    static const size_t offsets[] = {192, 204, 4096, 4104, 4128, 4132,
        512, 520, 524, 528, 544, 548, 552, 554, 672, 676};
    struct sweet_smem_view view;
    struct sweet_glink_smem bound, saved;
    unsigned i;
    uint8_t *s;
    memset(&saved, 0x44, sizeof(saved));
    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        s = smem_fixture();
        put32(s, offsets[i], UINT32_MAX);
        view.data = NULL; view.size = 777;
        bound = saved;
        assert(sweet_smem_find_uncached(s, 8192, 1, 478, &view) != SWEET_GLINK_OK);
        assert(view.data == NULL && view.size == 777);
        assert(sweet_glink_smem_bind_existing(&bound, s, 8192, 1, smem_notify,
            &notifications) != SWEET_GLINK_OK);
        assert(memcmp(&bound, &saved, sizeof(bound)) == 0);
    }
    s = smem_fixture();
    put32(s, 92, 10);
    assert(sweet_smem_find_uncached(s, 8192, 1, 478, &view) == SWEET_SMEM_UNSUPPORTED);
    s = smem_fixture();
    put16(s, 674, 478); /* duplicate item */
    assert(sweet_smem_find_uncached(s, 8192, 1, 478, &view) == SWEET_SMEM_FORMAT);
    s = smem_fixture();
    put32(s, 4104, 2);
    memcpy(s + 4096 + 80, s + 4096 + 32, 48); /* duplicate AP/modem partition */
    assert(sweet_smem_find_uncached(s, 8192, 1, 478, &view) == SWEET_SMEM_FORMAT);
    s = smem_fixture();
    assert(sweet_smem_find_uncached(s, 8192, 2, 478, &view) == SWEET_SMEM_NOT_FOUND);
    assert(sweet_smem_find_uncached(s, 8192, 1, 999, &view) == SWEET_SMEM_NOT_FOUND);
    assert(sweet_smem_find_uncached(s, 8192, 0, 478, &view) == SWEET_SMEM_ARGUMENT);
    assert(sweet_smem_find_uncached(s + 1, 8192, 1, 478, &view) == SWEET_SMEM_ARGUMENT);
    assert(sweet_smem_find_uncached(s, 8191, 1, 478, &view) == SWEET_SMEM_ARGUMENT);
    assert(sweet_smem_find_uncached(s, SIZE_MAX, 1, 478, &view) == SWEET_SMEM_ARGUMENT);
    assert(sweet_smem_find_uncached(s, 8192, 1, 478,
        (struct sweet_smem_view *)(s + 768)) == SWEET_SMEM_ARGUMENT);
    assert(sweet_glink_smem_bind_existing((struct sweet_glink_smem *)(s + 768),
        s, 8192, 1, smem_notify, &notifications) == SWEET_SMEM_ARGUMENT);
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
    else if (!strcmp(argv[1], "smem")) smem();
    else if (!strcmp(argv[1], "smem-malformed")) smem_malformed();
    else return 2;
    puts("PASS");
    return 0;
}
