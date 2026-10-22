/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "glink_control.h"

static uint32_t descriptor[8];
static uint8_t tx[64], rx[64];
static struct sweet_glink_smem link;
static struct sweet_glink_control state;
static unsigned notifications;
static bool check_tail;
static uint32_t expected_tail;
static const uint8_t version1[8] = {0, 0, 1, 0, 0, 0, 0, 0};
static const uint8_t ack1[8] = {1, 0, 1, 0, 0, 0, 0, 0};

static void notify(void *context)
{
    assert(context == &notifications);
    if (check_tail)
        assert(descriptor[2] == expected_tail);
    ++notifications;
}

static void reset(void)
{
    memset(descriptor, 0, sizeof(descriptor));
    memset(tx, 0xa5, sizeof(tx));
    memset(rx, 0x5a, sizeof(rx));
    memset(&state, 0, sizeof(state));
    check_tail = false;
    notifications = 0;
    assert(sweet_glink_smem_bind(&link, descriptor, sizeof(descriptor),
        tx, sizeof(tx), rx, sizeof(rx), notify, &notifications) == 0);
    assert(sweet_glink_control_init(&state, &link) == 0);
    assert(notifications == 0 && descriptor[1] == 0);
}

static void receive(const uint8_t *frame)
{
    unsigned i;
    uint32_t head = descriptor[3];
    for (i = 0; i < 8; ++i)
        rx[(head + i) % 64] = frame[i];
    descriptor[3] = (head + 8) % 64;
}

static void start(void)
{
    reset();
    assert(sweet_glink_control_step(&state) == 0);
    assert(state.local_version_sent && !state.has_pending && !state.ready);
    assert(memcmp(tx, version1, 8) == 0);
}

static void vectors(void)
{
    start();
    assert(notifications == 1);
    assert(sweet_glink_control_step(&state) == SWEET_GLINK_AGAIN);
    receive(version1);
    assert(sweet_glink_control_step(&state) == 0);
    assert(memcmp(tx + 8, ack1, 8) == 0 && descriptor[2] == 8);
    assert(!state.ready && notifications == 2);
    receive(ack1);
    assert(sweet_glink_control_step(&state) == 0);
    assert(state.ready && !state.has_pending && !state.failed);
    receive(ack1);
    assert(sweet_glink_control_step(&state) == 0 && state.ready);
    /* VERSION can arrive later than the ACK; it isn't proof of a restart. */
    receive(version1);
    assert(sweet_glink_control_step(&state) == 0 && state.ready);
    assert(memcmp(tx + 16, ack1, 8) == 0);
}

static void count_only(void *context) { ++*(unsigned *)context; }

static void peers(void)
{
    uint32_t other_descriptor[8] = {0};
    struct sweet_glink_smem other_link;
    struct sweet_glink_control other;
    unsigned other_notifications = 0, i;
    reset();
    assert(sweet_glink_smem_bind(&other_link, other_descriptor, sizeof(other_descriptor),
        rx, sizeof(rx), tx, sizeof(tx), count_only, &other_notifications) == 0);
    assert(sweet_glink_control_init(&other, &other_link) == 0);
    for (i = 0; i < 12; ++i) {
        int a = sweet_glink_control_step(&state);
        int b;
        assert(a == 0 || a == SWEET_GLINK_AGAIN);
        /* Mirror the two views of each shared index, preserving ownership. */
        other_descriptor[3] = descriptor[1];
        other_descriptor[0] = descriptor[2];
        b = sweet_glink_control_step(&other);
        assert(b == 0 || b == SWEET_GLINK_AGAIN);
        descriptor[3] = other_descriptor[1];
        descriptor[0] = other_descriptor[2];
    }
    assert(state.ready && other.ready);
    assert(notifications == 2 && other_notifications == 2);
    assert(descriptor[1] == descriptor[0] && descriptor[3] == descriptor[2]);
}

static void counteroffer(void)
{
    uint8_t frame[8];
    unsigned kind, v;
    for (kind = 0; kind < 2; ++kind) {
        for (v = 1; v <= 3; ++v) {
            start();
            memcpy(frame, version1, 8);
            frame[0] = (uint8_t)kind;
            frame[2] = (uint8_t)v;
            frame[7] = 0x80; /* High, unknown feature must never be enabled. */
            receive(frame);
            assert(sweet_glink_control_step(&state) == 0 && !state.ready);
            assert(memcmp(tx + 8, kind ? version1 : ack1, 8) == 0);
            receive(ack1);
            assert(sweet_glink_control_step(&state) == 0 && state.ready);
        }
    }
}

