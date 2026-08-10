#include "unity.h"
#include "bl_frame.h"
#include "bl_proto.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define TEST_BUFFER_SIZE (BL_MAX_PAYLOAD + 8u)


static bl_frame_parser_t parser;


void setUp(void)
{
    bl_frame_parser_init(&parser);
}


void tearDown(void)
{
}


/*
 * Feed an encoded frame byte-by-byte and return the final parser status.
 */
static bl_frame_status_t feed_frame(const uint8_t *frame, size_t len)
{
    bl_frame_status_t status = BL_FRAME_INCOMPLETE;

    for(size_t i = 0; i < len; i++){
        status = bl_frame_feed(&parser, frame[i]);
    }

    return status;
}


/*
 * Encode a frame and immediately decode it again.
 *
 * This exercises both sides of the framing layer at once.
 */
void test_encode_then_decode(void)
{
    const uint8_t payload[] = {
        0x10, 0x20, 0x30, 0x40
    };

    uint8_t encoded[TEST_BUFFER_SIZE];

    size_t encoded_len = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        encoded,
        sizeof(encoded)
    );

    TEST_ASSERT_EQUAL_UINT16(12, encoded_len);

    bl_frame_status_t status = feed_frame(encoded, encoded_len);

    TEST_ASSERT_EQUAL(BL_FRAME_COMPLETE, status);
    TEST_ASSERT_EQUAL(BL_CMD_WRITE, parser.frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), parser.frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload,
        parser.frame.payload,
        sizeof(payload)
    );
}


/*
 * Garbage before SOF must be ignored.
 */
void test_garbage_before_sof_is_ignored(void)
{
    const uint8_t payload[] = {
        0xDE, 0xAD, 0xBE, 0xEF
    };

    /*
     * Deliberately contains no BL_SOF byte, so the parser must remain
     * in BL_FRAME_WAIT_SOF throughout.
     */
    const uint8_t garbage[] = {
        0x00, 0x12, 0x55, 0xAA, 0xFF
    };

    uint8_t frame[TEST_BUFFER_SIZE];

    size_t frame_len = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        frame,
        sizeof(frame)
    );

    TEST_ASSERT_NOT_EQUAL(0, frame_len);

    for(size_t i = 0; i < sizeof(garbage); i++){
        TEST_ASSERT_EQUAL(
            BL_FRAME_INCOMPLETE,
            bl_frame_feed(&parser, garbage[i])
        );
    }

    TEST_ASSERT_EQUAL(BL_FRAME_WAIT_SOF, parser.state);

    /* A valid frame following the garbage must still parse. */
    TEST_ASSERT_EQUAL(
        BL_FRAME_COMPLETE,
        feed_frame(frame, frame_len)
    );

    TEST_ASSERT_EQUAL(BL_CMD_WRITE, parser.frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), parser.frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload,
        parser.frame.payload,
        sizeof(payload)
    );
}


/*
 * A corrupted payload byte must cause CRC validation to fail.
 */
void test_corrupted_payload_is_rejected(void)
{
    const uint8_t payload[] = {
        0x11, 0x22, 0x33, 0x44
    };

    uint8_t frame[TEST_BUFFER_SIZE];

    size_t frame_len = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        frame,
        sizeof(frame)
    );

    TEST_ASSERT_NOT_EQUAL(0, frame_len);

    /* Corrupt one payload byte. */
    frame[4] ^= 0xFF;

    bl_frame_status_t status = feed_frame(frame, frame_len);

    TEST_ASSERT_EQUAL(BL_FRAME_ERROR, status);
}


/*
 * An externally supplied length larger than BL_MAX_PAYLOAD must be
 * rejected before any payload bytes are written.
 */
