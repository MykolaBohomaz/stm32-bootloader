#include "unity.h"
#include "bl_core.h"
#include "bl_crc32.h"
#include "bl_port.h"
#include "bl_port_host.h"
#include "bl_proto.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * Tests for boot-time image validation and slot selection.
 *
 * Images are written through the simulated port using the same ordering
 * the update path will use: payload first, header last.
 */

#define TEST_PAYLOAD_SIZE 512u

/* Initial stack pointer used by generated images; within simulated SRAM. */
#define TEST_INITIAL_MSP 0x20010000u


void setUp(void)
{
    bl_port_init();
}


void tearDown(void)
{
}


static uint32_t slot_base_of(uint8_t slot)
{
    return bl_port_layout()->slot[slot].base;
}


static uint32_t slot_size_of(uint8_t slot)
{
    return bl_port_layout()->slot[slot].size;
}


/*
 * Fill a buffer with a Cortex-M vector table prologue followed by
 * deterministic filler, so the generated image is one the port will
 * accept as an entry point.
 */
static void build_payload(uint8_t *payload, uint32_t len, uint32_t app_base)
{
    const uint32_t reset_vector = (app_base + 0x100u) | 1u;

    for (uint32_t i = 0u; i < len; ++i) {
        payload[i] = (uint8_t)(i * 7u + 3u);
    }

    payload[0] = (uint8_t)(TEST_INITIAL_MSP & 0xFFu);
    payload[1] = (uint8_t)((TEST_INITIAL_MSP >> 8) & 0xFFu);
    payload[2] = (uint8_t)((TEST_INITIAL_MSP >> 16) & 0xFFu);
    payload[3] = (uint8_t)((TEST_INITIAL_MSP >> 24) & 0xFFu);

    payload[4] = (uint8_t)(reset_vector & 0xFFu);
    payload[5] = (uint8_t)((reset_vector >> 8) & 0xFFu);
    payload[6] = (uint8_t)((reset_vector >> 16) & 0xFFu);
    payload[7] = (uint8_t)((reset_vector >> 24) & 0xFFu);
}


/*
 * Populate a header describing the supplied payload, including a correct
 * header CRC.
 */
static bl_img_hdr_t build_header(const uint8_t *payload,
                                 uint32_t len,
                                 uint32_t fw_version,
                                 uint32_t entry_offset)
{
    bl_img_hdr_t hdr;

    memset(&hdr, 0, sizeof(hdr));

    hdr.magic = BL_IMG_MAGIC;
    hdr.hdr_version = (uint16_t)BL_IMG_HDR_VERSION;
    hdr.flags = 0u;
    hdr.img_size = len;
    hdr.img_crc32 = bl_crc32(0u, payload, len);
    hdr.fw_version = fw_version;
    hdr.entry_offset = entry_offset;
    hdr.hdr_crc32 = bl_crc32(0u, &hdr, offsetof(bl_img_hdr_t, hdr_crc32));

    return hdr;
}


static void write_payload(uint8_t slot,
                          uint32_t entry_offset,
                          const uint8_t *payload,
                          uint32_t len)
{
    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_write(slot_base_of(slot) + entry_offset, payload, len));
}


static void write_header(uint8_t slot, const bl_img_hdr_t *hdr)
{
    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_write(slot_base_of(slot), hdr, sizeof(*hdr)));
}


/*
 * Write a complete, valid image into a slot.
 */
static void write_valid_image(uint8_t slot, uint32_t fw_version)
{
    uint8_t payload[TEST_PAYLOAD_SIZE];

    const uint32_t app_base = slot_base_of(slot) + BL_IMG_HDR_REGION;

    build_payload(payload, sizeof(payload), app_base);

    const bl_img_hdr_t hdr = build_header(
        payload, sizeof(payload), fw_version, BL_IMG_HDR_REGION);

    write_payload(slot, BL_IMG_HDR_REGION, payload, sizeof(payload));
    write_header(slot, &hdr);
}


/*
 * Write an image whose header has been altered after the header CRC was
 * computed, unless recompute_crc is set.
 */
