#include "unity.h"
#include "bl_core.h"
#include "bl_crc32.h"
#include "bl_frame.h"
#include "bl_meta.h"
#include "bl_port.h"
#include "bl_port_host.h"
#include "bl_proto.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * Tests for the update command handler and for trial-boot rollback.
 *
 * Requests are built with bl_frame_encode() and decoded responses are
 * parsed with the same frame parser the device uses, so the tests
 * exercise the wire format rather than an internal shortcut.
 */

#define TEST_RESPONSE_CAPACITY (BL_MAX_PAYLOAD + 8u)
#define TEST_PAYLOAD_SIZE 512u
#define TEST_INITIAL_MSP 0x20010000u

static bl_session_t session;
static uint8_t response[TEST_RESPONSE_CAPACITY];
static size_t response_len;


void setUp(void)
{
    bl_port_init();

    memset(&session, 0, sizeof(session));
    memset(response, 0, sizeof(response));
    response_len = 0u;
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
 * Build a request frame, hand it to the command handler, and decode the
 * response into out_frame.
 */
static bl_result_t exchange(uint8_t cmd,
                            const uint8_t *payload,
                            uint16_t payload_len,
                            bl_frame_t *out_frame)
{
    uint8_t request_bytes[TEST_RESPONSE_CAPACITY];

    const size_t request_len = bl_frame_encode(
        cmd, payload, payload_len, request_bytes, sizeof(request_bytes));

    TEST_ASSERT_NOT_EQUAL(0u, request_len);

    /* Decode the request the same way the device would. */
    bl_frame_parser_t parser;
    bl_frame_parser_init(&parser);

    bl_frame_status_t status = BL_FRAME_INCOMPLETE;

    for (size_t i = 0u; i < request_len; ++i) {
        status = bl_frame_feed(&parser, request_bytes[i]);
    }

    TEST_ASSERT_EQUAL(BL_FRAME_COMPLETE, status);

    const bl_result_t result = bl_core_handle_frame(
        &session, &parser.frame, response, sizeof(response), &response_len);

    if (result != BL_OK) {
        return result;
    }

    /* Decode the response. */
    bl_frame_parser_t response_parser;
    bl_frame_parser_init(&response_parser);

    status = BL_FRAME_INCOMPLETE;

    for (size_t i = 0u; i < response_len; ++i) {
        status = bl_frame_feed(&response_parser, response[i]);
    }

    TEST_ASSERT_EQUAL(BL_FRAME_COMPLETE, status);
    TEST_ASSERT_EQUAL_UINT8(cmd, response_parser.frame.cmd);
    TEST_ASSERT_GREATER_THAN_UINT16(0u, response_parser.frame.len);

    *out_frame = response_parser.frame;

    return BL_OK;
}


/*
 * Send a command and return only the status byte from the response.
 */
static uint8_t status_of(uint8_t cmd,
                         const uint8_t *payload,
                         uint16_t payload_len)
{
    bl_frame_t frame;

    TEST_ASSERT_EQUAL(BL_OK, exchange(cmd, payload, payload_len, &frame));

    return frame.payload[0];
}


static uint32_t get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}


static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}


static void build_payload(uint8_t *payload, uint32_t len, uint32_t app_base)
{
    const uint32_t reset_vector = (app_base + 0x100u) | 1u;

    for (uint32_t i = 0u; i < len; ++i) {
        payload[i] = (uint8_t)(i * 7u + 3u);
    }

    put_u32_le(&payload[0], TEST_INITIAL_MSP);
    put_u32_le(&payload[4], reset_vector);
}


static bl_img_hdr_t build_header(const uint8_t *payload,
                                 uint32_t len,
                                 uint32_t fw_version)
{
    bl_img_hdr_t hdr;

    memset(&hdr, 0, sizeof(hdr));

    hdr.magic = BL_IMG_MAGIC;
    hdr.hdr_version = (uint16_t)BL_IMG_HDR_VERSION;
    hdr.img_size = len;
    hdr.img_crc32 = bl_crc32(0u, payload, len);
    hdr.fw_version = fw_version;
    hdr.entry_offset = BL_IMG_HDR_REGION;
    hdr.hdr_crc32 = bl_crc32(0u, &hdr, offsetof(bl_img_hdr_t, hdr_crc32));

    return hdr;
}