void test_oversized_length_is_rejected(void)
{
    const uint16_t oversized_len = 60000;

    TEST_ASSERT_EQUAL(
        BL_FRAME_INCOMPLETE,
        bl_frame_feed(&parser, BL_SOF)
    );

    TEST_ASSERT_EQUAL(
        BL_FRAME_INCOMPLETE,
        bl_frame_feed(&parser, BL_CMD_WRITE)
    );

    /* Little-endian length: 60000 = 0xEA60. */
    TEST_ASSERT_EQUAL(
        BL_FRAME_INCOMPLETE,
        bl_frame_feed(&parser, (uint8_t)(oversized_len & 0xFF))
    );

    TEST_ASSERT_EQUAL(
        BL_FRAME_ERROR,
        bl_frame_feed(&parser, (uint8_t)(oversized_len >> 8))
    );

    /*
     * Parser must have been reset after the error.
     */
    TEST_ASSERT_EQUAL(BL_FRAME_WAIT_SOF, parser.state);
}


/*
 * A frame may be split across multiple calls to bl_frame_feed().
 */
void test_frame_can_be_split_across_feed_calls(void)
{
    const uint8_t payload[] = {
        0x01, 0x02, 0x03, 0x04, 0x05
    };

    uint8_t frame[TEST_BUFFER_SIZE];

    size_t frame_len = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        frame,
        sizeof(frame)
    );

    TEST_ASSERT_NOT_EQUAL(0, frame_len);

    for(size_t i = 0; i < frame_len; i++){
        bl_frame_status_t status = bl_frame_feed(&parser, frame[i]);

        if(i < frame_len - 1){
            TEST_ASSERT_EQUAL(BL_FRAME_INCOMPLETE, status);
        }
        else{
            TEST_ASSERT_EQUAL(BL_FRAME_COMPLETE, status);
        }
    }

    TEST_ASSERT_EQUAL(BL_CMD_WRITE, parser.frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), parser.frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload,
        parser.frame.payload,
        sizeof(payload)
    );
}


/*
 * Zero-length payloads are valid frames.
 */
void test_zero_length_payload(void)
{
    uint8_t frame[TEST_BUFFER_SIZE];

    size_t frame_len = bl_frame_encode(
        BL_CMD_HELLO,
        NULL,
        0,
        frame,
        sizeof(frame)
    );

    TEST_ASSERT_EQUAL_UINT16(8, frame_len);

    bl_frame_status_t status = feed_frame(frame, frame_len);

    TEST_ASSERT_EQUAL(BL_FRAME_COMPLETE, status);
    TEST_ASSERT_EQUAL(BL_CMD_HELLO, parser.frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(0, parser.frame.len);
}


/*
 * SOF appearing inside a payload must be treated as ordinary data.
 */
void test_sof_inside_payload_does_not_break_frame(void)
{
    const uint8_t payload[] = {
        0x01,
        BL_SOF,
        0x02,
        0x03,
        BL_SOF,
        0x04
    };

    uint8_t frame[TEST_BUFFER_SIZE];

    size_t frame_len = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        frame,
        sizeof(frame)
    );

    TEST_ASSERT_NOT_EQUAL(0, frame_len);

    bl_frame_status_t status = feed_frame(frame, frame_len);

    TEST_ASSERT_EQUAL(BL_FRAME_COMPLETE, status);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), parser.frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload,
        parser.frame.payload,
        sizeof(payload)
    );
}


/*
 * Corrupting one CRC byte must cause the frame to be rejected.
 */
void test_corrupted_crc_is_rejected(void)
{
    const uint8_t payload[] = {
        0xAA, 0xBB, 0xCC
    };

    uint8_t frame[TEST_BUFFER_SIZE];

    size_t frame_len = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        frame,
        sizeof(frame)
    );

    TEST_ASSERT_NOT_EQUAL(0, frame_len);

    /* CRC starts immediately after SOF + CMD + LEN + PAYLOAD. */
    frame[4 + sizeof(payload)] ^= 0x01;

    bl_frame_status_t status = feed_frame(frame, frame_len);

    TEST_ASSERT_EQUAL(BL_FRAME_ERROR, status);
}


/*
 * Encoding must fail when the output buffer is too small.
 */