static void write_image_with_header_mutation(uint8_t slot,
                                             void (*mutate)(bl_img_hdr_t *),
                                             bool recompute_crc)
{
    uint8_t payload[TEST_PAYLOAD_SIZE];

    const uint32_t app_base = slot_base_of(slot) + BL_IMG_HDR_REGION;

    build_payload(payload, sizeof(payload), app_base);

    bl_img_hdr_t hdr = build_header(
        payload, sizeof(payload), 0x00010000u, BL_IMG_HDR_REGION);

    mutate(&hdr);

    if (recompute_crc) {
        hdr.hdr_crc32 = bl_crc32(0u, &hdr, offsetof(bl_img_hdr_t, hdr_crc32));
    }

    write_payload(slot, BL_IMG_HDR_REGION, payload, sizeof(payload));
    write_header(slot, &hdr);
}


/* ------------------------------------------------------------------ */
/* Argument validation                                                 */
/* ------------------------------------------------------------------ */

void test_validate_rejects_null_header(void)
{
    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER,
        bl_core_validate_slot(BL_SLOT_A, NULL));
}


void test_validate_rejects_unknown_slot(void)
{
    bl_img_hdr_t hdr;

    TEST_ASSERT_EQUAL(BL_ERR_INVALID_ARGUMENT,
        bl_core_validate_slot(BL_SLOT_COUNT, &hdr));
}


/* ------------------------------------------------------------------ */
/* Header validation                                                   */
/* ------------------------------------------------------------------ */

/*
 * A complete image must validate and report its header contents.
 */
void test_validate_accepts_a_valid_image(void)
{
    bl_img_hdr_t hdr;

    write_valid_image(BL_SLOT_A, 0x00010203u);

    TEST_ASSERT_EQUAL(BL_OK, bl_core_validate_slot(BL_SLOT_A, &hdr));
    TEST_ASSERT_EQUAL_HEX32(BL_IMG_MAGIC, hdr.magic);
    TEST_ASSERT_EQUAL_UINT32(TEST_PAYLOAD_SIZE, hdr.img_size);
    TEST_ASSERT_EQUAL_HEX32(0x00010203u, hdr.fw_version);
    TEST_ASSERT_EQUAL_UINT32(BL_IMG_HDR_REGION, hdr.entry_offset);
}


/*
 * An erased slot reads as 0xFF and must be reported as having no image
 * rather than producing a header-format error.
 */
void test_validate_rejects_an_erased_slot(void)
{
    bl_img_hdr_t hdr;

    TEST_ASSERT_EQUAL(BL_ERR_MAGIC, bl_core_validate_slot(BL_SLOT_A, &hdr));
}


static void mutate_magic(bl_img_hdr_t *hdr)
{
    hdr->magic = 0xDEADBEEFu;
}

void test_validate_rejects_a_bad_magic(void)
{
    bl_img_hdr_t hdr;

    write_image_with_header_mutation(BL_SLOT_A, mutate_magic, true);

    TEST_ASSERT_EQUAL(BL_ERR_MAGIC, bl_core_validate_slot(BL_SLOT_A, &hdr));
}


static void mutate_hdr_version(bl_img_hdr_t *hdr)
{
    hdr->hdr_version = (uint16_t)(BL_IMG_HDR_VERSION + 1u);
}

void test_validate_rejects_an_unsupported_header_version(void)
{
    bl_img_hdr_t hdr;

    write_image_with_header_mutation(BL_SLOT_A, mutate_hdr_version, true);

    TEST_ASSERT_EQUAL(BL_ERR_HDR_VERSION,
        bl_core_validate_slot(BL_SLOT_A, &hdr));
}


static void mutate_without_fixing_crc(bl_img_hdr_t *hdr)
{
    hdr->flags ^= 0xFFFFu;
}

/*
 * A header field altered after the header CRC was computed must be
 * rejected before any of its contents are used.
 */
void test_validate_rejects_a_corrupt_header(void)
{
    bl_img_hdr_t hdr;

    write_image_with_header_mutation(BL_SLOT_A, mutate_without_fixing_crc,
                                     false);

    TEST_ASSERT_EQUAL(BL_ERR_HDR_CRC, bl_core_validate_slot(BL_SLOT_A, &hdr));
}