static void blocked(void)
{
    uint8_t fill[40] = {0};
    struct sweet_glink_control before;
    start();
    assert(sweet_glink_smem_send(&link, fill, sizeof(fill)) == 0);
    receive(version1);
    assert(sweet_glink_control_step(&state) == SWEET_GLINK_AGAIN);
    assert(descriptor[2] == 8 && descriptor[1] == 48 && state.has_pending);
    assert(memcmp(state.pending, ack1, 8) == 0);
    before = state;
    assert(sweet_glink_control_step(&state) == SWEET_GLINK_AGAIN);
    assert(memcmp(&before, &state, sizeof(state)) == 0 && descriptor[2] == 8);
    assert(sweet_glink_smem_request_space(&link) == 0 && descriptor[1] == 56);
    assert(tx[48] == 13 && notifications == 3);
    receive(ack1);
    assert(sweet_glink_control_step(&state) == SWEET_GLINK_AGAIN);
    assert(descriptor[2] == 8 && !state.ready);
    descriptor[0] = 48; /* Peer frees space. */
    assert(sweet_glink_control_step(&state) == 0);
    assert(memcmp(tx + 56, ack1, 8) == 0 && descriptor[1] == 0);
    assert(!state.has_pending && descriptor[2] == 8 && notifications == 4);
    assert(sweet_glink_control_step(&state) == 0 && state.ready);
    assert(descriptor[2] == 16 && notifications == 4);
    /* The initial VERSION can be blocked without pretending it was sent. */
    reset();
    descriptor[1] = 48;
    assert(sweet_glink_control_step(&state) == SWEET_GLINK_AGAIN);
    assert(!state.local_version_sent && state.has_pending);
    descriptor[0] = 48;
    assert(sweet_glink_control_step(&state) == 0 && state.local_version_sent);
}

static void read_notif(void)
{
    static const uint8_t request[8] = {13, 0, 0, 0, 0, 0, 0, 0};
    uint8_t invalid[8];
    start();
    receive(request);
    check_tail = true;
    expected_tail = 8;
    assert(sweet_glink_control_step(&state) == 0);
    assert(notifications == 2 && descriptor[1] == 8);
    memcpy(invalid, request, 8);
    invalid[2] = 1;
    receive(invalid);
    assert(sweet_glink_control_step(&state) == SWEET_GLINK_CONTROL_PROTOCOL);
    assert(descriptor[2] == 8 && notifications == 2);
}

static void errors(void)
{
    uint8_t frame[8] = {0};
    unsigned kind;
    for (kind = 0; kind < 2; ++kind) {
        start();
        frame[0] = (uint8_t)kind;
        receive(frame);
        assert(sweet_glink_control_step(&state) == SWEET_GLINK_CONTROL_REJECTED);
        assert(state.failed && !state.ready && descriptor[2] == 8);
        assert(sweet_glink_control_step(&state) == SWEET_GLINK_CONTROL_REJECTED);
        assert(notifications == 1);
    }
    start();
    frame[0] = 2; /* OPEN belongs to the channel layer, including its name. */
    receive(frame);
    assert(sweet_glink_control_step(&state) == SWEET_GLINK_CONTROL_UNHANDLED);
    assert(descriptor[2] == 0 && notifications == 1);
    descriptor[3] = 7;
    assert(sweet_glink_control_step(&state) == SWEET_GLINK_CORRUPT);
    assert(descriptor[2] == 0 && notifications == 1);
}

static void wrap(void)
{
    unsigned position;
    for (position = 0; position < 64; position += 8) {
        unsigned i;
        reset();
        descriptor[0] = descriptor[1] = position;
        descriptor[2] = descriptor[3] = position;
        assert(sweet_glink_control_step(&state) == 0);
        for (i = 0; i < 8; ++i)
            assert(tx[(position + i) % 64] == version1[i]);
        receive(version1);
        assert(sweet_glink_control_step(&state) == 0);
        for (i = 0; i < 8; ++i)
            assert(tx[(position + 8 + i) % 64] == ack1[i]);
        receive(ack1);
        assert(sweet_glink_control_step(&state) == 0 && state.ready);
        assert(descriptor[2] == (position + 16) % 64);
    }
}

static void arguments(void)
{
    struct sweet_glink_control before;
    uint8_t saved[64];
    reset();
    before = state;
    memcpy(saved, tx, 64);
    assert(sweet_glink_control_init(NULL, &link) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_control_init(&state, NULL) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_control_step(NULL) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_control_init((struct sweet_glink_control *)(void *)tx,
        &link) == SWEET_GLINK_ARGUMENT);
    assert(sweet_glink_control_init((struct sweet_glink_control *)(void *)&link,
        &link) == SWEET_GLINK_ARGUMENT);
    descriptor[1] = 1;
    assert(sweet_glink_control_init(&state, &link) == SWEET_GLINK_CORRUPT);
    assert(memcmp(saved, tx, 64) == 0);
    assert(memcmp(&before, &state, sizeof(state)) == 0);
    assert(notifications == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "vectors")) vectors();
    else if (!strcmp(argv[1], "peers")) peers();
    else if (!strcmp(argv[1], "counteroffer")) counteroffer();
    else if (!strcmp(argv[1], "blocked")) blocked();
    else if (!strcmp(argv[1], "read-notif")) read_notif();
    else if (!strcmp(argv[1], "errors")) errors();
    else if (!strcmp(argv[1], "wrap")) wrap();
    else if (!strcmp(argv[1], "arguments")) arguments();
    else assert(0);
    puts("PASS");
    return 0;
}
