/* SPDX-License-Identifier: MIT
 * Independent A618 firmware upload primitives; format/register facts from
 * sweet KGSL A6xx/GMU sources. See docs/GPU_RESEARCH.md for references.
 */
#if !defined(KERNEL) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "firmware.h"

static uint32_t load_little(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t native_little(uint32_t value)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}

static void complete_io(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static bool separate(const void *source, size_t size,
    const volatile void *target, size_t target_size)
{
    uintptr_t a = (uintptr_t)source, b = (uintptr_t)target;
    return size <= UINTPTR_MAX - a && target_size <= UINTPTR_MAX - b &&
        (a + size <= b || b + target_size <= a);
}

int a618_gmu_upload(volatile uint32_t *gmu, size_t mapped_bytes,
    uint32_t chip_id, const void *firmware, size_t firmware_bytes, bool verify)
{
    const uint8_t *source = (const uint8_t *)firmware;
    size_t i;
    if (!gmu || !firmware || ((uintptr_t)gmu & 3u) ||
        (chip_id & UINT32_C(0xffffff00)) != UINT32_C(0x06010800) ||
        mapped_bytes < A618_GMU_SYSRESET_OFFSET + 4u || (mapped_bytes & 3u) ||
        !firmware_bytes || firmware_bytes > A618_GMU_ITCM_BYTES || (firmware_bytes & 3u) ||
        !separate(firmware, firmware_bytes, gmu, mapped_bytes))
        return A618_FIRMWARE_ARGUMENT;
    if (!(native_little(gmu[A618_GMU_SYSRESET_OFFSET / 4u]) & 1u))
        return A618_FIRMWARE_RESET;
    complete_io();
    for (i = 0; i < firmware_bytes; i += 4u)
        gmu[(A618_GMU_ITCM_OFFSET + i) / 4u] = native_little(load_little(source + i));
    complete_io();
    if (verify) {
        for (i = 0; i < firmware_bytes; i += 4u)
            if (native_little(gmu[(A618_GMU_ITCM_OFFSET + i) / 4u]) != load_little(source + i))
                return A618_FIRMWARE_VERIFY;
        complete_io();
    }
    if (!(native_little(gmu[A618_GMU_SYSRESET_OFFSET / 4u]) & 1u))
        return A618_FIRMWARE_RESET;
    return A618_FIRMWARE_OK;
}

int a618_sqe_code(const void *firmware, size_t bytes, struct a618_sqe_view *out)
{
    const uint8_t *data = (const uint8_t *)firmware;
    struct a618_sqe_view view;
    if (!firmware || !out || bytes < 8u || (bytes & 3u) || bytes > UINT32_MAX ||
        !separate(firmware, bytes, out, sizeof(*out)))
        return A618_FIRMWARE_ARGUMENT;
    view.code = data + 4u;
    view.bytes = bytes - 4u;
    view.file_header = load_little(data);
    *out = view;
    return A618_FIRMWARE_OK;
}
#endif