/* ------------------------------------------------------------------ */
/* Entry offset validation                                             */
/* ------------------------------------------------------------------ */

static void mutate_entry_offset_too_small(bl_img_hdr_t *hdr)
{
    hdr->entry_offset = 8u;
}

void test_validate_rejects_entry_offset_inside_the_header(void)
{
    bl_img_hdr_t hdr;

    write_image_with_header_mutation(BL_SLOT_A,
                                     mutate_entry_offset_too_small, true);

    TEST_ASSERT_EQUAL(BL_ERR_ENTRY_OFFSET,
        bl_core_validate_slot(BL_SLOT_A, &hdr));
}


static void mutate_entry_offset_misaligned(bl_img_hdr_t *hdr)
{
    hdr->entry_offset = BL_IMG_HDR_REGION + 8u;
}

/*
 * An entry offset that does not preserve vector table alignment must be
 * rejected; the hardware would silently discard the low address bits.
 */
void test_validate_rejects_a_misaligned_entry_offset(void)
{
    bl_img_hdr_t hdr;

    write_image_with_header_mutation(BL_SLOT_A,
                                     mutate_entry_offset_misaligned, true);

    TEST_ASSERT_EQUAL(BL_ERR_ENTRY_OFFSET,
        bl_core_validate_slot(BL_SLOT_A, &hdr));
}


static void mutate_entry_offset_past_slot(bl_img_hdr_t *hdr)
{
    hdr->entry_offset = 0x40000u;
}

void test_validate_rejects_entry_offset_beyond_the_slot(void)
{
    bl_img_hdr_t hdr;

    write_image_with_header_mutation(BL_SLOT_A,
                                     mutate_entry_offset_past_slot, true);

    TEST_ASSERT_EQUAL(BL_ERR_ENTRY_OFFSET,
        bl_core_validate_slot(BL_SLOT_A, &hdr));
}


/* ------------------------------------------------------------------ */
/* Image size validation                                               */
/* ------------------------------------------------------------------ */

static void mutate_img_size_zero(bl_img_hdr_t *hdr)
{
    hdr->img_size = 0u;
}

void test_validate_rejects_a_zero_length_image(void)
{
    bl_img_hdr_t hdr;

    write_image_with_header_mutation(BL_SLOT_A, mutate_img_size_zero, true);

    TEST_ASSERT_EQUAL(BL_ERR_IMAGE_SIZE,
        bl_core_validate_slot(BL_SLOT_A, &hdr));
}


static void mutate_img_size_huge(bl_img_hdr_t *hdr)
{
    hdr->img_size = 0xFFFFFFF8u;
}

/*
 * An oversized length must be rejected using the slot bounds rather than
 * being used to read past the end of the slot.
 */
void test_validate_rejects_an_image_larger_than_the_slot(void)
{
    bl_img_hdr_t hdr;

    write_image_with_header_mutation(BL_SLOT_A, mutate_img_size_huge, true);

    TEST_ASSERT_EQUAL(BL_ERR_IMAGE_SIZE,
        bl_core_validate_slot(BL_SLOT_A, &hdr));
}


/*
 * An image that exactly fills the slot must be accepted; the bound is
 * inclusive.
 */
