#ifndef BL_CRC32_H
#define BL_CRC32_H

#include <stdint.h>
#include <stddef.h>

uint32_t bl_crc32(uint32_t crc, const void *data, size_t len); 

#endif // !BL_CRC32_H