/*
 * Send data as a sequence of WRITE requests.
 *
 * A WRITE payload carries a four-byte offset ahead of the data, so no
 * single request can transfer BL_MAX_PAYLOAD bytes of firmware.
 */
static void write_payload_in_chunks(uint32_t offset,
                                    const uint8_t *data,
                                    uint32_t len)
{
    uint8_t request[4u + BL_MAX_WRITE_DATA];
    uint32_t sent = 0u;

    while (sent < len) {
        uint32_t chunk = len - sent;

        if (chunk > BL_MAX_WRITE_DATA) {
            chunk = BL_MAX_WRITE_DATA;
        }

        put_u32_le(request, offset + sent);
        memcpy(&request[4], &data[sent], chunk);

        TEST_ASSERT_EQUAL_UINT8(BL_OK,
            status_of(BL_CMD_WRITE, request, (uint16_t)(4u + chunk)));

        sent += chunk;
    }
}


/*
 * Perform a complete update of a slot through the protocol: erase, write
 * the payload, then commit the header last.
 */
static void update_slot_via_protocol(uint8_t slot, uint32_t fw_version)
{
    uint8_t payload[TEST_PAYLOAD_SIZE];

    build_payload(payload, sizeof(payload),
                  slot_base_of(slot) + BL_IMG_HDR_REGION);

    const bl_img_hdr_t hdr =
        build_header(payload, sizeof(payload), fw_version);

    const uint8_t slot_arg = slot;

    TEST_ASSERT_EQUAL_UINT8(BL_OK,
        status_of(BL_CMD_ERASE_SLOT, &slot_arg, 1u));

    write_payload_in_chunks(BL_IMG_HDR_REGION, payload, sizeof(payload));

    uint8_t header_payload[4u + sizeof(bl_img_hdr_t)];

    put_u32_le(header_payload, 0u);
    memcpy(&header_payload[4], &hdr, sizeof(hdr));

    TEST_ASSERT_EQUAL_UINT8(BL_OK,
        status_of(BL_CMD_WRITE, header_payload,
                  (uint16_t)(4u + sizeof(hdr))));
}


/* ------------------------------------------------------------------ */
/* Handler contract                                                    */
/* ------------------------------------------------------------------ */

void test_handler_rejects_null_arguments(void)
{
    bl_frame_t frame;

    memset(&frame, 0, sizeof(frame));

    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER,
        bl_core_handle_frame(NULL, &frame, response, sizeof(response),
                             &response_len));

    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER,
        bl_core_handle_frame(&session, NULL, response, sizeof(response),
                             &response_len));

    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER,
        bl_core_handle_frame(&session, &frame, NULL, sizeof(response),
                             &response_len));

    TEST_ASSERT_EQUAL(BL_ERR_NULL_POINTER,
        bl_core_handle_frame(&session, &frame, response, sizeof(response),
                             NULL));
}


/*
 * A response buffer too small for even a status-only frame must be
 * reported rather than producing a truncated response.
 */
void test_handler_reports_an_undersized_response_buffer(void)
{
    bl_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.cmd = BL_CMD_HELLO;
    frame.len = 0u;

    uint8_t tiny[4];

    TEST_ASSERT_NOT_EQUAL(BL_OK,
        bl_core_handle_frame(&session, &frame, tiny, sizeof(tiny),
                             &response_len));
}


/*
 * An unrecognised command must be answered, with its command byte
 * echoed, so the host can distinguish rejection from an unresponsive
 * device.
 */
void test_unknown_command_is_rejected_with_the_command_echoed(void)
{
    bl_frame_t frame;

    TEST_ASSERT_EQUAL(BL_OK, exchange(0x7Au, NULL, 0u, &frame));
    TEST_ASSERT_EQUAL_UINT8(0x7Au, frame.cmd);
    TEST_ASSERT_EQUAL_UINT8(BL_ERR_INVALID_PACKET, frame.payload[0]);
}


/* ------------------------------------------------------------------ */
/* HELLO                                                               */
/* ------------------------------------------------------------------ */

/*
 * HELLO must report the device's geometry so the host can size its
 * transfers and reject oversized images before erasing anything.
 */
