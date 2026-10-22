/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "glink_channel.h"

static uint32_t descriptor[8];
static uint8_t tx[64], rx[64];
static struct sweet_glink_smem link;
static struct sweet_glink_control control;
static struct sweet_glink_channel channel;
static unsigned notifications;
static const uint8_t ack1[8] = {1, 0, 1, 0, 0, 0, 0, 0};

static void notify(void *context) { ++*(unsigned *)context; }

static void header(uint8_t *out, uint16_t kind, uint16_t id, uint32_t param)
{
    out[0] = (uint8_t)kind; out[1] = (uint8_t)(kind >> 8);
    out[2] = (uint8_t)id; out[3] = (uint8_t)(id >> 8);
    out[4] = (uint8_t)param; out[5] = (uint8_t)(param >> 8);
    out[6] = (uint8_t)(param >> 16); out[7] = (uint8_t)(param >> 24);
}

static void receive(const uint8_t *frame, size_t bytes)
{
    size_t i, padded = (bytes + 7u) & ~(size_t)7u;
    uint32_t head = descriptor[3];
    for (i = 0; i < padded; ++i)
        rx[(head + i) % 64] = i < bytes ? frame[i] : 0xa5;
    descriptor[3] = (uint32_t)((head + padded) % 64);
}

static void command(uint16_t kind, uint16_t id)
{
    uint8_t frame[8];
    header(frame, kind, id, 0);
    receive(frame, 8);
}

static void remote_open(uint16_t id, const char *name, size_t bytes)
{
    uint8_t frame[40];
    assert(bytes <= 32);
    header(frame, 2, id, (uint32_t)bytes);
    memcpy(frame + 8, name, bytes);
    receive(frame, 8 + bytes);
}

static void reset(void)
{
    memset(descriptor, 0, sizeof(descriptor));
    memset(tx, 0xa5, sizeof(tx)); memset(rx, 0x5a, sizeof(rx));
    memset(&channel, 0, sizeof(channel));
    notifications = 0;
    assert(sweet_glink_smem_bind(&link, descriptor, sizeof(descriptor), tx, 64,
        rx, 64, notify, &notifications) == 0);
    assert(sweet_glink_control_init(&control, &link) == 0);
    assert(sweet_glink_control_step(&control) == 0);
    receive(ack1, 8);
    assert(sweet_glink_control_step(&control) == 0 && control.ready);
    descriptor[0] = descriptor[1];
    assert(sweet_glink_channel_init(&channel, &control, 0x1234, "IPCRTR", 7) == 0);
}

static void start(void)
{
    reset();
    assert(sweet_glink_channel_step(&channel) == 0 && channel.open_sent);
}

static void ready(void)
{
    start();
    remote_open(0xabcd, "IPCRTR", 7);
    assert(sweet_glink_channel_step(&channel) == 0);
    command(4, 0x1234);
    assert(sweet_glink_channel_step(&channel) == 0);
    assert(sweet_glink_channel_ready(&channel));
    descriptor[0] = descriptor[1];
}

static void open_test(void)
{
    static const uint8_t expected[16] = {
        2, 0, 0x34, 0x12, 7, 0, 0, 0, 'I', 'P', 'C', 'R', 'T', 'R', 0, 0};
    static const uint8_t ack_remote[8] = {4, 0, 0xcd, 0xab, 0, 0, 0, 0};
    start();
    assert(memcmp(tx + 8, expected, 16) == 0 && descriptor[1] == 24);
    assert(!sweet_glink_channel_ready(&channel));
    command(4, 0x1234); /* ACK can precede the peer's OPEN. */
    assert(sweet_glink_channel_step(&channel) == 0);
    assert(channel.open_acked && !sweet_glink_channel_ready(&channel));
    remote_open(0xabcd, "IPCRTR", 7);
    assert(sweet_glink_channel_step(&channel) == 0);
    assert(memcmp(tx + 24, ack_remote, 8) == 0);
    assert(channel.remote_id == 0xabcd && sweet_glink_channel_ready(&channel));
    remote_open(0xabcd, "IPCRTR", 7); /* Duplicate: re-ACK, no ID rebinding. */
    assert(sweet_glink_channel_step(&channel) == 0);
    assert(memcmp(tx + 32, ack_remote, 8) == 0 && sweet_glink_channel_ready(&channel));
    command(4, 0x1234);
    assert(sweet_glink_channel_step(&channel) == 0 && sweet_glink_channel_ready(&channel));
    /* A remote-initiated OPEN must get its ACK before our first OPEN. */
    reset();
    remote_open(0xabcd, "IPCRTR", 7);
    assert(sweet_glink_channel_step(&channel) == 0);
    assert(memcmp(tx + 8, ack_remote, 8) == 0 && !channel.open_sent);
    assert(sweet_glink_channel_step(&channel) == 0 && channel.open_sent);
    assert(memcmp(tx + 16, expected, 16) == 0);
    command(4, 0x1234);
    assert(sweet_glink_channel_step(&channel) == 0 && sweet_glink_channel_ready(&channel));
}

