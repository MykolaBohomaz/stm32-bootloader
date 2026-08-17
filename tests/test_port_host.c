#include "unity.h"
#include "bl_port.h"
#include "bl_port_host.h"
#include "bl_proto.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * Tests for the simulated host port.
 *
 * Every other test in this project runs on top of this port, so a defect
 * here would silently invalidate them. These tests verify the simulator
 * itself rather than any bootloader logic.
 *
 * Contract violations (misaligned writes, out-of-range addresses, double
 * programming) are enforced with BL_HOST_ASSERT, which aborts the process.
 * They therefore cannot be exercised from an ordinary Unity test and are
 * deliberately not covered here.
 */

#define TEST_ERASE_PAGE 2048u


void setUp(void)
{
    bl_port_init();
}


void tearDown(void)
{
}


/*
 * Serialise a Cortex-M vector table prologue into a little-endian buffer.
 */
static void encode_vector_table(uint8_t out[8],
                                uint32_t initial_msp,
                                uint32_t reset_vector)
{
    out[0] = (uint8_t)(initial_msp & 0xFFu);
    out[1] = (uint8_t)((initial_msp >> 8) & 0xFFu);
    out[2] = (uint8_t)((initial_msp >> 16) & 0xFFu);
    out[3] = (uint8_t)((initial_msp >> 24) & 0xFFu);

    out[4] = (uint8_t)(reset_vector & 0xFFu);
    out[5] = (uint8_t)((reset_vector >> 8) & 0xFFu);
    out[6] = (uint8_t)((reset_vector >> 16) & 0xFFu);
    out[7] = (uint8_t)((reset_vector >> 24) & 0xFFu);
}


/*
 * Write a vector table that bl_jump_to_app() will accept at app_base.
 *
 * The port validates five properties, all of which a linked Cortex-M
 * image satisfies: the base address meets the VTOR alignment
 * requirement, the initial MSP lies in SRAM, the MSP is word aligned,
 * the reset vector has the Thumb bit set, and the reset vector points
 * inside the slot holding the image.
 */
static void write_valid_vector_table(uint32_t app_base)
{
    uint8_t vector_table[8];

    encode_vector_table(vector_table, 0x20010000u, (app_base + 0x100u) | 1u);

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_write(app_base, vector_table, sizeof(vector_table)));
}


/*
 * Return the address an application image occupies within a slot.
 */
static uint32_t app_base_of(uint8_t slot)
{
    return bl_port_layout()->slot[slot].base + BL_IMG_HDR_REGION;
}


/* ------------------------------------------------------------------ */
/* Initialisation                                                      */
/* ------------------------------------------------------------------ */

/*
 * A freshly initialised device must present fully erased flash.
 */
void test_init_leaves_flash_erased(void)
{
    const bl_layout_t *layout = bl_port_layout();
    uint8_t buf[64];

    memset(buf, 0x00, sizeof(buf));

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_read(layout->slot[BL_SLOT_A].base, buf, sizeof(buf)));

    for (size_t i = 0u; i < sizeof(buf); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, buf[i]);
    }
}


/*
 * bl_port_init() must clear every piece of observable state, so that test
 * results cannot depend on execution order.
 */
void test_init_clears_all_observable_state(void)
{
    const uint8_t rx[] = { 0x01, 0x02 };
    const uint8_t tx[] = { 0x03 };

    /* Dirty every piece of state the port exposes. */
    bl_host_rx_load(rx, sizeof(rx));
    TEST_ASSERT_EQUAL(BL_OK, bl_transport_write(tx, sizeof(tx)));
    bl_host_time_set(12345u);
    bl_reset();

    write_valid_vector_table(app_base_of(BL_SLOT_A));
    bl_jump_to_app(app_base_of(BL_SLOT_A));
    TEST_ASSERT_TRUE(bl_host_did_jump());

    bl_port_init();

    TEST_ASSERT_EQUAL_UINT32(0u, bl_time_ms());
    TEST_ASSERT_EQUAL_UINT(0u, bl_host_tx_size());
    TEST_ASSERT_FALSE(bl_host_did_jump());
    TEST_ASSERT_FALSE(bl_host_did_reset());
    TEST_ASSERT_EQUAL_HEX32(0u, bl_host_jump_target());

    /* The RX queue must be empty, not merely rewound. */
    uint8_t byte = 0u;
    TEST_ASSERT_EQUAL(BL_ERR_TIMEOUT, bl_transport_read_byte(&byte, 100u));
}