void test_validate_accepts_an_image_that_fills_the_slot(void)
{
    bl_img_hdr_t hdr;

    const uint32_t max_size = slot_size_of(BL_SLOT_A) - BL_IMG_HDR_REGION;

    /*
     * The payload is written in chunks to avoid a large stack buffer; the
     * CRC is accumulated over the same data.
     */
    uint8_t chunk[256];
    uint32_t crc = 0u;
    uint32_t written = 0u;

    memset(chunk, 0xA5, sizeof(chunk));

    while (written < max_size) {
        uint32_t len = max_size - written;

        if (len > sizeof(chunk)) {
            len = sizeof(chunk);
        }

        TEST_ASSERT_EQUAL(BL_OK,
            bl_flash_write(slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION +
                               written,
                           chunk,
                           len));

        crc = bl_crc32(crc, chunk, len);
        written += len;
    }

    bl_img_hdr_t image_hdr;
    memset(&image_hdr, 0, sizeof(image_hdr));

    image_hdr.magic = BL_IMG_MAGIC;
    image_hdr.hdr_version = (uint16_t)BL_IMG_HDR_VERSION;
    image_hdr.img_size = max_size;
    image_hdr.img_crc32 = crc;
    image_hdr.fw_version = 0x00010000u;
    image_hdr.entry_offset = BL_IMG_HDR_REGION;
    image_hdr.hdr_crc32 =
        bl_crc32(0u, &image_hdr, offsetof(bl_img_hdr_t, hdr_crc32));

    write_header(BL_SLOT_A, &image_hdr);

    TEST_ASSERT_EQUAL(BL_OK, bl_core_validate_slot(BL_SLOT_A, &hdr));
    TEST_ASSERT_EQUAL_UINT32(max_size, hdr.img_size);
}


/* ------------------------------------------------------------------ */
/* Payload validation                                                  */
/* ------------------------------------------------------------------ */

/*
 * A single altered payload byte must be detected by the image CRC.
 */
void test_validate_rejects_a_corrupt_payload(void)
{
    uint8_t payload[TEST_PAYLOAD_SIZE];
    bl_img_hdr_t hdr;

    const uint32_t app_base = slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION;

    build_payload(payload, sizeof(payload), app_base);

    /* Describe the intended payload, then store a different one. */
    const bl_img_hdr_t image_hdr = build_header(
        payload, sizeof(payload), 0x00010000u, BL_IMG_HDR_REGION);

    payload[100] ^= 0xFFu;

    write_payload(BL_SLOT_A, BL_IMG_HDR_REGION, payload, sizeof(payload));
    write_header(BL_SLOT_A, &image_hdr);

    TEST_ASSERT_EQUAL(BL_ERR_IMAGE_CRC,
        bl_core_validate_slot(BL_SLOT_A, &hdr));
}


/*
 * A header that survived an interrupted payload write must not validate:
 * the unwritten remainder reads as erased flash and fails the image CRC.
 */
void test_validate_rejects_a_truncated_payload(void)
{
    uint8_t payload[TEST_PAYLOAD_SIZE];
    bl_img_hdr_t hdr;

    const uint32_t app_base = slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION;

    build_payload(payload, sizeof(payload), app_base);

    const bl_img_hdr_t image_hdr = build_header(
        payload, sizeof(payload), 0x00010000u, BL_IMG_HDR_REGION);

    /* Only half of the described payload reaches flash. */
    write_payload(BL_SLOT_A, BL_IMG_HDR_REGION, payload,
                  TEST_PAYLOAD_SIZE / 2u);
    write_header(BL_SLOT_A, &image_hdr);

    TEST_ASSERT_EQUAL(BL_ERR_IMAGE_CRC,
        bl_core_validate_slot(BL_SLOT_A, &hdr));
}


/*
 * Output must not be written when validation fails, so a caller cannot
 * act on a partially populated header.
 */
void test_validate_leaves_output_untouched_on_failure(void)
{
    bl_img_hdr_t hdr;

    memset(&hdr, 0x5A, sizeof(hdr));

    TEST_ASSERT_EQUAL(BL_ERR_MAGIC, bl_core_validate_slot(BL_SLOT_A, &hdr));

    for (size_t i = 0u; i < sizeof(hdr); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x5A, ((const uint8_t *)&hdr)[i]);
    }
}


/* ------------------------------------------------------------------ */
/* Slot selection                                                      */
/* ------------------------------------------------------------------ */

void test_select_rejects_null_arguments(void)
{
    uint8_t slot = 0u;
    bl_img_hdr_t hdr;

    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER, bl_core_select_slot(NULL, &hdr));
    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER, bl_core_select_slot(&slot, NULL));
}


void test_select_reports_when_no_image_is_bootable(void)
{
    uint8_t slot = 0xFFu;
    bl_img_hdr_t hdr;

    TEST_ASSERT_EQUAL(BL_ERR_NO_VALID_IMAGE,
        bl_core_select_slot(&slot, &hdr));
}