void test_hello_reports_device_geometry(void)
{
    bl_frame_t frame;

    TEST_ASSERT_EQUAL(BL_OK, exchange(BL_CMD_HELLO, NULL, 0u, &frame));

    TEST_ASSERT_EQUAL_UINT16(18u, frame.len);
    TEST_ASSERT_EQUAL_UINT8(BL_OK, frame.payload[0]);
    TEST_ASSERT_EQUAL_UINT8(BL_PROTO_VERSION, frame.payload[1]);
    TEST_ASSERT_EQUAL_UINT8(BL_IMG_HDR_VERSION, frame.payload[2]);
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_COUNT, frame.payload[3]);
    TEST_ASSERT_EQUAL_UINT32(slot_size_of(BL_SLOT_A),
                             get_u32_le(&frame.payload[4]));
    TEST_ASSERT_EQUAL_UINT32(bl_flash_write_granularity(),
                             get_u32_le(&frame.payload[8]));
    TEST_ASSERT_EQUAL_UINT32(
        bl_flash_erase_granularity(slot_base_of(BL_SLOT_A)),
        get_u32_le(&frame.payload[12]));

    /*
     * The reported write size must be usable directly as a chunk size,
     * including the four-byte offset the request carries.
     */
    const uint16_t max_write =
        (uint16_t)((uint16_t)frame.payload[16] |
                   (uint16_t)((uint16_t)frame.payload[17] << 8));

    TEST_ASSERT_EQUAL_UINT16(BL_MAX_WRITE_DATA, max_write);
    TEST_ASSERT_TRUE((uint32_t)max_write + 4u <= BL_MAX_PAYLOAD);
    TEST_ASSERT_EQUAL_UINT32(0u, max_write % bl_flash_write_granularity());
}


/*
 * A chunk size taken from HELLO must produce a request the frame encoder
 * can actually represent.
 */
void test_hello_write_size_fits_in_a_frame(void)
{
    uint8_t request[4u + BL_MAX_WRITE_DATA];
    uint8_t encoded[BL_MAX_PAYLOAD + 8u];

    memset(request, 0xA5, sizeof(request));

    TEST_ASSERT_NOT_EQUAL(0u,
        bl_frame_encode(BL_CMD_WRITE, request, (uint16_t)sizeof(request),
                        encoded, sizeof(encoded)));
}


void test_hello_rejects_a_payload(void)
{
    const uint8_t junk = 0x00u;

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_INVALID_PACKET,
        status_of(BL_CMD_HELLO, &junk, 1u));
}


/* ------------------------------------------------------------------ */
/* ERASE_SLOT                                                          */
/* ------------------------------------------------------------------ */

void test_erase_clears_the_slot(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t buf[16];

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_read(slot_base_of(BL_SLOT_A), buf, sizeof(buf)));

    for (size_t i = 0u; i < sizeof(buf); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, buf[i]);
    }

    TEST_ASSERT_TRUE(session.erased);
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_A, session.target_slot);
}


void test_erase_rejects_an_unknown_slot(void)
{
    const uint8_t slot = BL_SLOT_COUNT;

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_INVALID_ARGUMENT,
        status_of(BL_CMD_ERASE_SLOT, &slot, 1u));
}


void test_erase_rejects_a_malformed_payload(void)
{
    TEST_ASSERT_EQUAL_UINT8(BL_ERR_INVALID_PACKET,
        status_of(BL_CMD_ERASE_SLOT, NULL, 0u));
}


/* ------------------------------------------------------------------ */
/* WRITE                                                               */
/* ------------------------------------------------------------------ */

/*
 * Writing before erasing would program flash twice without an
 * intervening erase, which the target cannot do.
 */
void test_write_requires_a_prior_erase(void)
{
    uint8_t payload[12];

    memset(payload, 0xA5, sizeof(payload));
    put_u32_le(payload, BL_IMG_HDR_REGION);

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_NOT_ERASED,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));
}


void test_write_stores_data_and_echoes_the_offset(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t payload[4u + 16u];

    put_u32_le(payload, BL_IMG_HDR_REGION);

    for (uint32_t i = 0u; i < 16u; ++i) {
        payload[4u + i] = (uint8_t)(0x10u + i);
    }

    bl_frame_t frame;

    TEST_ASSERT_EQUAL(BL_OK,
        exchange(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload), &frame));

    TEST_ASSERT_EQUAL_UINT8(BL_OK, frame.payload[0]);
    TEST_ASSERT_EQUAL_UINT32(BL_IMG_HDR_REGION,
                             get_u32_le(&frame.payload[1]));

    uint8_t stored[16];

    TEST_ASSERT_EQUAL(BL_OK,
        bl_flash_read(slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION,
                      stored, sizeof(stored)));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(&payload[4], stored, sizeof(stored));
    TEST_ASSERT_EQUAL_UINT32(BL_IMG_HDR_REGION + 16u,
                             session.highest_offset);
}