/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/*
 * Every programmable region must be erase-page aligned, sized in whole
 * pages, and contained within flash. A region that violates this cannot be
 * erased without destroying a neighbour.
 */
void test_layout_regions_are_page_aligned(void)
{
    const bl_layout_t *layout = bl_port_layout();

    for (uint32_t i = 0u; i < BL_SLOT_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT32(0u, layout->slot[i].base % TEST_ERASE_PAGE);
        TEST_ASSERT_EQUAL_UINT32(0u, layout->slot[i].size % TEST_ERASE_PAGE);
        TEST_ASSERT_GREATER_THAN_UINT32(0u, layout->slot[i].size);
    }

    for (uint32_t i = 0u; i < BL_META_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT32(0u, layout->meta[i].base % TEST_ERASE_PAGE);
        TEST_ASSERT_EQUAL_UINT32(0u, layout->meta[i].size % TEST_ERASE_PAGE);
        TEST_ASSERT_GREATER_THAN_UINT32(0u, layout->meta[i].size);
    }
}


/*
 * Slots and metadata copies must not overlap. An overlap would let an
 * update to one region corrupt another.
 */
void test_layout_regions_do_not_overlap(void)
{
    const bl_layout_t *layout = bl_port_layout();

    const uint32_t a_start = layout->slot[BL_SLOT_A].base;
    const uint32_t a_end   = a_start + layout->slot[BL_SLOT_A].size;
    const uint32_t b_start = layout->slot[BL_SLOT_B].base;
    const uint32_t b_end   = b_start + layout->slot[BL_SLOT_B].size;

    TEST_ASSERT_TRUE(a_end <= b_start || b_end <= a_start);

    const uint32_t m0_start = layout->meta[BL_META_PRIMARY].base;
    const uint32_t m0_end   = m0_start + layout->meta[BL_META_PRIMARY].size;
    const uint32_t m1_start = layout->meta[BL_META_SECONDARY].base;
    const uint32_t m1_end   = m1_start + layout->meta[BL_META_SECONDARY].size;

    TEST_ASSERT_TRUE(m0_end <= m1_start || m1_end <= m0_start);

    /* Metadata must not land inside either slot. */
    TEST_ASSERT_TRUE(m0_end <= a_start || m0_start >= a_end);
    TEST_ASSERT_TRUE(m0_end <= b_start || m0_start >= b_end);
    TEST_ASSERT_TRUE(m1_end <= a_start || m1_start >= a_end);
    TEST_ASSERT_TRUE(m1_end <= b_start || m1_start >= b_end);
}


/*
 * The two slots must be interchangeable, otherwise an image that fits one
 * could not be rolled back into the other.
 */
void test_layout_slots_are_equal_size(void)
{
    const bl_layout_t *layout = bl_port_layout();

    TEST_ASSERT_EQUAL_UINT32(layout->slot[BL_SLOT_A].size,
                             layout->slot[BL_SLOT_B].size);
}


/* ------------------------------------------------------------------ */
/* Flash                                                               */
/* ------------------------------------------------------------------ */

/*
 * Erasing must restore a region to 0xFF.
 */
void test_erase_restores_erased_state(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_A].base;
    const uint8_t data[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t buf[8];

    TEST_ASSERT_EQUAL(BL_OK, bl_flash_write(base, data, sizeof(data)));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_erase(base, TEST_ERASE_PAGE));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_read(base, buf, sizeof(buf)));

    for (size_t i = 0u; i < sizeof(buf); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, buf[i]);
    }
}


/*
 * Erasing must also clear the programmed-byte tracking, so the region can
 * be written again.
 */
void test_erase_permits_rewriting(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_A].base;
    const uint8_t first[8]  = { 1, 1, 1, 1, 1, 1, 1, 1 };
    const uint8_t second[8] = { 2, 2, 2, 2, 2, 2, 2, 2 };
    uint8_t buf[8];

    TEST_ASSERT_EQUAL(BL_OK, bl_flash_write(base, first, sizeof(first)));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_erase(base, TEST_ERASE_PAGE));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_write(base, second, sizeof(second)));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_read(base, buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(second, buf, sizeof(second));
}