struct endpoint {
    uint32_t d[8];
    struct sweet_glink_smem link;
    struct sweet_glink_control control;
    struct sweet_glink_channel channel;
    unsigned notifications;
};

static void sync(struct endpoint *a, struct endpoint *b)
{
    b->d[3] = a->d[1]; b->d[0] = a->d[2];
    a->d[3] = b->d[1]; a->d[0] = b->d[2];
}

static void peers(void)
{
    struct endpoint a, b;
    uint8_t ab[128], ba[128];
    unsigned i;
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    memset(ab, 0, sizeof(ab)); memset(ba, 0, sizeof(ba));
    assert(sweet_glink_smem_bind(&a.link, a.d, 32, ab, 128, ba, 128,
        notify, &a.notifications) == 0);
    assert(sweet_glink_smem_bind(&b.link, b.d, 32, ba, 128, ab, 128,
        notify, &b.notifications) == 0);
    assert(sweet_glink_control_init(&a.control, &a.link) == 0);
    assert(sweet_glink_control_init(&b.control, &b.link) == 0);
    for (i = 0; i < 12; ++i) {
        int r = sweet_glink_control_step(&a.control);
        assert(r == 0 || r == SWEET_GLINK_AGAIN); sync(&a, &b);
        r = sweet_glink_control_step(&b.control);
        assert(r == 0 || r == SWEET_GLINK_AGAIN); sync(&a, &b);
    }
    assert(a.control.ready && b.control.ready);
    assert(sweet_glink_channel_init(&a.channel, &a.control, 1, "IPCRTR", 7) == 0);
    assert(sweet_glink_channel_init(&b.channel, &b.control, 7, "IPCRTR", 7) == 0);
    for (i = 0; i < 12; ++i) {
        int r = sweet_glink_channel_step(&a.channel);
        assert(r == 0 || r == SWEET_GLINK_AGAIN); sync(&a, &b);
        r = sweet_glink_channel_step(&b.channel);
        assert(r == 0 || r == SWEET_GLINK_AGAIN); sync(&a, &b);
    }
    assert(sweet_glink_channel_ready(&a.channel) && sweet_glink_channel_ready(&b.channel));
    assert(a.channel.remote_id == 7 && b.channel.remote_id == 1);
    assert(sweet_glink_channel_close(&a.channel) == 0);
    for (i = 0; i < 16; ++i) {
        int r = sweet_glink_channel_step(&a.channel);
        assert(r == 0 || r == SWEET_GLINK_AGAIN); sync(&a, &b);
        r = sweet_glink_channel_step(&b.channel);
        assert(r == 0 || r == SWEET_GLINK_AGAIN); sync(&a, &b);
    }
    assert(sweet_glink_channel_closed(&a.channel) && sweet_glink_channel_closed(&b.channel));
    assert(!sweet_glink_channel_ready(&a.channel) && !sweet_glink_channel_ready(&b.channel));
}

