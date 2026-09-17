#include "unity.h"
#include "bl_crc32.h"
#include "bl_meta.h"
#include "bl_port.h"
#include "bl_port_host.h"
#include "bl_proto.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * Tests for persistent boot metadata.
 *
 * The records are written alternately between two flash regions so that
 * an interrupted erase or program always leaves one intact record. These
 * tests exercise that property directly by injecting flash failures.
 */


void setUp(void)
{
    bl_port_init();
}


void tearDown(void)
{
}


static uint32_t meta_base(uint32_t index)
{
    return bl_port_layout()->meta[index].base;
}


static bl_meta_t read_region_raw(uint32_t index)
{
    bl_meta_t record;

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_read(meta_base(index), &record, sizeof(record)));

    return record;
}


static bool region_holds_valid_record(uint32_t index)
{
    const bl_meta_t record = read_region_raw(index);

    return record.crc32 ==
           bl_crc32(0u, &record, offsetof(bl_meta_t, crc32));
}


/* ------------------------------------------------------------------ */
/* Absence of metadata                                                 */
/* ------------------------------------------------------------------ */

/*
 * A device that has never been updated has no metadata, which must be
 * reported distinctly so the bootloader can fall back to selecting by
 * firmware version.
 */
void test_read_reports_no_metadata_when_erased(void)
{
    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_ERR_NO_METADATA, bl_meta_read(&meta));
}


void test_read_rejects_null_output(void)
{
    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER, bl_meta_read(NULL));
}


/* ------------------------------------------------------------------ */
/* Commit and read back                                                */
/* ------------------------------------------------------------------ */

void test_commit_then_read_returns_the_record(void)
{
    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_TRIAL, 2u));

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_B, meta.active_slot);
    TEST_ASSERT_EQUAL_UINT8(BL_BOOT_TRIAL, meta.state);
    TEST_ASSERT_EQUAL_UINT8(2u, meta.boot_attempts);
    TEST_ASSERT_EQUAL_UINT32(1u, meta.seq);
}


void test_commit_rejects_an_invalid_slot(void)
{
    TEST_ASSERT_EQUAL(BL_ERR_INVALID_ARGUMENT,
        bl_meta_commit(BL_SLOT_COUNT, (uint8_t)BL_BOOT_CONFIRMED, 0u));
}


/*
 * 0xFF is the erased-flash value and must not be accepted as a state,
 * otherwise an erased region could be mistaken for a record.
 */
void test_commit_rejects_an_invalid_state(void)
{
    TEST_ASSERT_EQUAL(BL_ERR_INVALID_ARGUMENT,
        bl_meta_commit(BL_SLOT_A, 0xFFu, 0u));

    TEST_ASSERT_EQUAL(BL_ERR_INVALID_ARGUMENT,
        bl_meta_commit(BL_SLOT_A, 0x00u, 0u));
}


/* ------------------------------------------------------------------ */
/* Ping-pong behaviour                                                 */
/* ------------------------------------------------------------------ */

/*
 * Consecutive commits must alternate regions, so that the previous
 * record is still readable while the new one is being written.
 */
void test_commits_alternate_between_regions(void)
{
    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));

    TEST_ASSERT_TRUE(region_holds_valid_record(0u));
    TEST_ASSERT_FALSE(region_holds_valid_record(1u));

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_CONFIRMED, 0u));

    TEST_ASSERT_TRUE(region_holds_valid_record(0u));
    TEST_ASSERT_TRUE(region_holds_valid_record(1u));

    /* The third commit reuses the first region. */
    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_TRIAL, 1u));

    const bl_meta_t region0 = read_region_raw(0u);

    TEST_ASSERT_EQUAL_UINT32(3u, region0.seq);
}


/*
 * The sequence number must advance on every commit so the authoritative
 * record is unambiguous.
 */
void test_sequence_numbers_increase_monotonically(void)
{
    bl_meta_t meta;

    for (uint32_t i = 1u; i <= 6u; ++i) {
        TEST_ASSERT_EQUAL(BL_OK,
            bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));

        TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
        TEST_ASSERT_EQUAL_UINT32(i, meta.seq);
    }
}


/*
 * When both regions hold valid records, the higher sequence number is
 * authoritative regardless of which region it occupies.
 */
void test_read_returns_the_highest_sequence_number(void)
{
    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));
    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_TRIAL, 1u));

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT32(2u, meta.seq);
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_B, meta.active_slot);
}


/* ------------------------------------------------------------------ */
/* Corruption                                                          */
/* ------------------------------------------------------------------ */

/*
 * A record whose CRC does not match must be ignored, leaving the older
 * record authoritative.
 */
void test_a_corrupt_record_is_ignored(void)
{
    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));
    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_TRIAL, 1u));

    /* Corrupt the newer record in place. */
    bl_meta_t damaged = read_region_raw(1u);
    damaged.active_slot ^= 0xFFu;
    bl_host_flash_poke(meta_base(1u), &damaged, sizeof(damaged));

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT32(1u, meta.seq);
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_A, meta.active_slot);
}