/*
 * Erasing one page must not disturb the next.
 */
void test_erase_is_confined_to_the_requested_pages(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_A].base;
    const uint8_t data[8] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    uint8_t buf[8];

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_write(base + TEST_ERASE_PAGE, data, sizeof(data)));

    TEST_ASSERT_EQUAL(BL_OK, bl_flash_erase(base, TEST_ERASE_PAGE));

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_read(base + TEST_ERASE_PAGE, buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, buf, sizeof(data));
}


/*
 * A write into erased flash must be readable verbatim.
 */
void test_write_then_read_round_trip(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_B].base;
    uint8_t data[64];
    uint8_t buf[64];

    for (size_t i = 0u; i < sizeof(data); ++i) {
        data[i] = (uint8_t)(i * 3u + 1u);
    }

    TEST_ASSERT_EQUAL(BL_OK, bl_flash_write(base, data, sizeof(data)));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_read(base, buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, buf, sizeof(data));
}


/*
 * Reads have no alignment constraint, unlike writes.
 */
void test_read_is_unaligned_capable(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_A].base;
    const uint8_t data[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t buf[3];

    TEST_ASSERT_EQUAL(BL_OK, bl_flash_write(base, data, sizeof(data)));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_read(base + 3u, buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_HEX8(3, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(4, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(5, buf[2]);
}


/*
 * Zero-length operations are permitted and must not modify flash.
 */
void test_zero_length_operations_are_accepted(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_A].base;

    TEST_ASSERT_EQUAL(BL_OK, bl_flash_erase(base, 0u));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_write(base, NULL, 0u));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_read(base, NULL, 0u));
}


/*
 * A NULL buffer with a non-zero length is an argument error, not a flash
 * failure, and must be reported rather than asserted.
 */
void test_null_buffers_are_rejected(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_A].base;

    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER, bl_flash_write(base, NULL, 8u));
    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER, bl_flash_read(base, NULL, 8u));
}


/*
 * The simulated geometry must match the STM32L432 it models.
 */
void test_granularities_match_the_modelled_target(void)
{
    const bl_layout_t *layout = bl_port_layout();

    TEST_ASSERT_EQUAL_UINT32(8u, bl_flash_write_granularity());
    TEST_ASSERT_EQUAL_UINT32(TEST_ERASE_PAGE,
        bl_flash_erase_granularity(layout->slot[BL_SLOT_A].base));
    TEST_ASSERT_EQUAL_UINT32(TEST_ERASE_PAGE,
        bl_flash_erase_granularity(layout->slot[BL_SLOT_B].base));
}


/* ------------------------------------------------------------------ */
/* Failure injection                                                   */
/* ------------------------------------------------------------------ */

/*
 * Failure injection must allow exactly N writes before failing, and must
 * keep failing afterwards. This is the mechanism used to simulate power
 * loss part-way through an update.
 */
void test_failure_injection_fails_after_n_writes(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_A].base;
    const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    bl_host_fail_after_n_writes(2u);

    TEST_ASSERT_EQUAL(BL_OK, bl_flash_write(base, data, sizeof(data)));
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_write(base + 8u, data, sizeof(data)));
    TEST_ASSERT_EQUAL(BL_ERR_FLASH_PROGRAM,
        bl_flash_write(base + 16u, data, sizeof(data)));
    TEST_ASSERT_EQUAL(BL_ERR_FLASH_PROGRAM,
        bl_flash_write(base + 24u, data, sizeof(data)));
}


/*
 * A failed write must leave flash untouched, modelling the target's
 * all-or-nothing programming operation.
 */
void test_failed_write_does_not_modify_flash(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_A].base;
    const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t buf[8];

    bl_host_fail_after_n_writes(0u);

    TEST_ASSERT_EQUAL(BL_ERR_FLASH_PROGRAM,
        bl_flash_write(base, data, sizeof(data)));

    TEST_ASSERT_EQUAL(BL_OK, bl_flash_read(base, buf, sizeof(buf)));

    for (size_t i = 0u; i < sizeof(buf); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, buf[i]);
    }
}