static void blocked(void)
{
    uint8_t fill[32] = {0};
    uint8_t fill_close[48] = {0};
    reset();
    descriptor[1] = 0; descriptor[0] = 16;
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_AGAIN);
    assert(!channel.open_sent && channel.pending_bytes == 15);
    descriptor[0] = 0;
    assert(sweet_glink_channel_step(&channel) == 0 && channel.open_sent);
    assert(sweet_glink_smem_send(&link, fill, sizeof(fill)) == 0);
    remote_open(0xabcd, "IPCRTR", 7);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_AGAIN);
    assert(channel.remote_id == 0xabcd && !channel.remote_open_acked);
    assert(channel.pending[0] == 4 && channel.pending_bytes == 8);
    command(4, 0x1234);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_AGAIN);
    assert(!channel.open_acked && !sweet_glink_channel_ready(&channel));
    descriptor[0] = descriptor[1];
    assert(sweet_glink_channel_step(&channel) == 0 && channel.remote_open_acked);
    assert(sweet_glink_channel_step(&channel) == 0 && sweet_glink_channel_ready(&channel));
    ready();
    assert(sweet_glink_smem_send(&link, fill_close, 48) == 0);
    assert(sweet_glink_channel_close(&channel) == 0);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_AGAIN && !channel.close_sent);
    descriptor[0] = descriptor[1];
    assert(sweet_glink_channel_step(&channel) == 0 && channel.close_sent);
    assert(sweet_glink_smem_send(&link, fill_close, 40) == 0);
    command(3, 0xabcd);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_AGAIN);
    assert(channel.remote_closed && !channel.remote_close_acked && !sweet_glink_channel_closed(&channel));
    command(11, 0x1234);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_AGAIN && !channel.close_acked);
    descriptor[0] = descriptor[1];
    assert(sweet_glink_channel_step(&channel) == 0 && channel.remote_close_acked);
    assert(sweet_glink_channel_step(&channel) == 0 && sweet_glink_channel_closed(&channel));
}

static void close_test(void)
{
    unsigned remote_first;
    for (remote_first = 0; remote_first < 2; ++remote_first) {
        ready();
        if (remote_first) {
            command(3, 0xabcd);
            assert(sweet_glink_channel_step(&channel) == 0);
            assert(channel.remote_closed && channel.remote_close_acked && !channel.close_sent);
        } else {
            assert(sweet_glink_channel_close(&channel) == 0);
            assert(sweet_glink_channel_close(&channel) == 0); /* Idempotent. */
        }
        assert(!sweet_glink_channel_ready(&channel));
        assert(sweet_glink_channel_step(&channel) == 0 && channel.close_sent);
        if (!remote_first) {
            command(3, 0xabcd);
            assert(sweet_glink_channel_step(&channel) == 0 && channel.remote_close_acked);
        }
        assert(!sweet_glink_channel_closed(&channel));
        command(11, 0x1234);
        assert(sweet_glink_channel_step(&channel) == 0 && sweet_glink_channel_closed(&channel));
        descriptor[0] = descriptor[1];
        command(3, 0xabcd); /* Duplicate close receives another ACK. */
        assert(sweet_glink_channel_step(&channel) == 0 && sweet_glink_channel_closed(&channel));
        remote_open(0xabcd, "IPCRTR", 7);
        assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CONTROL_PROTOCOL);
        assert(sweet_glink_channel_closed(&channel));
    }
    /* Peer aborts before we publish our ID: no unknown-ID CLOSE is emitted. */
    reset(); remote_open(0xabcd, "IPCRTR", 7);
    assert(sweet_glink_channel_step(&channel) == 0 && !channel.open_sent);
    command(3, 0xabcd);
    assert(sweet_glink_channel_step(&channel) == 0 && sweet_glink_channel_closed(&channel));
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_AGAIN && !channel.close_sent);
}

static void malformed(void)
{
    uint8_t frame[16];
    static const uint32_t bad_lengths[] = {0, 1, 33, UINT32_MAX};
    unsigned i;
    for (i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); ++i) {
        start(); header(frame, 2, 3, bad_lengths[i]); receive(frame, 8);
        assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CONTROL_PROTOCOL);
        assert(descriptor[2] == 8);
    }
    start(); header(frame, 2, 3, 7); receive(frame, 8);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_AGAIN && descriptor[2] == 8);
    receive((const uint8_t *)"IPCRTR\0", 8);
    assert(sweet_glink_channel_step(&channel) == 0 && channel.remote_id == 3);
    for (i = 0; i < 2; ++i) {
        start(); header(frame, 2, 3, 7); memcpy(frame + 8, "IPCRTR", 7);
        frame[8 + (i ? 6 : 1)] = i ? 'X' : 0;
        receive(frame, 15);
        assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CONTROL_PROTOCOL);
        assert(descriptor[2] == 8);
    }
    start(); remote_open(3, "OTHER", 6);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CONTROL_UNHANDLED);
    assert(descriptor[2] == 8);
    start(); command(4, 0x5678);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CONTROL_UNHANDLED);
    assert(descriptor[2] == 8);
    start(); command(11, 0x1234);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CONTROL_PROTOCOL);
    start(); command(4, 0);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CONTROL_PROTOCOL);
    ready(); remote_open(3, "IPCRTR", 7);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CONTROL_PROTOCOL);
    assert(channel.remote_id == 0xabcd);
    ready(); command(5, 0xabcd); /* INTENT belongs to the future data layer. */
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CONTROL_UNHANDLED);
}