void test_write_rejects_a_misaligned_offset(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t payload[4u + 8u];

    memset(payload, 0x00, sizeof(payload));
    put_u32_le(payload, BL_IMG_HDR_REGION + 1u);

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_INVALID_ARGUMENT,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));
}


void test_write_rejects_a_misaligned_length(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t payload[4u + 5u];

    memset(payload, 0x00, sizeof(payload));
    put_u32_le(payload, BL_IMG_HDR_REGION);

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_INVALID_ARGUMENT,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));
}


/*
 * An offset near the end of the slot must be rejected using the slot
 * bounds rather than being allowed to write past them.
 */
void test_write_rejects_data_past_the_end_of_the_slot(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t payload[4u + 16u];

    memset(payload, 0x00, sizeof(payload));
    put_u32_le(payload, slot_size_of(BL_SLOT_A) - 8u);

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_IMAGE_SIZE,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));
}


void test_write_rejects_a_payload_without_data(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t payload[4];

    put_u32_le(payload, 0u);

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_INVALID_PACKET,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));
}


/*
 * Programming the same location twice between erases produces
 * conflicting ECC bits on the target, after which reading that location
 * faults. A retransmission after a lost acknowledgement is ordinary
 * behaviour, so the device must reject the repeat itself.
 */
void test_write_rejects_a_repeated_offset(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t payload[4u + 16u];

    memset(payload, 0x11, sizeof(payload));
    put_u32_le(payload, BL_IMG_HDR_REGION);

    TEST_ASSERT_EQUAL_UINT8(BL_OK,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_NOT_ERASED,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));
}


/*
 * A write that partially overlaps an earlier one must also be rejected,
 * not only an exact repeat.
 */
void test_write_rejects_a_partially_overlapping_offset(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t first[4u + 32u];

    memset(first, 0x22, sizeof(first));
    put_u32_le(first, BL_IMG_HDR_REGION);

    TEST_ASSERT_EQUAL_UINT8(BL_OK,
        status_of(BL_CMD_WRITE, first, (uint16_t)sizeof(first)));

    uint8_t overlapping[4u + 32u];

    memset(overlapping, 0x33, sizeof(overlapping));
    put_u32_le(overlapping, BL_IMG_HDR_REGION + 16u);

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_NOT_ERASED,
        status_of(BL_CMD_WRITE, overlapping, (uint16_t)sizeof(overlapping)));
}


/*
 * The header is committed once, out of order, at offset zero. A second
 * header write must be rejected.
 */
void test_write_rejects_a_repeated_header(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t header[4u + sizeof(bl_img_hdr_t)];

    memset(header, 0x44, sizeof(header));
    put_u32_le(header, 0u);

    TEST_ASSERT_EQUAL_UINT8(BL_OK,
        status_of(BL_CMD_WRITE, header, (uint16_t)sizeof(header)));

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_NOT_ERASED,
        status_of(BL_CMD_WRITE, header, (uint16_t)sizeof(header)));
}


/*
 * Re-erasing the slot must permit the same offsets to be written again,
 * so a failed update can simply be retried from the start.
 */
void test_re_erasing_permits_rewriting(void)
{
    const uint8_t slot = BL_SLOT_A;

    uint8_t payload[4u + 16u];

    memset(payload, 0x55, sizeof(payload));
    put_u32_le(payload, BL_IMG_HDR_REGION);

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));
    TEST_ASSERT_EQUAL_UINT8(BL_OK,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));
    TEST_ASSERT_EQUAL_UINT8(BL_OK,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));
}


/*
 * Writes need not ascend across the header boundary: the header is
 * committed at offset 0 after the payload has been stored, which is what
 * makes an interrupted update cleanly invalid.
 */
void test_write_accepts_the_header_after_the_payload(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);

    bl_img_hdr_t hdr;

    TEST_ASSERT_EQUAL(BL_OK, bl_core_validate_slot(BL_SLOT_A, &hdr));
}


/*
 * A flash failure must be reported to the host rather than silently
 * dropped.
 */