/*
 * A record with a plausible CRC but an out-of-range slot must also be
 * rejected, so a malformed record cannot nominate a slot that does not
 * exist.
 */
void test_a_record_with_an_invalid_slot_is_ignored(void)
{
    bl_meta_t record;
    bl_meta_t meta;

    memset(&record, 0, sizeof(record));

    record.seq = 99u;
    record.active_slot = 0x7Fu;
    record.state = (uint8_t)BL_BOOT_CONFIRMED;
    record.crc32 = bl_crc32(0u, &record, offsetof(bl_meta_t, crc32));

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_write(meta_base(0u), &record, sizeof(record)));

    TEST_ASSERT_EQUAL(BL_ERR_NO_METADATA, bl_meta_read(&meta));
}


void test_a_record_with_an_invalid_state_is_ignored(void)
{
    bl_meta_t record;
    bl_meta_t meta;

    memset(&record, 0, sizeof(record));

    record.seq = 99u;
    record.active_slot = BL_SLOT_A;
    record.state = 0x55u;
    record.crc32 = bl_crc32(0u, &record, offsetof(bl_meta_t, crc32));

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_write(meta_base(0u), &record, sizeof(record)));

    TEST_ASSERT_EQUAL(BL_ERR_NO_METADATA, bl_meta_read(&meta));
}


/* ------------------------------------------------------------------ */
/* Power loss                                                          */
/* ------------------------------------------------------------------ */

/*
 * Losing power while programming a new record must leave the previous
 * record authoritative. This is the property that makes the two-region
 * scheme worthwhile.
 */
void test_power_loss_during_a_commit_preserves_the_previous_record(void)
{
    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));

    /* Fail the write that stores the second record. */
    bl_host_fail_after_n_writes(0u);

    TEST_ASSERT_NOT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_TRIAL, 0u));

    bl_host_clear_failures();

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT32(1u, meta.seq);
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_A, meta.active_slot);
    TEST_ASSERT_EQUAL_UINT8(BL_BOOT_CONFIRMED, meta.state);
}


/*
 * After an interrupted commit, a later commit must still succeed and
 * supersede the surviving record.
 */
void test_a_commit_recovers_after_an_interrupted_one(void)
{
    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));

    bl_host_fail_after_n_writes(0u);
    (void)bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_TRIAL, 0u);
    bl_host_clear_failures();

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_TRIAL, 0u));

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_B, meta.active_slot);
    TEST_ASSERT_EQUAL_UINT8(BL_BOOT_TRIAL, meta.state);
}


/*
 * Every commit point must leave the metadata in a state the bootloader
 * can interpret: either the old record or the new one, never neither.
 */
void test_metadata_survives_failure_at_every_write(void)
{
    for (uint32_t fail_at = 0u; fail_at < 4u; ++fail_at) {
        bl_port_init();

        TEST_ASSERT_EQUAL(BL_OK,
            bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));

        bl_host_fail_after_n_writes(fail_at);
        (void)bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_TRIAL, 1u);
        bl_host_clear_failures();

        bl_meta_t meta;

        TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
        TEST_ASSERT_TRUE(meta.active_slot == BL_SLOT_A ||
                         meta.active_slot == BL_SLOT_B);
    }
}


/* ------------------------------------------------------------------ */
/* Erase                                                               */
/* ------------------------------------------------------------------ */

void test_erase_all_removes_every_record(void)
{
    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));
    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_TRIAL, 0u));

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_erase_all());

    TEST_ASSERT_EQUAL(BL_ERR_NO_METADATA, bl_meta_read(&meta));
}


/*
 * After erasing, sequence numbers restart. The scheme relies only on the
 * relative ordering of records present in flash.
 */
void test_sequence_restarts_after_erase(void)
{
    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));
    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_B, (uint8_t)BL_BOOT_TRIAL, 0u));
    TEST_ASSERT_EQUAL(BL_OK, bl_meta_erase_all());

    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_CONFIRMED, 0u));

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT32(1u, meta.seq);
}


int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_read_reports_no_metadata_when_erased);
    RUN_TEST(test_read_rejects_null_output);

    RUN_TEST(test_commit_then_read_returns_the_record);
    RUN_TEST(test_commit_rejects_an_invalid_slot);
    RUN_TEST(test_commit_rejects_an_invalid_state);

    RUN_TEST(test_commits_alternate_between_regions);
    RUN_TEST(test_sequence_numbers_increase_monotonically);
    RUN_TEST(test_read_returns_the_highest_sequence_number);

    RUN_TEST(test_a_corrupt_record_is_ignored);
    RUN_TEST(test_a_record_with_an_invalid_slot_is_ignored);
    RUN_TEST(test_a_record_with_an_invalid_state_is_ignored);

    RUN_TEST(test_power_loss_during_a_commit_preserves_the_previous_record);
    RUN_TEST(test_a_commit_recovers_after_an_interrupted_one);
    RUN_TEST(test_metadata_survives_failure_at_every_write);

    RUN_TEST(test_erase_all_removes_every_record);
    RUN_TEST(test_sequence_restarts_after_erase);

    return UNITY_END();
}
