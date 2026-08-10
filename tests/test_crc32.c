#include "unity.h"
#include "bl_crc32.h"

#include <stdint.h>
#include <stddef.h>


void setUp(void)
{
}


void tearDown(void)
{
}


/*
 * Published check value for CRC-32/ISO-HDLC.
 *
 * This pins the implementation to the same algorithm used by
 * zlib.crc32(), which the Python tooling relies on.
 */
void test_crc_check_vector(void)
{
    uint32_t output = bl_crc32(0, "123456789", 9);

    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, output);
}


/*
 * CRCs computed over consecutive chunks must match the CRC of the
 * concatenated input.
 *
 * The bootloader verifies images arriving in 512-byte pieces and
 * cannot buffer the whole payload, so this property is required.
 */
void test_crc_split_matches_single_pass(void)
{
    uint32_t output = bl_crc32(bl_crc32(0, "12345", 5), "6789", 4);

    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, output);
}


/*
 * An empty input must leave the running value unchanged.
 */
void test_crc_empty_input(void)
{
    TEST_ASSERT_EQUAL_HEX32(0, bl_crc32(0, NULL, 0));
}


/*
 * Inputs differing by a single character must produce different CRCs.
 */
void test_crc_detects_single_byte_difference(void)
{
    uint32_t output1 = bl_crc32(0, "123456789", 9);
    uint32_t output2 = bl_crc32(0, "123456799", 9);

    TEST_ASSERT_NOT_EQUAL_HEX32(output1, output2);
}


int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_crc_check_vector);
    RUN_TEST(test_crc_split_matches_single_pass);
    RUN_TEST(test_crc_empty_input);
    RUN_TEST(test_crc_detects_single_byte_difference);

    return UNITY_END();
}