void test_select_picks_the_only_valid_slot_a(void)
{
    uint8_t slot = 0xFFu;
    bl_img_hdr_t hdr;

    write_valid_image(BL_SLOT_A, 0x00010000u);

    TEST_ASSERT_EQUAL(BL_OK, bl_core_select_slot(&slot, &hdr));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_A, slot);
}


void test_select_picks_the_only_valid_slot_b(void)
{
    uint8_t slot = 0xFFu;
    bl_img_hdr_t hdr;

    write_valid_image(BL_SLOT_B, 0x00010000u);

    TEST_ASSERT_EQUAL(BL_OK, bl_core_select_slot(&slot, &hdr));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_B, slot);
}


/*
 * When both slots hold valid images the newer version must run.
 */
void test_select_prefers_the_newer_version(void)
{
    uint8_t slot = 0xFFu;
    bl_img_hdr_t hdr;

    write_valid_image(BL_SLOT_A, 0x00010000u);
    write_valid_image(BL_SLOT_B, 0x00020000u);

    TEST_ASSERT_EQUAL(BL_OK, bl_core_select_slot(&slot, &hdr));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_B, slot);
    TEST_ASSERT_EQUAL_HEX32(0x00020000u, hdr.fw_version);
}


void test_select_prefers_the_newer_version_in_slot_a(void)
{
    uint8_t slot = 0xFFu;
    bl_img_hdr_t hdr;

    write_valid_image(BL_SLOT_A, 0x00030000u);
    write_valid_image(BL_SLOT_B, 0x00020000u);

    TEST_ASSERT_EQUAL(BL_OK, bl_core_select_slot(&slot, &hdr));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_A, slot);
}


/*
 * Equal versions resolve deterministically so boot selection cannot
 * oscillate between slots.
 */
void test_select_breaks_version_ties_towards_slot_a(void)
{
    uint8_t slot = 0xFFu;
    bl_img_hdr_t hdr;

    write_valid_image(BL_SLOT_A, 0x00010000u);
    write_valid_image(BL_SLOT_B, 0x00010000u);

    TEST_ASSERT_EQUAL(BL_OK, bl_core_select_slot(&slot, &hdr));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_A, slot);
}


/*
 * A newer but corrupt image must not displace an older valid one.
 */
void test_select_ignores_a_newer_corrupt_image(void)
{
    uint8_t slot = 0xFFu;
    bl_img_hdr_t hdr;
    uint8_t payload[TEST_PAYLOAD_SIZE];

    write_valid_image(BL_SLOT_A, 0x00010000u);

    const uint32_t app_base = slot_base_of(BL_SLOT_B) + BL_IMG_HDR_REGION;

    build_payload(payload, sizeof(payload), app_base);

    const bl_img_hdr_t image_hdr = build_header(
        payload, sizeof(payload), 0x00090000u, BL_IMG_HDR_REGION);

    payload[64] ^= 0xFFu;

    write_payload(BL_SLOT_B, BL_IMG_HDR_REGION, payload, sizeof(payload));
    write_header(BL_SLOT_B, &image_hdr);

    TEST_ASSERT_EQUAL(BL_OK, bl_core_select_slot(&slot, &hdr));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_A, slot);
    TEST_ASSERT_EQUAL_HEX32(0x00010000u, hdr.fw_version);
}


/* ------------------------------------------------------------------ */
/* Boot                                                                */
/* ------------------------------------------------------------------ */

/*
 * Booting must transfer control to the payload, not to the slot base.
 */
void test_boot_jumps_to_the_payload(void)
{
    write_valid_image(BL_SLOT_A, 0x00010000u);

    TEST_ASSERT_EQUAL(BL_ERR_JUMP_REFUSED, bl_core_boot());

    TEST_ASSERT_TRUE(bl_host_did_jump());
    TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION,
                            bl_host_jump_target());
}