void test_write_reports_a_flash_failure(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    uint8_t payload[4u + 8u];

    memset(payload, 0x5A, sizeof(payload));
    put_u32_le(payload, BL_IMG_HDR_REGION);

    bl_host_fail_after_n_writes(0u);

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_FLASH_PROGRAM,
        status_of(BL_CMD_WRITE, payload, (uint16_t)sizeof(payload)));

    bl_host_clear_failures();
}


/* ------------------------------------------------------------------ */
/* VERIFY                                                              */
/* ------------------------------------------------------------------ */

void test_verify_reports_image_details(void)
{
    update_slot_via_protocol(BL_SLOT_B, 0x00020304u);

    const uint8_t slot = BL_SLOT_B;
    bl_frame_t frame;

    TEST_ASSERT_EQUAL(BL_OK, exchange(BL_CMD_VERIFY, &slot, 1u, &frame));

    TEST_ASSERT_EQUAL_UINT8(BL_OK, frame.payload[0]);
    TEST_ASSERT_EQUAL_UINT16(13u, frame.len);
    TEST_ASSERT_EQUAL_UINT32(TEST_PAYLOAD_SIZE,
                             get_u32_le(&frame.payload[1]));
    TEST_ASSERT_EQUAL_HEX32(0x00020304u, get_u32_le(&frame.payload[9]));
}


/*
 * The specific validation failure must reach the host, so an operator
 * learns which stage of the update failed.
 */
void test_verify_propagates_the_validation_failure(void)
{
    const uint8_t slot = BL_SLOT_A;

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_MAGIC,
        status_of(BL_CMD_VERIFY, &slot, 1u));
}


void test_verify_rejects_an_unknown_slot(void)
{
    const uint8_t slot = BL_SLOT_COUNT;

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_INVALID_ARGUMENT,
        status_of(BL_CMD_VERIFY, &slot, 1u));
}


/* ------------------------------------------------------------------ */
/* SET_ACTIVE                                                          */
/* ------------------------------------------------------------------ */

void test_set_active_arms_a_trial_boot(void)
{
    update_slot_via_protocol(BL_SLOT_B, 0x00020000u);

    const uint8_t slot = BL_SLOT_B;

    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_SET_ACTIVE, &slot, 1u));

    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_B, meta.active_slot);
    TEST_ASSERT_EQUAL_UINT8(BL_BOOT_TRIAL, meta.state);
    TEST_ASSERT_EQUAL_UINT8(0u, meta.boot_attempts);
}


/*
 * A slot without a valid image must not be armed, so a failed update
 * cannot nominate an unbootable slot.
 */
void test_set_active_refuses_a_slot_without_a_valid_image(void)
{
    const uint8_t slot = BL_SLOT_B;

    TEST_ASSERT_EQUAL_UINT8(BL_ERR_MAGIC,
        status_of(BL_CMD_SET_ACTIVE, &slot, 1u));

    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_ERR_NO_METADATA, bl_meta_read(&meta));
}


/* ------------------------------------------------------------------ */
/* RESET                                                              */
/* ------------------------------------------------------------------ */

/*
 * The reset must be deferred so the acknowledgement can be transmitted
 * before the device restarts.
 */
void test_reset_is_acknowledged_before_being_performed(void)
{
    TEST_ASSERT_EQUAL_UINT8(BL_OK, status_of(BL_CMD_RESET, NULL, 0u));

    TEST_ASSERT_TRUE(session.reset_requested);
    TEST_ASSERT_FALSE(bl_host_did_reset());
}


/* ------------------------------------------------------------------ */
/* Trial boot and rollback                                             */
/* ------------------------------------------------------------------ */

/*
 * A confirmed image boots without consuming attempts.
 */
void test_confirmed_image_boots_repeatedly(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_A));
    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());

    for (int i = 0; i < 5; ++i) {
        (void)bl_core_boot();

        TEST_ASSERT_TRUE(bl_host_did_jump());
        TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION,
                                bl_host_jump_target());
    }

    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT8(BL_BOOT_CONFIRMED, meta.state);
    TEST_ASSERT_EQUAL_UINT8(0u, meta.boot_attempts);
}


/*
 * Each boot of a trial image consumes one attempt, and the counter is
 * advanced before control is transferred so an image that hangs still
 * consumes one.
 */
void test_trial_boot_consumes_attempts(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_A));

    bl_meta_t meta;

    for (uint8_t i = 1u; i <= BL_MAX_BOOT_ATTEMPTS; ++i) {
        (void)bl_core_boot();

        TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
        TEST_ASSERT_EQUAL_UINT8(i, meta.boot_attempts);
    }
}


