#include "unity.h"
#include "bl_api.h"
#include "bl_core.h"
#include "bl_crc32.h"
#include "bl_meta.h"
#include "bl_port.h"
#include "bl_port_host.h"
#include "bl_proto.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * Tests for the table the bootloader publishes to the application.
 *
 * The application calls into bootloader code after control has been
 * transferred, at a point where it has not yet confirmed itself
 * working. A fault there is unrecoverable without a debugger, so the
 * table is validated before any pointer in it is used.
 */

/*
 * A region standing in for the bootloader's flash.
 *
 * On the target the bootloader's code and its published table share one
 * flash region. On the host they are in different segments, so the
 * bounds are derived from the address of the function being validated
 * rather than from the table's own address.
 */
#define TEST_REGION_SIZE 0x8000u


static uintptr_t region_base_for(const bl_api_t *table)
{
    const uintptr_t address = (uintptr_t)table->confirm;
    const uintptr_t half = TEST_REGION_SIZE / 2u;

    return address > half ? address - half : 0u;
}


void setUp(void)
{
    bl_port_init();
}


void tearDown(void)
{
}


/* ------------------------------------------------------------------ */
/* Published table                                                     */
/* ------------------------------------------------------------------ */

/*
 * The table the bootloader publishes must be well formed, since the
 * application has no way to repair it.
 */
void test_published_table_is_well_formed(void)
{
    TEST_ASSERT_EQUAL_HEX32(BL_API_MAGIC, bl_api_table.magic);
    TEST_ASSERT_EQUAL_UINT16(BL_API_VERSION, bl_api_table.version);
    TEST_ASSERT_EQUAL_UINT16(sizeof(bl_api_t), bl_api_table.size);
    TEST_ASSERT_NOT_NULL(bl_api_table.confirm);
}


void test_lookup_accepts_the_published_table(void)
{
    const bl_api_t *api = bl_api_lookup(
        &bl_api_table, region_base_for(&bl_api_table), TEST_REGION_SIZE);

    TEST_ASSERT_EQUAL_PTR(&bl_api_table, api);
}


/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

void test_lookup_rejects_a_null_address(void)
{
    TEST_ASSERT_NULL(bl_api_lookup(NULL, 0x08000000u, TEST_REGION_SIZE));
}


void test_lookup_rejects_an_empty_region(void)
{
    TEST_ASSERT_NULL(
        bl_api_lookup(&bl_api_table, region_base_for(&bl_api_table), 0u));
}


/*
 * Erased flash reads as 0xFF throughout, which must not be mistaken for
 * a table.
 */
void test_lookup_rejects_erased_flash(void)
{
    uint8_t erased[sizeof(bl_api_t)];

    memset(erased, 0xFF, sizeof(erased));

    TEST_ASSERT_NULL(
        bl_api_lookup(erased, region_base_for(&bl_api_table),
                      TEST_REGION_SIZE));
}


void test_lookup_rejects_a_bad_magic(void)
{
    bl_api_t table = bl_api_table;

    table.magic = 0xDEADBEEFu;

    TEST_ASSERT_NULL(
        bl_api_lookup(&table, region_base_for(&bl_api_table),
                      TEST_REGION_SIZE));
}


/*
 * A bootloader publishing a newer table than the application
 * understands must be refused rather than called with a layout the
 * application would misread.
 */
void test_lookup_rejects_a_different_version(void)
{
    bl_api_t table = bl_api_table;

    table.version = (uint16_t)(BL_API_VERSION + 1u);

    TEST_ASSERT_NULL(
        bl_api_lookup(&table, region_base_for(&bl_api_table),
                      TEST_REGION_SIZE));
}


/*
 * A size mismatch means the two binaries were built against different
 * definitions, which is exactly the condition the field exists to
 * catch.
 */
void test_lookup_rejects_a_size_mismatch(void)
{
    bl_api_t table = bl_api_table;

    table.size = (uint16_t)(sizeof(bl_api_t) + 4u);

    TEST_ASSERT_NULL(
        bl_api_lookup(&table, region_base_for(&bl_api_table),
                      TEST_REGION_SIZE));
}


void test_lookup_rejects_a_null_function_pointer(void)
{
    bl_api_t table = bl_api_table;

    table.confirm = NULL;

    TEST_ASSERT_NULL(
        bl_api_lookup(&table, region_base_for(&bl_api_table),
                      TEST_REGION_SIZE));
}


/*
 * A pointer outside the bootloader's region cannot be the bootloader's
 * code. Calling it would fault on a device whose application has not
 * yet confirmed itself, so the table must be refused.
 */
void test_lookup_rejects_a_pointer_outside_the_region(void)
{
    const bl_api_t table = bl_api_table;

    /* A region that deliberately excludes the real function's address. */
    const uintptr_t elsewhere = 0x00001000u;

    TEST_ASSERT_NULL(bl_api_lookup(&table, elsewhere, 0x100u));
}


/* ------------------------------------------------------------------ */
/* Behaviour through the table                                         */
/* ------------------------------------------------------------------ */

/*
 * Confirming through the published pointer must have the same effect as
 * calling the bootloader directly; this is the whole reason the table
 * exists.
 */
void test_confirm_through_the_table_clears_the_trial(void)
{
    TEST_ASSERT_EQUAL(BL_OK,
        bl_meta_commit(BL_SLOT_A, (uint8_t)BL_BOOT_TRIAL, 1u));

    const bl_api_t *api = bl_api_lookup(
        &bl_api_table, region_base_for(&bl_api_table), TEST_REGION_SIZE);

    TEST_ASSERT_NOT_NULL(api);
    TEST_ASSERT_EQUAL(BL_OK, api->confirm());

    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT8(BL_BOOT_CONFIRMED, meta.state);
    TEST_ASSERT_EQUAL_UINT8(0u, meta.boot_attempts);
}


void test_confirm_through_the_table_reports_absent_metadata(void)
{
    const bl_api_t *api = bl_api_lookup(
        &bl_api_table, region_base_for(&bl_api_table), TEST_REGION_SIZE);

    TEST_ASSERT_NOT_NULL(api);
    TEST_ASSERT_EQUAL(BL_ERR_NO_METADATA, api->confirm());
}


int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_published_table_is_well_formed);
    RUN_TEST(test_lookup_accepts_the_published_table);

    RUN_TEST(test_lookup_rejects_a_null_address);
    RUN_TEST(test_lookup_rejects_an_empty_region);
    RUN_TEST(test_lookup_rejects_erased_flash);
    RUN_TEST(test_lookup_rejects_a_bad_magic);
    RUN_TEST(test_lookup_rejects_a_different_version);
    RUN_TEST(test_lookup_rejects_a_size_mismatch);
    RUN_TEST(test_lookup_rejects_a_null_function_pointer);
    RUN_TEST(test_lookup_rejects_a_pointer_outside_the_region);

    RUN_TEST(test_confirm_through_the_table_clears_the_trial);
    RUN_TEST(test_confirm_through_the_table_reports_absent_metadata);

    return UNITY_END();
}
