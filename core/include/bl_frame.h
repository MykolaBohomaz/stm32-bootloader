#ifndef BL_FRAME_H
#define BL_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include "bl_proto.h"

/*
 * Result returned by the incremental frame parser.
 */
typedef enum {
    BL_FRAME_INCOMPLETE,   /* need more bytes */
    BL_FRAME_COMPLETE,     /* parser->frame is valid */
    BL_FRAME_ERROR         /* bad length or CRC; parser reset */
} bl_frame_status_t;

/*
 * Parser states:
 */
typedef enum {
    BL_FRAME_WAIT_SOF,
    BL_FRAME_CMD,
    BL_FRAME_LEN,
    BL_FRAME_PAYLOAD,
    BL_FRAME_CRC
} bl_frame_state_t;

/*
 * Decoded frame.
 *
 * len is the payload length in bytes.
 * payload contains up to BL_MAX_PAYLOAD bytes.
 */
typedef struct{
    uint8_t cmd;
    uint16_t len;
    uint8_t payload[BL_MAX_PAYLOAD];
} bl_frame_t;

/*
 * Incremental frame parser state.
 *
 * The parser contains all state required to resume parsing when the
 * next byte arrives. No dynamic allocation or transport/I/O state
 * is required.
 */
typedef struct{
    uint16_t byte_count;
    uint32_t running_crc;
    uint32_t received_crc;
    bl_frame_state_t state;
    bl_frame_t frame;
} bl_frame_parser_t ;

/*
 * Initialize or reset a frame parser.
 *
 * Must be called before the parser is first used and may be called
 * to discard a partially received frame.
 */
void bl_frame_parser_init(bl_frame_parser_t *p);

/*
 * Feed one received byte into the parser.
 */
bl_frame_status_t bl_frame_feed(bl_frame_parser_t *p, uint8_t byte);

/*
 * Encode a command and payload into a complete frame.
 *
 * Frame format:
 *
 *   SOF | CMD | LEN[0] | LEN[1] | PAYLOAD | CRC[0..3]
 *
 * LEN and CRC are little-endian. The CRC covers CMD, LEN and PAYLOAD.
 *
 * Returns the number of bytes written, or 0 if the output buffer is
 * too small or the arguments are invalid.
 */
size_t bl_frame_encode(uint8_t cmd, const void *payload, uint16_t len,
                       uint8_t *out, size_t out_size);

#endif // !BL_FRAME_H