/*
 * Confirming clears the trial state so later boots no longer consume
 * attempts.
 */
void test_confirm_ends_the_trial(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_A));

    (void)bl_core_boot();
    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());

    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT8(BL_BOOT_CONFIRMED, meta.state);
    TEST_ASSERT_EQUAL_UINT8(0u, meta.boot_attempts);
}


void test_confirm_is_idempotent(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_A));

    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());
    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());
    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());
}


void test_confirm_without_metadata_is_reported(void)
{
    TEST_ASSERT_EQUAL(BL_ERR_NO_METADATA, bl_core_confirm());
}


/*
 * The central rollback behaviour: a trial image that never confirms
 * itself must be abandoned in favour of the previous image.
 */
void test_unconfirmed_trial_image_is_rolled_back(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_A));
    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());

    /* A newer image is installed into slot B and armed for trial. */
    update_slot_via_protocol(BL_SLOT_B, 0x00020000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_B));

    /* It boots but never confirms. */
    for (uint8_t i = 0u; i < BL_MAX_BOOT_ATTEMPTS; ++i) {
        bl_host_reboot();
        (void)bl_core_boot();

        TEST_ASSERT_TRUE(bl_host_did_jump());
        TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_B) + BL_IMG_HDR_REGION,
                                bl_host_jump_target());
    }

    /* The next boot reverts to the previously confirmed image. */
    bl_host_reboot();
    (void)bl_core_boot();

    TEST_ASSERT_TRUE(bl_host_did_jump());
    TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION,
                            bl_host_jump_target());

    bl_meta_t meta;

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_read(&meta));
    TEST_ASSERT_EQUAL_UINT8(BL_SLOT_A, meta.active_slot);
    TEST_ASSERT_EQUAL_UINT8(BL_BOOT_CONFIRMED, meta.state);
}


/*
 * A trial image that confirms itself on its first boot must survive
 * indefinitely.
 */
void test_confirmed_trial_image_is_not_rolled_back(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_A));
    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());

    update_slot_via_protocol(BL_SLOT_B, 0x00020000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_B));

    bl_host_reboot();
    (void)bl_core_boot();
    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());

    for (uint8_t i = 0u; i < BL_MAX_BOOT_ATTEMPTS + 2u; ++i) {
        bl_host_reboot();
        (void)bl_core_boot();

        TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_B) + BL_IMG_HDR_REGION,
                                bl_host_jump_target());
    }
}


/*
 * If the previous image is no longer valid there is nothing to roll back
 * to, so a failing trial image must keep being booted rather than
 * leaving the device unbootable.
 */
void test_rollback_is_skipped_when_no_fallback_exists(void)
{
    update_slot_via_protocol(BL_SLOT_B, 0x00020000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_B));

    for (uint8_t i = 0u; i < BL_MAX_BOOT_ATTEMPTS + 2u; ++i) {
        bl_host_reboot();
        (void)bl_core_boot();

        TEST_ASSERT_TRUE(bl_host_did_jump());
        TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_B) + BL_IMG_HDR_REGION,
                                bl_host_jump_target());
    }
}


/*
 * With metadata present but the nominated slot damaged, the bootloader
 * must fall back to the other slot.
 */
void test_boot_falls_back_when_the_active_slot_is_damaged(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);
    update_slot_via_protocol(BL_SLOT_B, 0x00020000u);

    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_B));
    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());

    /* Damage slot B's payload. */
    uint8_t damage[8];

    memset(damage, 0x00, sizeof(damage));
    bl_host_flash_poke(slot_base_of(BL_SLOT_B) + BL_IMG_HDR_REGION + 64u,
                       damage, sizeof(damage));

    (void)bl_core_boot();

    TEST_ASSERT_TRUE(bl_host_did_jump());
    TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION,
                            bl_host_jump_target());
}


/*
 * Without metadata the bootloader selects by firmware version, so a
 * device programmed directly through SWD still boots.
 */
void test_boot_without_metadata_selects_by_version(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);
    update_slot_via_protocol(BL_SLOT_B, 0x00030000u);

    TEST_ASSERT_EQUAL(BL_OK, bl_meta_erase_all());

    (void)bl_core_boot();

    TEST_ASSERT_TRUE(bl_host_did_jump());
    TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_B) + BL_IMG_HDR_REGION,
                            bl_host_jump_target());
}