static void control_test(void)
{
    uint8_t frame[8];
    ready();
    command(13, 0);
    assert(sweet_glink_channel_step(&channel) == 0 && sweet_glink_channel_ready(&channel));
    header(frame, 1, 1, 1); receive(frame, 8); /* Unsupported feature counter-offer. */
    assert(sweet_glink_channel_step(&channel) == 0);
    assert(channel.invalidated && !sweet_glink_channel_ready(&channel));
    receive(ack1, 8);
    assert(sweet_glink_control_step(&control) == 0 && control.ready);
    assert(sweet_glink_channel_step(&channel) == SWEET_GLINK_CHANNEL_STATE);
    assert(!sweet_glink_channel_ready(&channel));
}

static void wrap(void)
{
    char name[32];
    unsigned length, pos, i;
    for (length = 2; length <= 32; ++length) {
        memset(name, 'A', length); name[length - 1] = 0;
        for (pos = 0; pos < 64; pos += 8) {
            reset();
            descriptor[0] = descriptor[1] = pos;
            descriptor[2] = descriptor[3] = pos;
            assert(sweet_glink_channel_init(&channel, &control, 1, name, length) == 0);
            assert(sweet_glink_channel_step(&channel) == 0);
            for (i = 0; i < length; ++i)
                assert(tx[(pos + 8 + i) % 64] == (uint8_t)name[i]);
            remote_open(7, name, length);
            assert(sweet_glink_channel_step(&channel) == 0);
            command(4, 1);
            assert(sweet_glink_channel_step(&channel) == 0 && sweet_glink_channel_ready(&channel));
        }
    }
}

static void arguments(void)
{
    struct sweet_glink_channel before;
    reset(); before = channel;
    assert(sweet_glink_channel_init(NULL, &control, 1, "IPCRTR", 7) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_channel_init(&channel, NULL, 1, "IPCRTR", 7) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_channel_init(&channel, &control, 0, "IPCRTR", 7) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_channel_init(&channel, &control, 1, "IPCRTR", 6) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_channel_init(&channel, &control, 1, "", 1) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_channel_init(&channel, &control, 1, "A\0B", 4) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_channel_init((struct sweet_glink_channel *)(void *)tx,
        &control, 1, "IPCRTR", 7) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_channel_init((struct sweet_glink_channel *)(void *)&control,
        &control, 1, "IPCRTR", 7) == SWEET_GLINK_ARGUMENT);
    control.ready = false;
    assert(sweet_glink_channel_init(&channel, &control, 1, "IPCRTR", 7) == SWEET_GLINK_CHANNEL_STATE);
    assert(memcmp(&before, &channel, sizeof(channel)) == 0);
    assert(!sweet_glink_channel_ready(NULL) && !sweet_glink_channel_closed(NULL));
    assert(sweet_glink_channel_step(NULL) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_channel_close(NULL) == SWEET_GLINK_ARGUMENT);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "open")) open_test();
    else if (!strcmp(argv[1], "peers")) peers();
    else if (!strcmp(argv[1], "blocked")) blocked();
    else if (!strcmp(argv[1], "close")) close_test();
    else if (!strcmp(argv[1], "malformed")) malformed();
    else if (!strcmp(argv[1], "control")) control_test();
    else if (!strcmp(argv[1], "wrap")) wrap();
    else if (!strcmp(argv[1], "arguments")) arguments();
    else assert(0);
    puts("PASS");
    return 0;
}