/*
 * Clearing injection must restore normal behaviour, including for a
 * location whose write previously failed.
 */
void test_failure_injection_can_be_cleared(void)
{
    const bl_layout_t *layout = bl_port_layout();
    const uint32_t base = layout->slot[BL_SLOT_A].base;
    const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    bl_host_fail_after_n_writes(0u);
    TEST_ASSERT_EQUAL(BL_ERR_FLASH_PROGRAM,
        bl_flash_write(base, data, sizeof(data)));

    bl_host_clear_failures();
    TEST_ASSERT_EQUAL(BL_OK, bl_flash_write(base, data, sizeof(data)));
}


/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

/*
 * Loaded bytes must be returned in order, then the queue must report a
 * timeout rather than an error or stale data.
 */
void test_rx_returns_loaded_bytes_then_times_out(void)
{
    const uint8_t data[] = { 0xAA, 0xBB, 0xCC };
    uint8_t byte = 0u;

    bl_host_rx_load(data, sizeof(data));

    for (size_t i = 0u; i < sizeof(data); ++i) {
        TEST_ASSERT_EQUAL(BL_OK, bl_transport_read_byte(&byte, 100u));
        TEST_ASSERT_EQUAL_HEX8(data[i], byte);
    }

    TEST_ASSERT_EQUAL(BL_ERR_TIMEOUT, bl_transport_read_byte(&byte, 100u));
}


/*
 * Loading replaces the queue rather than appending to it.
 */
void test_rx_load_replaces_previous_contents(void)
{
    const uint8_t first[]  = { 0x11, 0x22 };
    const uint8_t second[] = { 0x33 };
    uint8_t byte = 0u;

    bl_host_rx_load(first, sizeof(first));
    TEST_ASSERT_EQUAL(BL_OK, bl_transport_read_byte(&byte, 100u));

    bl_host_rx_load(second, sizeof(second));

    TEST_ASSERT_EQUAL(BL_OK, bl_transport_read_byte(&byte, 100u));
    TEST_ASSERT_EQUAL_HEX8(0x33, byte);
    TEST_ASSERT_EQUAL(BL_ERR_TIMEOUT, bl_transport_read_byte(&byte, 100u));
}


/*
 * An empty queue must time out immediately regardless of the timeout
 * value, so timeout handling can be tested without real delays.
 */
void test_rx_empty_queue_times_out(void)
{
    uint8_t byte = 0u;

    TEST_ASSERT_EQUAL(BL_ERR_TIMEOUT, bl_transport_read_byte(&byte, 0u));
    TEST_ASSERT_EQUAL(BL_ERR_TIMEOUT, bl_transport_read_byte(&byte, 100000u));
}


/*
 * Transmitted bytes must accumulate in order and be observable by tests.
 */