/* ------------------------------------------------------------------ */
/* Interrupted updates                                                 */
/* ------------------------------------------------------------------ */

/*
 * An update interrupted before the header is committed must leave the
 * slot cleanly invalid, and the previously active image must still boot.
 */
void test_an_interrupted_update_leaves_the_previous_image_bootable(void)
{
    update_slot_via_protocol(BL_SLOT_A, 0x00010000u);
    TEST_ASSERT_EQUAL(BL_OK, bl_core_mark_pending(BL_SLOT_A));
    TEST_ASSERT_EQUAL(BL_OK, bl_core_confirm());

    /* Begin updating slot B, then stop before committing the header. */
    uint8_t payload[TEST_PAYLOAD_SIZE];

    build_payload(payload, sizeof(payload),
                  slot_base_of(BL_SLOT_B) + BL_IMG_HDR_REGION);

    const uint8_t slot = BL_SLOT_B;

    TEST_ASSERT_EQUAL_UINT8(BL_OK,
        status_of(BL_CMD_ERASE_SLOT, &slot, 1u));

    write_payload_in_chunks(BL_IMG_HDR_REGION, payload, sizeof(payload));

    /* Slot B has no header, so it must not validate. */
    bl_img_hdr_t hdr;

    TEST_ASSERT_EQUAL(BL_ERR_MAGIC, bl_core_validate_slot(BL_SLOT_B, &hdr));

    (void)bl_core_boot();

    TEST_ASSERT_EQUAL_HEX32(slot_base_of(BL_SLOT_A) + BL_IMG_HDR_REGION,
                            bl_host_jump_target());
}


int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_handler_rejects_null_arguments);
    RUN_TEST(test_handler_reports_an_undersized_response_buffer);
    RUN_TEST(test_unknown_command_is_rejected_with_the_command_echoed);

    RUN_TEST(test_hello_reports_device_geometry);
    RUN_TEST(test_hello_write_size_fits_in_a_frame);
    RUN_TEST(test_hello_rejects_a_payload);

    RUN_TEST(test_erase_clears_the_slot);
    RUN_TEST(test_erase_rejects_an_unknown_slot);
    RUN_TEST(test_erase_rejects_a_malformed_payload);

    RUN_TEST(test_write_requires_a_prior_erase);
    RUN_TEST(test_write_stores_data_and_echoes_the_offset);
    RUN_TEST(test_write_rejects_a_misaligned_offset);
    RUN_TEST(test_write_rejects_a_misaligned_length);
    RUN_TEST(test_write_rejects_data_past_the_end_of_the_slot);
    RUN_TEST(test_write_rejects_a_payload_without_data);
    RUN_TEST(test_write_rejects_a_repeated_offset);
    RUN_TEST(test_write_rejects_a_partially_overlapping_offset);
    RUN_TEST(test_write_rejects_a_repeated_header);
    RUN_TEST(test_re_erasing_permits_rewriting);
    RUN_TEST(test_write_accepts_the_header_after_the_payload);
    RUN_TEST(test_write_reports_a_flash_failure);

    RUN_TEST(test_verify_reports_image_details);
    RUN_TEST(test_verify_propagates_the_validation_failure);
    RUN_TEST(test_verify_rejects_an_unknown_slot);

    RUN_TEST(test_set_active_arms_a_trial_boot);
    RUN_TEST(test_set_active_refuses_a_slot_without_a_valid_image);

    RUN_TEST(test_reset_is_acknowledged_before_being_performed);

    RUN_TEST(test_confirmed_image_boots_repeatedly);
    RUN_TEST(test_trial_boot_consumes_attempts);
    RUN_TEST(test_confirm_ends_the_trial);
    RUN_TEST(test_confirm_is_idempotent);
    RUN_TEST(test_confirm_without_metadata_is_reported);

    RUN_TEST(test_unconfirmed_trial_image_is_rolled_back);
    RUN_TEST(test_confirmed_trial_image_is_not_rolled_back);
    RUN_TEST(test_rollback_is_skipped_when_no_fallback_exists);
    RUN_TEST(test_boot_falls_back_when_the_active_slot_is_damaged);
    RUN_TEST(test_boot_without_metadata_selects_by_version);

    RUN_TEST(test_an_interrupted_update_leaves_the_previous_image_bootable);

    return UNITY_END();
}