void test_boot_jumps_to_the_selected_slot(void)
{
    write_valid_image(BL_SLOT_A, 0x00010000u);
    write_valid_image(BL_SLOT_B, 0x00020000u);

    (void)bl_core_boot();

    TEST_ASSERT_TRUE(bl_host_did_jump());
    TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_B) + BL_IMG_HDR_REGION,
                            bl_host_jump_target());
}


/*
 * With nothing bootable the device must remain in the bootloader rather
 * than jumping into an erased slot.
 */
void test_boot_does_not_jump_without_a_valid_image(void)
{
    TEST_ASSERT_EQUAL(BL_ERR_NO_VALID_IMAGE, bl_core_boot());
    TEST_ASSERT_FALSE(bl_host_did_jump());
}


/*
 * An image can pass integrity validation and still present an unusable
 * vector table; the port refuses the transfer and the bootloader reports
 * that distinctly from having found no image at all.
 */
void test_boot_reports_a_refused_jump(void)
{
    uint8_t payload[TEST_PAYLOAD_SIZE];

    /* Valid image whose reset vector lacks the Thumb bit. */
    for (uint32_t i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }

    payload[0] = 0x00;
    payload[1] = 0x00;
    payload[2] = 0x01;
    payload[3] = 0x20;

    const uint32_t reset_vector =
        slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION + 0x100u;

    payload[4] = (uint8_t)(reset_vector & 0xFFu);
    payload[5] = (uint8_t)((reset_vector >> 8) & 0xFFu);
    payload[6] = (uint8_t)((reset_vector >> 16) & 0xFFu);
    payload[7] = (uint8_t)((reset_vector >> 24) & 0xFFu);

    const bl_img_hdr_t hdr = build_header(
        payload, sizeof(payload), 0x00010000u, BL_IMG_HDR_REGION);

    write_payload(BL_SLOT_A, BL_IMG_HDR_REGION, payload, sizeof(payload));
    write_header(BL_SLOT_A, &hdr);

    TEST_ASSERT_EQUAL(BL_ERR_JUMP_REFUSED, bl_core_boot());
    TEST_ASSERT_FALSE(bl_host_did_jump());
}


int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_validate_rejects_null_header);
    RUN_TEST(test_validate_rejects_unknown_slot);

    RUN_TEST(test_validate_accepts_a_valid_image);
    RUN_TEST(test_validate_rejects_an_erased_slot);
    RUN_TEST(test_validate_rejects_a_bad_magic);
    RUN_TEST(test_validate_rejects_an_unsupported_header_version);
    RUN_TEST(test_validate_rejects_a_corrupt_header);

    RUN_TEST(test_validate_rejects_entry_offset_inside_the_header);
    RUN_TEST(test_validate_rejects_a_misaligned_entry_offset);
    RUN_TEST(test_validate_rejects_entry_offset_beyond_the_slot);

    RUN_TEST(test_validate_rejects_a_zero_length_image);
    RUN_TEST(test_validate_rejects_an_image_larger_than_the_slot);
    RUN_TEST(test_validate_accepts_an_image_that_fills_the_slot);

    RUN_TEST(test_validate_rejects_a_corrupt_payload);
    RUN_TEST(test_validate_rejects_a_truncated_payload);
    RUN_TEST(test_validate_leaves_output_untouched_on_failure);

    RUN_TEST(test_select_rejects_null_arguments);
    RUN_TEST(test_select_reports_when_no_image_is_bootable);
    RUN_TEST(test_select_picks_the_only_valid_slot_a);
    RUN_TEST(test_select_picks_the_only_valid_slot_b);
    RUN_TEST(test_select_prefers_the_newer_version);
    RUN_TEST(test_select_prefers_the_newer_version_in_slot_a);
    RUN_TEST(test_select_breaks_version_ties_towards_slot_a);
    RUN_TEST(test_select_ignores_a_newer_corrupt_image);

    RUN_TEST(test_boot_jumps_to_the_payload);
    RUN_TEST(test_boot_jumps_to_the_selected_slot);
    RUN_TEST(test_boot_does_not_jump_without_a_valid_image);
    RUN_TEST(test_boot_reports_a_refused_jump);

    return UNITY_END();
}