void test_tx_captures_written_bytes(void)
{
    const uint8_t first[]  = { 0x01, 0x02 };
    const uint8_t second[] = { 0x03 };
    uint8_t buf[8];

    TEST_ASSERT_EQUAL(BL_OK, bl_transport_write(first, sizeof(first)));
    TEST_ASSERT_EQUAL(BL_OK, bl_transport_write(second, sizeof(second)));

    TEST_ASSERT_EQUAL_UINT(3u, bl_host_tx_size());
    TEST_ASSERT_EQUAL_UINT(3u, bl_host_tx_read(buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_HEX8(0x01, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03, buf[2]);
}


/*
 * Reading the TX buffer must not consume it; clearing must.
 */
void test_tx_read_does_not_consume_and_clear_does(void)
{
    const uint8_t data[] = { 0x42 };
    uint8_t buf[4];

    TEST_ASSERT_EQUAL(BL_OK, bl_transport_write(data, sizeof(data)));

    TEST_ASSERT_EQUAL_UINT(1u, bl_host_tx_read(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT(1u, bl_host_tx_read(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT(1u, bl_host_tx_size());

    bl_host_tx_clear();

    TEST_ASSERT_EQUAL_UINT(0u, bl_host_tx_size());
    TEST_ASSERT_EQUAL_UINT(0u, bl_host_tx_read(buf, sizeof(buf)));
}


/*
 * bl_host_tx_read() must never write past the caller's buffer.
 */
void test_tx_read_respects_destination_size(void)
{
    const uint8_t data[] = { 1, 2, 3, 4, 5 };
    uint8_t buf[2];

    TEST_ASSERT_EQUAL(BL_OK, bl_transport_write(data, sizeof(data)));

    TEST_ASSERT_EQUAL_UINT(2u, bl_host_tx_read(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8(1, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(2, buf[1]);
}


/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

/*
 * The clock must be fully test-controlled so timeout behaviour is
 * deterministic rather than dependent on machine speed.
 */
void test_time_is_test_controlled(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, bl_time_ms());

    bl_host_time_set(1000u);
    TEST_ASSERT_EQUAL_UINT32(1000u, bl_time_ms());

    bl_host_time_advance(500u);
    TEST_ASSERT_EQUAL_UINT32(1500u, bl_time_ms());

    /* Repeated reads must not advance the clock. */
    TEST_ASSERT_EQUAL_UINT32(1500u, bl_time_ms());
}


/*
 * The clock wraps at 2^32, matching the bl_time_ms() contract. Callers are
 * required to use unsigned subtraction, which stays correct across the wrap.
 */
void test_time_wraps_and_subtraction_stays_correct(void)
{
    const uint32_t start = UINT32_MAX - 100u;

    bl_host_time_set(start);
    bl_host_time_advance(250u);

    TEST_ASSERT_EQUAL_UINT32(149u, bl_time_ms());
    TEST_ASSERT_EQUAL_UINT32(250u, bl_time_ms() - start);
}


/* ------------------------------------------------------------------ */
/* Control flow                                                        */
/* ------------------------------------------------------------------ */

/*
 * An erased slot has an initial stack pointer of 0xFFFFFFFF, which is not a
 * valid SRAM address, so the jump must be refused.
 */
void test_jump_is_refused_for_an_erased_slot(void)
{
    bl_jump_to_app(app_base_of(BL_SLOT_A));

    TEST_ASSERT_FALSE(bl_host_did_jump());
}


/*
 * The application vector table must satisfy the target's VTOR alignment
 * requirement. An unaligned base is silently truncated by the hardware,
 * so it must be rejected before the jump rather than producing incorrect
 * exception vectors at run time.
 */
void test_jump_is_refused_for_an_unaligned_base(void)
{
    const uint32_t aligned = app_base_of(BL_SLOT_A);
    const uint32_t unaligned = aligned + 8u;

    uint8_t vector_table[8];

    encode_vector_table(vector_table, 0x20010000u, (unaligned + 0x100u) | 1u);

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_write(unaligned, vector_table, sizeof(vector_table)));

    bl_jump_to_app(unaligned);

    TEST_ASSERT_FALSE(bl_host_did_jump());
}


/*
 * Both slot bases must be aligned such that an image placed after the
 * header region satisfies the VTOR alignment requirement. A layout
 * change that breaks this would otherwise only fail on hardware.
 */
void test_layout_supports_aligned_application_bases(void)
{
    const bl_layout_t *layout = bl_port_layout();

    for (uint32_t i = 0u; i < BL_SLOT_COUNT; ++i) {
        const uint32_t app_base = layout->slot[i].base + BL_IMG_HDR_REGION;

        TEST_ASSERT_EQUAL_UINT32(0u, app_base % BL_IMG_HDR_REGION);
        TEST_ASSERT_GREATER_THAN_UINT32(BL_IMG_HDR_REGION, layout->slot[i].size);
    }
}


/*
 * A plausible initial stack pointer must be accepted and the target
 * recorded, so boot-selection logic can be verified without a real jump.
 */
void test_jump_is_accepted_for_a_plausible_vector_table(void)
{
    const uint32_t app_base = app_base_of(BL_SLOT_B);

    write_valid_vector_table(app_base);

    bl_jump_to_app(app_base);

    TEST_ASSERT_TRUE(bl_host_did_jump());
    TEST_ASSERT_EQUAL_HEX32(app_base, bl_host_jump_target());
}


/*
 * A reset vector without the Thumb bit would fault immediately on a
 * Cortex-M, so the port must refuse it.
 */
void test_jump_is_refused_without_thumb_bit(void)
{
    const uint32_t app_base = app_base_of(BL_SLOT_A);

    uint8_t vector_table[8];

    /* Thumb bit cleared. */
    encode_vector_table(vector_table, 0x20010000u, app_base + 0x100u);

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_write(app_base, vector_table, sizeof(vector_table)));

    bl_jump_to_app(app_base);

    TEST_ASSERT_FALSE(bl_host_did_jump());
}


/*
 * A reset vector pointing outside its own slot indicates an image linked
 * for the other slot, which must not be booted.
 */
void test_jump_is_refused_when_reset_vector_leaves_the_slot(void)
{
    const uint32_t app_base = app_base_of(BL_SLOT_B);

    /* A valid-looking entry point, but located in slot A. */
    const uint32_t reset_vector = (app_base_of(BL_SLOT_A) + 0x100u) | 1u;

    uint8_t vector_table[8];

    encode_vector_table(vector_table, 0x20010000u, reset_vector);

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_write(app_base, vector_table, sizeof(vector_table)));

    bl_jump_to_app(app_base);

    TEST_ASSERT_FALSE(bl_host_did_jump());
}


/*
 * Addresses outside any slot must be refused even if flash there happens to
 * contain a plausible vector table.
 */
void test_jump_is_refused_outside_the_slots(void)
{
    const bl_layout_t *layout = bl_port_layout();

    bl_jump_to_app(layout->meta[BL_META_PRIMARY].base);

    TEST_ASSERT_FALSE(bl_host_did_jump());
}


/*
 * bl_reset() must record the request instead of terminating the process,
 * which would cause CTest to report success without running assertions.
 */
void test_reset_is_recorded_and_returns(void)
{
    TEST_ASSERT_FALSE(bl_host_did_reset());

    bl_reset();

    TEST_ASSERT_TRUE(bl_host_did_reset());
}


int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_init_leaves_flash_erased);
    RUN_TEST(test_init_clears_all_observable_state);

    RUN_TEST(test_layout_regions_are_page_aligned);
    RUN_TEST(test_layout_regions_do_not_overlap);
    RUN_TEST(test_layout_slots_are_equal_size);
    RUN_TEST(test_layout_supports_aligned_application_bases);

    RUN_TEST(test_erase_restores_erased_state);
    RUN_TEST(test_erase_permits_rewriting);
    RUN_TEST(test_erase_is_confined_to_the_requested_pages);
    RUN_TEST(test_write_then_read_round_trip);
    RUN_TEST(test_read_is_unaligned_capable);
    RUN_TEST(test_zero_length_operations_are_accepted);
    RUN_TEST(test_null_buffers_are_rejected);
    RUN_TEST(test_granularities_match_the_modelled_target);

    RUN_TEST(test_failure_injection_fails_after_n_writes);
    RUN_TEST(test_failed_write_does_not_modify_flash);
    RUN_TEST(test_failure_injection_can_be_cleared);

    RUN_TEST(test_rx_returns_loaded_bytes_then_times_out);
    RUN_TEST(test_rx_load_replaces_previous_contents);
    RUN_TEST(test_rx_empty_queue_times_out);
    RUN_TEST(test_tx_captures_written_bytes);
    RUN_TEST(test_tx_read_does_not_consume_and_clear_does);
    RUN_TEST(test_tx_read_respects_destination_size);

    RUN_TEST(test_time_is_test_controlled);
    RUN_TEST(test_time_wraps_and_subtraction_stays_correct);

    RUN_TEST(test_jump_is_refused_for_an_erased_slot);
    RUN_TEST(test_jump_is_refused_for_an_unaligned_base);
    RUN_TEST(test_jump_is_accepted_for_a_plausible_vector_table);
    RUN_TEST(test_jump_is_refused_without_thumb_bit);
    RUN_TEST(test_jump_is_refused_when_reset_vector_leaves_the_slot);
    RUN_TEST(test_jump_is_refused_outside_the_slots);
    RUN_TEST(test_reset_is_recorded_and_returns);

    return UNITY_END();
}
