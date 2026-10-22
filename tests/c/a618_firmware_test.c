/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "firmware.h"

static uint32_t registers[0x31000 / 4];
static uint8_t microcode[A618_GMU_ITCM_BYTES + 4];

static void initialize(void)
{
    unsigned i;
    for (i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) registers[i] = 0xa5a5a5a5;
    for (i = 0; i < sizeof(microcode); ++i) microcode[i] = (uint8_t)(i * 17);
    registers[A618_GMU_SYSRESET_OFFSET / 4] = 1;
}

static void upload(void)
{
    unsigned i;
    initialize();
    assert(a618_gmu_upload(registers, sizeof(registers), 0x06010800,
        microcode, A618_GMU_ITCM_BYTES, true) == A618_FIRMWARE_OK);
    assert(memcmp((uint8_t *)registers + A618_GMU_ITCM_OFFSET,
        microcode, A618_GMU_ITCM_BYTES) == 0);
    for (i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        if (i >= A618_GMU_ITCM_OFFSET / 4 && i < (A618_GMU_ITCM_OFFSET + A618_GMU_ITCM_BYTES) / 4)
            continue;
        assert(registers[i] == (i == A618_GMU_SYSRESET_OFFSET / 4 ? 1u : 0xa5a5a5a5u));
    }
    initialize();
    assert(a618_gmu_upload(registers, sizeof(registers), 0x06010801,
        microcode + 1, 4, false) == A618_FIRMWARE_OK);
    assert(memcmp((uint8_t *)registers + A618_GMU_ITCM_OFFSET, microcode + 1, 4) == 0);
    assert(registers[(A618_GMU_ITCM_OFFSET + 4) / 4] == 0xa5a5a5a5);
}

static void validation(void)
{
    static uint32_t saved[sizeof(registers) / sizeof(registers[0])];
    const size_t sizes[] = {0, 1, 3, 32767, 32769, 32772, SIZE_MAX};
    const uint32_t chips[] = {0, 530, 618, 0x05030000, 0x06030000, 0x06010900};
    unsigned i;
    initialize();
    memcpy(saved, registers, sizeof(registers));
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        assert(a618_gmu_upload(registers, sizeof(registers), 0x06010800,
            microcode, sizes[i], false) == A618_FIRMWARE_ARGUMENT);
    for (i = 0; i < sizeof(chips) / sizeof(chips[0]); ++i)
        assert(a618_gmu_upload(registers, sizeof(registers), chips[i],
            microcode, 32, false) == A618_FIRMWARE_ARGUMENT);
    assert(a618_gmu_upload(registers, A618_GMU_SYSRESET_OFFSET, 0x06010800,
        microcode, 32, false) == A618_FIRMWARE_ARGUMENT);
    assert(a618_gmu_upload(registers, SIZE_MAX, 0x06010800,
        microcode, 32, false) == A618_FIRMWARE_ARGUMENT);
    assert(a618_gmu_upload(registers, sizeof(registers), 0x06010800,
        registers, 32, false) == A618_FIRMWARE_ARGUMENT);
    assert(a618_gmu_upload(NULL, sizeof(registers), 0x06010800,
        microcode, 32, false) == A618_FIRMWARE_ARGUMENT);
    assert(memcmp(saved, registers, sizeof(registers)) == 0);
    registers[A618_GMU_SYSRESET_OFFSET / 4] = 0;
    memcpy(saved, registers, sizeof(registers));
    assert(a618_gmu_upload(registers, sizeof(registers), 0x06010800,
        microcode, 32, true) == A618_FIRMWARE_RESET);
    assert(memcmp(saved, registers, sizeof(registers)) == 0);
}

static void sqe(void)
{
    const uint8_t image[12] = {0x12, 0x34, 0x56, 0x78, 1, 2, 3, 4, 5, 6, 7, 8};
    const size_t sizes[] = {0, 1, 4, 7, 9, SIZE_MAX};
    struct a618_sqe_view view, saved;
    unsigned i;
    assert(a618_sqe_code(image, sizeof(image), &view) == A618_FIRMWARE_OK);
    assert(view.code == image + 4 && view.bytes == 8 && view.file_header == 0x78563412);
    saved = view;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        assert(a618_sqe_code(image, sizes[i], &view) == A618_FIRMWARE_ARGUMENT);
        assert(memcmp(&saved, &view, sizeof(view)) == 0);
    }
    assert(a618_sqe_code(&view, sizeof(view), &view) == A618_FIRMWARE_ARGUMENT);
    assert(a618_sqe_code(NULL, 8, &view) == A618_FIRMWARE_ARGUMENT);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "upload")) upload();
    else if (!strcmp(argv[1], "validation")) validation();
    else if (!strcmp(argv[1], "sqe")) sqe();
    else return 2;
    puts("PASS");
    return 0;
}