void test_encode_rejects_small_output_buffer(void)
{
    const uint8_t payload[] = {
        0x01, 0x02, 0x03, 0x04
    };

    uint8_t output[8];

    size_t result = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        output,
        sizeof(output)
    );

    TEST_ASSERT_EQUAL_UINT16(0, result);
}


/*
 * Encoding must reject a payload larger than BL_MAX_PAYLOAD.
 */
void test_encode_rejects_oversized_payload(void)
{
    uint8_t payload[BL_MAX_PAYLOAD + 1];
    uint8_t output[BL_MAX_PAYLOAD + 9];

    memset(payload, 0xA5, sizeof(payload));

    size_t result = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        output,
        sizeof(output)
    );

    TEST_ASSERT_EQUAL_UINT16(0, result);
}


/*
 * A NULL payload is valid only when the payload length is zero.
 */
void test_encode_rejects_null_payload_with_nonzero_length(void)
{
    uint8_t output[TEST_BUFFER_SIZE];

    size_t result = bl_frame_encode(
        BL_CMD_WRITE,
        NULL,
        1,
        output,
        sizeof(output)
    );

    TEST_ASSERT_EQUAL_UINT16(0, result);
}


/*
 * The encoded length field must use little-endian byte order.
 */
void test_encode_length_is_little_endian(void)
{
    uint8_t payload[0x123];
    uint8_t output[0x123 + 8];

    memset(payload, 0xA5, sizeof(payload));

    size_t result = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        output,
        sizeof(output)
    );

    TEST_ASSERT_EQUAL_UINT16(0x12B, result);

    TEST_ASSERT_EQUAL_HEX8(0x23, output[2]);
    TEST_ASSERT_EQUAL_HEX8(0x01, output[3]);
}


/*
 * The parser must recover after a CRC error and accept the next frame.
 */
void test_parser_recovers_after_crc_error(void)
{
    const uint8_t bad_payload[] = {
        0x10, 0x20, 0x30
    };

    const uint8_t good_payload[] = {
        0xAA, 0xBB
    };

    uint8_t bad_frame[TEST_BUFFER_SIZE];
    uint8_t good_frame[TEST_BUFFER_SIZE];

    size_t bad_len = bl_frame_encode(
        BL_CMD_WRITE,
        bad_payload,
        sizeof(bad_payload),
        bad_frame,
        sizeof(bad_frame)
    );

    size_t good_len = bl_frame_encode(
        BL_CMD_HELLO,
        good_payload,
        sizeof(good_payload),
        good_frame,
        sizeof(good_frame)
    );

    TEST_ASSERT_NOT_EQUAL(0, bad_len);
    TEST_ASSERT_NOT_EQUAL(0, good_len);

    /* Force CRC failure. */
    bad_frame[4] ^= 0xFF;

    TEST_ASSERT_EQUAL(
        BL_FRAME_ERROR,
        feed_frame(bad_frame, bad_len)
    );

    /* Parser was reset by the error. */
    TEST_ASSERT_EQUAL(
        BL_FRAME_COMPLETE,
        feed_frame(good_frame, good_len)
    );

    TEST_ASSERT_EQUAL(BL_CMD_HELLO, parser.frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(sizeof(good_payload), parser.frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        good_payload,
        parser.frame.payload,
        sizeof(good_payload)
    );
}


/*
 * A parser reset must discard a partially received frame.
 */
void test_parser_reset_discards_partial_frame(void)
{
    uint8_t frame[TEST_BUFFER_SIZE];

    const uint8_t payload[] = {
        0x01, 0x02, 0x03, 0x04
    };

    size_t frame_len = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        frame,
        sizeof(frame)
    );

    TEST_ASSERT_NOT_EQUAL(0, frame_len);

    /* Feed only the first half of the frame. */
    for(size_t i = 0; i < frame_len / 2; i++){
        TEST_ASSERT_EQUAL(
            BL_FRAME_INCOMPLETE,
            bl_frame_feed(&parser, frame[i])
        );
    }

    /* Simulate timeout/reset from bl_core. */
    bl_frame_parser_init(&parser);

    TEST_ASSERT_EQUAL(
        BL_FRAME_WAIT_SOF,
        parser.state
    );

    /* The complete frame must still parse correctly. */
    TEST_ASSERT_EQUAL(
        BL_FRAME_COMPLETE,
        feed_frame(frame, frame_len)
    );

    TEST_ASSERT_EQUAL(BL_CMD_WRITE, parser.frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), parser.frame.len);
}


