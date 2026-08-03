#include "bl_crc32.h"
#include <stdint.h>



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
