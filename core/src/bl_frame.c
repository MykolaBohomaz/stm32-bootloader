#include "bl_frame.h"
#include "bl_crc32.h"
#include "bl_proto.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Reset the parser to its initial state.
 *
 * The parser is intentionally stateless with respect to I/O:
 * the caller feeds one byte at a time through bl_frame_feed().
 */
void bl_frame_parser_init(bl_frame_parser_t *p){
  p->state = BL_FRAME_WAIT_SOF;
  p->byte_count = 0;
  p->received_crc = 0;
  p->running_crc = 0;
  p->frame.len = 0;
}

/*
 * Feed one byte into the frame parser.
 *
 * The parser is a state machine because a complete frame may arrive
 * over multiple reads from the transport. No I/O or blocking is
 * performed here.
 *
 * CRC is accumulated incrementally over CMD, LEN and PAYLOAD.
 * SOF and the transmitted CRC are not included in the CRC calculation.
 */
bl_frame_status_t bl_frame_feed(bl_frame_parser_t *p, uint8_t byte){
  switch (p->state) {

      /*
       * Ignore all bytes until the start-of-frame marker is received.
       * SOF itself is not included in the frame CRC.
       */
      case BL_FRAME_WAIT_SOF:

        if(byte == BL_SOF){
          p->state = BL_FRAME_CMD;
          p->byte_count = 0;   
          p->frame.len = 0;
          p->running_crc = 0;
          p->received_crc = 0;
        }

        break;

      /*
       * CMD is a single byte and is included in the CRC.
       */
      case BL_FRAME_CMD:
      
        p->frame.cmd = byte;
        p->running_crc = bl_crc32(p->running_crc, &byte, 1); 
        p->state = BL_FRAME_LEN;
          
        break;

      /*
       * LEN is a 16-bit little-endian field.
       *
       * byte_count distinguishes the low and high bytes.
       * The length is validated immediately after both bytes have
       * been received, before any payload is written.
       */
      case BL_FRAME_LEN:

        p->running_crc = bl_crc32(p->running_crc, &byte, 1);

        if(p->byte_count == 0){
          p->frame.len = byte;
          p->byte_count++;
        }

        else{
          p->frame.len = (uint16_t)(p->frame.len | ((uint16_t)byte << 8));

          /*
           * Length originates from the external transport, so it must
           * be checked before using it as a payload buffer bound.
           */
          if(p->frame.len > BL_MAX_PAYLOAD){
            bl_frame_parser_init(p);
            return BL_FRAME_ERROR; 
          }

          p->byte_count = 0;

          /*
           * A zero-length frame has no payload bytes, so skip directly
           * to CRC.
           */
          if(p->frame.len == 0){
            p->state = BL_FRAME_CRC;
          }
          else{
            p->state = BL_FRAME_PAYLOAD;
          }
        }
          
        break;
        
      /*
       * Store payload bytes while incrementally updating the CRC.
       *
       * byte_count is the index of the next payload byte.
       * The length check performed in BL_FRAME_LEN guarantees that
       * writes remain within the fixed-size payload buffer.
       */
      case BL_FRAME_PAYLOAD:

        p->running_crc = bl_crc32(p->running_crc, &byte, 1);
        p->frame.payload[p->byte_count] = byte;
        p->byte_count++;

        if(p->byte_count == p->frame.len){
          p->state = BL_FRAME_CRC; 
          p->byte_count = 0;
        }
         
        break;

      /*
       * CRC is transmitted as four little-endian bytes.
       *
       * These bytes are compared against the incrementally calculated
       * CRC and are intentionally NOT included in running_crc.
       */
      case BL_FRAME_CRC:
        p->received_crc |= ((uint32_t)byte << (8 * p->byte_count));
        p->byte_count++;
        
        if(p->byte_count == 4){
          if(p->running_crc == p->received_crc){
            p->state = BL_FRAME_WAIT_SOF;
            p->byte_count = 0;
            return BL_FRAME_COMPLETE;
          } 
          else{
            bl_frame_parser_init(p);
            return BL_FRAME_ERROR; 
          }
        }
          
        break;
  }
  return BL_FRAME_INCOMPLETE;
}

/*
 * Encode a command and payload into a complete frame.
 *
 * Frame format:
 *
 *   SOF | CMD | LEN[0] | LEN[1] | PAYLOAD | CRC[0..3]
 *
 * All multi-byte fields are little-endian.
 *
 * CRC covers CMD, both LEN bytes, and PAYLOAD.
 * SOF and CRC itself are excluded.
 *
 * Returns the number of bytes written, or 0 if the output buffer
 * is too small or the arguments are invalid.
 */
size_t bl_frame_encode(uint8_t cmd, const void *payload, uint16_t len,
                       uint8_t *out, size_t out_size){
    if(len > BL_MAX_PAYLOAD){
    return 0;
  }

  if ((size_t)(len + 8) > out_size){
    return 0;
  }

    if((payload == NULL) && (len != 0)){
    return 0;
  }

  /* Encode frame header. */
  out[0] = BL_SOF;
  out[1] = cmd;

  /* 16-bit payload length, encoded little-endian. */
  out[2] = (uint8_t)(len & 0xFFu);
  out[3] = (uint8_t)((len >> 8) & 0xFFu);

  if(len > 0){
    memcpy(&out[4], payload, len);
  }

  uint32_t crc = 0; 
  crc = bl_crc32(crc, out + 1, len + 3);

  /* Store CRC as a 32-bit little-endian value. */
  out[4 + len] = (uint8_t)(crc & 0xFFu);
  out[5 + len] = (uint8_t)((crc >> 8) & 0xFFu);
  out[6 + len] = (uint8_t)((crc >> 16) & 0xFFu);
  out[7 + len] = (uint8_t)((crc >> 24) & 0xFFu);
  return 8 + len;
}
