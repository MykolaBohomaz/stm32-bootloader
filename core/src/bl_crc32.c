#include "bl_crc32.h"
#include <stdint.h>

/*
 * Calculate CRC-32 over a byte sequence.
 *
 * This implementation uses the reflected CRC-32 polynomial:
 *     0xEDB88320
 */

uint32_t bl_crc32(uint32_t crc, const void *data, size_t len){
  crc = ~crc;
  const uint8_t *bytes = data;

  for(size_t i = 0; i < len; i++){
    crc ^= bytes[i];

    for(int j = 0; j < 8; j++){
      if(crc & 1){
        crc = (crc >> 1) ^ 0xEDB88320u;
      }

      else{
        crc >>= 1;
      }
    }
  }
  return ~crc;
} 