/*
 * A maximum-sized payload must be accepted.
 */
void test_maximum_payload_size(void)
{
    uint8_t payload[BL_MAX_PAYLOAD];
    uint8_t frame[BL_MAX_PAYLOAD + 8];

    for(size_t i = 0; i < sizeof(payload); i++){
        payload[i] = (uint8_t)i;
    }

    size_t frame_len = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        frame,
        sizeof(frame)
    );

    TEST_ASSERT_EQUAL_UINT16(BL_MAX_PAYLOAD + 8, frame_len);
    TEST_ASSERT_EQUAL(
        BL_FRAME_COMPLETE,
        feed_frame(frame, frame_len)
    );
    TEST_ASSERT_EQUAL_UINT16(BL_MAX_PAYLOAD, parser.frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload,
        parser.frame.payload,
        BL_MAX_PAYLOAD
    );
}

/*
 * The parser must accept several frames in a row without being
 * reinitialized between them.
 *
 * Regression test: the CRC state was previously not reset on
 * BL_FRAME_COMPLETE, which left byte_count past 4 and wedged the
 * parser after the first successful frame.
 */
void test_consecutive_frames_without_reinit(void)
{
    const uint8_t payload[] = {
        0x10, 0x20, 0x30
    };

    uint8_t frame[TEST_BUFFER_SIZE];

    size_t frame_len = bl_frame_encode(
        BL_CMD_WRITE,
        payload,
        sizeof(payload),
        frame,
        sizeof(frame)
    );

    TEST_ASSERT_NOT_EQUAL(0, frame_len);

    /* First frame */
    TEST_ASSERT_EQUAL(
        BL_FRAME_COMPLETE,
        feed_frame(frame, frame_len)
    );

    TEST_ASSERT_EQUAL(BL_CMD_WRITE, parser.frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), parser.frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload,
        parser.frame.payload,
        sizeof(payload)
    );

    /* Second frame without reinitializing the parser */
    TEST_ASSERT_EQUAL(
        BL_FRAME_COMPLETE,
        feed_frame(frame, frame_len)
    );
    TEST_ASSERT_EQUAL(BL_CMD_WRITE, parser.frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), parser.frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload,
        parser.frame.payload,
        sizeof(payload)
    );

    /* Third frame without reinitializing the parser */
    TEST_ASSERT_EQUAL(
        BL_FRAME_COMPLETE,
        feed_frame(frame, frame_len)
    );
    TEST_ASSERT_EQUAL(BL_CMD_WRITE, parser.frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), parser.frame.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        payload,
        parser.frame.payload,
        sizeof(payload)
    );
}


int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_encode_then_decode);
    RUN_TEST(test_garbage_before_sof_is_ignored);
    RUN_TEST(test_corrupted_payload_is_rejected);
    RUN_TEST(test_oversized_length_is_rejected);
    RUN_TEST(test_frame_can_be_split_across_feed_calls);
    RUN_TEST(test_zero_length_payload);
    RUN_TEST(test_sof_inside_payload_does_not_break_frame);
    RUN_TEST(test_corrupted_crc_is_rejected);
    RUN_TEST(test_encode_rejects_small_output_buffer);
    RUN_TEST(test_encode_rejects_oversized_payload);
    RUN_TEST(test_encode_rejects_null_payload_with_nonzero_length);
    RUN_TEST(test_encode_length_is_little_endian);
    RUN_TEST(test_parser_recovers_after_crc_error);
    RUN_TEST(test_parser_reset_discards_partial_frame);
    RUN_TEST(test_maximum_payload_size);
    RUN_TEST(test_consecutive_frames_without_reinit);

    return UNITY_END();
}
