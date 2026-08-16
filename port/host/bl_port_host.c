#include "bl_port.h"
#include "bl_port_host.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Host contract checks must remain enabled in all build configurations.
 * Standard assert() is unsuitable because NDEBUG removes assertions.
 */
#define BL_HOST_ASSERT(expr)                                             \
    do {                                                                 \
        if (!(expr)) {                                                   \
            fprintf(stderr,                                              \
                    "BL_HOST_ASSERT failed: %s (%s:%d)\n",                \
                    #expr, __FILE__, __LINE__);                          \
            abort();                                                     \
        }                                                                \
    } while (0)

/* STM32L432-like simulated memory geometry. */
#define HOST_FLASH_BASE             0x08000000u
#define HOST_FLASH_SIZE             (256u * 1024u)

#define HOST_SRAM_BASE              0x20000000u
#define HOST_SRAM_END               0x20010000u

#define HOST_WRITE_GRANULARITY      8u
#define HOST_ERASE_GRANULARITY      2048u

#define HOST_RX_CAPACITY            4096u
#define HOST_TX_CAPACITY            4096u

/*
 * Host flash layout:
 *
 *   0x08000000..0x08007FFF  bootloader/reserved   32 KiB
 *   0x08008000..0x080087FF  metadata copy 0        2 KiB
 *   0x08008800..0x08008FFF  metadata copy 1        2 KiB
 *   0x08009000..0x08023FFF  slot A               108 KiB
 *   0x08024000..0x0803EFFF  slot B               108 KiB
 *   0x0803F000..0x0803FFFF  reserved               4 KiB
 *
 * All programmable regions begin on erase-page boundaries.
 */
#define HOST_META_PRIMARY_BASE      0x08008000u
#define HOST_META_PRIMARY_SIZE      0x00000800u

#define HOST_META_SECONDARY_BASE    0x08008800u
#define HOST_META_SECONDARY_SIZE    0x00000800u

#define HOST_SLOT_A_BASE            0x08009000u
#define HOST_SLOT_A_SIZE            0x0001B000u

#define HOST_SLOT_B_BASE            0x08024000u
#define HOST_SLOT_B_SIZE            0x0001B000u

static uint8_t g_flash[HOST_FLASH_SIZE];

/*
 * One bit per simulated flash byte.
 *
 * A set bit means the byte has already been programmed since its most
 * recent erase. Tracking per byte is intentionally stricter than the
 * target's 64-bit programming unit.
 */
static uint8_t g_written[HOST_FLASH_SIZE / 8u];

/* Simulated transport state. */
static uint8_t g_rx[HOST_RX_CAPACITY];
static size_t g_rx_len;
static size_t g_rx_pos;

static uint8_t g_tx[HOST_TX_CAPACITY];
static size_t g_tx_len;

/* Test-controlled monotonic clock. */
static uint32_t g_time_ms;

/* Observable control-flow requests. */
static uint32_t g_jump_target;
static bool g_jumped;
static bool g_reset;

/*
 * Flash-write failure injection.
 *
 * UINT32_MAX disables injection. Otherwise, writes whose zero-based call
 * index is greater than or equal to g_fail_write_from fail.
 */
static uint32_t g_write_call_count;
static uint32_t g_fail_write_from;

static const bl_layout_t g_layout = {
    .slot = {
        [BL_SLOT_A] = {
            .base = HOST_SLOT_A_BASE,
            .size = HOST_SLOT_A_SIZE,
        },

        [BL_SLOT_B] = {
            .base = HOST_SLOT_B_BASE,
            .size = HOST_SLOT_B_SIZE,
        },
    },

    .meta = {
        [BL_META_PRIMARY] = {
            .base = HOST_META_PRIMARY_BASE,
            .size = HOST_META_PRIMARY_SIZE,
        },

        [BL_META_SECONDARY] = {
            .base = HOST_META_SECONDARY_BASE,
            .size = HOST_META_SECONDARY_SIZE,
        },
    },
};

static uint32_t flash_offset(uint32_t addr)
{
    return addr - HOST_FLASH_BASE;
}

/*
 * Validate that [addr, addr + len) lies inside simulated flash.
 * A zero-length operation may use the one-past-the-end address.
 */
static bool flash_range_valid(uint32_t addr, uint32_t len)
{
    const uint32_t flash_end = HOST_FLASH_BASE + HOST_FLASH_SIZE;

    if (len == 0u) {
        return addr >= HOST_FLASH_BASE && addr <= flash_end;
    }

    if (addr < HOST_FLASH_BASE || addr >= flash_end) {
        return false;
    }

    const uint32_t offset = addr - HOST_FLASH_BASE;

    return len <= HOST_FLASH_SIZE - offset;
}

static bool flash_byte_written(uint32_t offset)
{
    const size_t byte_index = offset / 8u;
    const size_t bit_index = offset % 8u;
    const uint8_t mask = (uint8_t)(1u << bit_index);

    return (g_written[byte_index] & mask) != 0u;
}

static void flash_mark_written(uint32_t offset)
{
    const size_t byte_index = offset / 8u;
    const size_t bit_index = offset % 8u;
    const uint8_t mask = (uint8_t)(1u << bit_index);

    g_written[byte_index] |= mask;
}

static void flash_mark_erased(uint32_t offset)
{
    const size_t byte_index = offset / 8u;
    const size_t bit_index = offset % 8u;
    const uint8_t mask = (uint8_t)(1u << bit_index);

    g_written[byte_index] &= (uint8_t)~mask;
}

/*
 * Locate the slot containing addr. If requested, return that slot's size.
 */
static bool find_slot_for_address(uint32_t addr, uint32_t *slot_size_out)
{
    for (uint32_t i = 0u; i < BL_SLOT_COUNT; ++i) {
        const uint32_t base = g_layout.slot[i].base;
        const uint32_t size = g_layout.slot[i].size;

        if (addr >= base && addr < base + size) {
            if (slot_size_out != NULL) {
                *slot_size_out = size;
            }

            return true;
        }
    }

    return false;
}

bl_result_t bl_port_init(void)
{
    /*
     * Reset all simulated state so each test starts from an independent,
     * deterministic environment.
     */
    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(g_written, 0, sizeof(g_written));

    memset(g_rx, 0, sizeof(g_rx));
    g_rx_len = 0u;
    g_rx_pos = 0u;

    memset(g_tx, 0, sizeof(g_tx));
    g_tx_len = 0u;

    g_time_ms = 0u;

    g_jump_target = 0u;
    g_jumped = false;
    g_reset = false;

    g_write_call_count = 0u;
    g_fail_write_from = UINT32_MAX;

    return BL_OK;
}

const bl_layout_t *bl_port_layout(void)
{
    return &g_layout;
}

bl_result_t bl_flash_erase(uint32_t addr, uint32_t len)
{
    /*
     * Invalid ranges or alignment indicate a caller bug rather than a
     * recoverable flash-controller failure.
     */
    BL_HOST_ASSERT(flash_range_valid(addr, len));

    if (len == 0u) {
        return BL_OK;
    }

    BL_HOST_ASSERT(
        ((addr - HOST_FLASH_BASE) % HOST_ERASE_GRANULARITY) == 0u
    );

    BL_HOST_ASSERT(
        (len % HOST_ERASE_GRANULARITY) == 0u
    );

    const uint32_t offset = flash_offset(addr);

    memset(&g_flash[offset], 0xFF, len);

    for (uint32_t i = 0u; i < len; ++i) {
        flash_mark_erased(offset + i);
    }

    return BL_OK;
}

bl_result_t bl_flash_write(uint32_t addr, const void *data, uint32_t len)
{
    if (data == NULL && len > 0u) {
        return BL_ERR_NULL_POINTER;
    }

    BL_HOST_ASSERT(flash_range_valid(addr, len));

    if (len == 0u) {
        return BL_OK;
    }

    BL_HOST_ASSERT(
        ((addr - HOST_FLASH_BASE) % HOST_WRITE_GRANULARITY) == 0u
    );

    BL_HOST_ASSERT(
        (len % HOST_WRITE_GRANULARITY) == 0u
    );

    const uint32_t offset = flash_offset(addr);

    /*
     * Programming a location twice without erasing violates the port
     * contract and can lead to ECC faults on the target.
     *
     * This check precedes failure injection deliberately: a contract
     * violation is a bug in bl_core regardless of whether this particular
     * write was scheduled to fail, and injection must never mask it.
     */
    for (uint32_t i = 0u; i < len; ++i) {
        BL_HOST_ASSERT(!flash_byte_written(offset + i));
    }

    /*
     * Injected failures model runtime flash-program failures that core must
     * handle normally. Contract violations remain fatal host assertions.
     *
     * A failed write leaves flash entirely unmodified. This models the
     * target's 64-bit programming operation, which either completes or does
     * not; it does not model a partially programmed doubleword.
     */
    const uint32_t call_index = g_write_call_count++;

    if (call_index >= g_fail_write_from) {
        return BL_ERR_FLASH_PROGRAM;
    }

    memcpy(&g_flash[offset], data, len);

    for (uint32_t i = 0u; i < len; ++i) {
        flash_mark_written(offset + i);
    }

    return BL_OK;
}

bl_result_t bl_flash_read(uint32_t addr, void *dst, uint32_t len)
{
    if (dst == NULL && len > 0u) {
        return BL_ERR_NULL_POINTER;
    }

    /* Reads intentionally have no alignment requirement. */
    BL_HOST_ASSERT(flash_range_valid(addr, len));

    if (len == 0u) {
        return BL_OK;
    }

    const uint32_t offset = flash_offset(addr);

    memcpy(dst, &g_flash[offset], len);

    return BL_OK;
}

uint32_t bl_flash_write_granularity(void)
{
    return HOST_WRITE_GRANULARITY;
}

uint32_t bl_flash_erase_granularity(uint32_t addr)
{
    BL_HOST_ASSERT(
        addr >= HOST_FLASH_BASE &&
        addr < HOST_FLASH_BASE + HOST_FLASH_SIZE
    );

    return HOST_ERASE_GRANULARITY;
}

bl_result_t bl_transport_read_byte(uint8_t *out, uint32_t timeout_ms)
{
    /*
     * Host RX is deterministic: tests preload all available input.
     * An empty queue therefore represents an immediate timeout.
     */
    (void)timeout_ms;

    if (out == NULL) {
        return BL_ERR_NULL_POINTER;
    }

    if (g_rx_pos >= g_rx_len) {
        return BL_ERR_TIMEOUT;
    }

    *out = g_rx[g_rx_pos++];

    return BL_OK;
}

bl_result_t bl_transport_write(const uint8_t *buf, uint32_t len)
{
    if (buf == NULL && len > 0u) {
        return BL_ERR_NULL_POINTER;
    }

    /* TX overflow indicates an invalid test or unexpected core behavior. */
    BL_HOST_ASSERT(
        (size_t)len <= HOST_TX_CAPACITY - g_tx_len
    );

    if (len == 0u) {
        return BL_OK;
    }

    memcpy(&g_tx[g_tx_len], buf, len);
    g_tx_len += len;

    return BL_OK;
}

uint32_t bl_time_ms(void)
{
    return g_time_ms;
}

void bl_reset(void)
{
    /*
     * Record the request instead of terminating the process so unit-test
     * assertions and Unity/CTest result reporting can complete normally.
     */
    g_reset = true;
}

void bl_jump_to_app(uint32_t base_addr)
{
    uint32_t slot_size = 0u;

    if (!find_slot_for_address(base_addr, &slot_size)) {
        return;
    }

    /* A Cortex-M vector table requires at least MSP and reset-vector words. */
    if (!flash_range_valid(base_addr, 8u)) {
        return;
    }

    const uint32_t offset = flash_offset(base_addr);

    uint32_t initial_msp;
    uint32_t reset_vector;

    memcpy(
        &initial_msp,
        &g_flash[offset],
        sizeof(initial_msp)
    );

    memcpy(
        &reset_vector,
        &g_flash[offset + sizeof(uint32_t)],
        sizeof(reset_vector)
    );

    /*
     * The initial MSP may equal HOST_SRAM_END because Cortex-M stacks grow
     * downward from the top of SRAM.
     */
    if (initial_msp < HOST_SRAM_BASE ||
        initial_msp > HOST_SRAM_END) {
        return;
    }

    if ((initial_msp & 0x3u) != 0u) {
        return;
    }

    /* Cortex-M exception vectors must contain a Thumb-state address. */
    if ((reset_vector & 1u) == 0u) {
        return;
    }

    const uint32_t reset_addr = reset_vector & ~1u;

    if (reset_addr < base_addr ||
        reset_addr >= base_addr + slot_size) {
        return;
    }

    g_jump_target = base_addr;
    g_jumped = true;
}

/* -------------------------------------------------------------------------
 * Test hooks
 * ------------------------------------------------------------------------- */

void bl_host_rx_load(const uint8_t *data, size_t len)
{
    BL_HOST_ASSERT(data != NULL || len == 0u);
    BL_HOST_ASSERT(len <= HOST_RX_CAPACITY);

    if (len > 0u) {
        memcpy(g_rx, data, len);
    }

    g_rx_len = len;
    g_rx_pos = 0u;
}

size_t bl_host_tx_read(uint8_t *dst, size_t max_len)
{
    BL_HOST_ASSERT(dst != NULL || max_len == 0u);

    size_t count = g_tx_len;

    if (count > max_len) {
        count = max_len;
    }

    if (count > 0u) {
        memcpy(dst, g_tx, count);
    }

    return count;
}

size_t bl_host_tx_size(void)
{
    return g_tx_len;
}

void bl_host_tx_clear(void)
{
    memset(g_tx, 0, sizeof(g_tx));
    g_tx_len = 0u;
}

void bl_host_time_set(uint32_t time_ms)
{
    g_time_ms = time_ms;
}

void bl_host_time_advance(uint32_t delta_ms)
{
    g_time_ms += delta_ms;
}

bool bl_host_did_jump(void)
{
    return g_jumped;
}

uint32_t bl_host_jump_target(void)
{
    return g_jump_target;
}

bool bl_host_did_reset(void)
{
    return g_reset;
}

void bl_host_fail_after_n_writes(uint32_t n)
{
    g_write_call_count = 0u;
    g_fail_write_from = n;
}

void bl_host_clear_failures(void)
{
    g_write_call_count = 0u;
    g_fail_write_from = UINT32_MAX;
}
