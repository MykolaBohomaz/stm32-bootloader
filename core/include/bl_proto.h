//=======================================================================
// ALL MULTI-BYTE FIELDS ARE LITTLE-ENDIAN
//=======================================================================

#ifndef BL_PROTO_H
#define BL_PROTO_H

#include <stdint.h>
#include <stddef.h>

/*
 * Bootloader wire/storage protocol definitions.
 *
 * All multi-byte fields are encoded in little-endian byte order.
 * These definitions are shared by the bootloader and host-side tools,
 * so changes to field sizes or numeric values must remain backward
 * compatible or require a protocol version change.
 */

/* Protocol and frame constants. */
#define BL_PROTO_VERSION 1u
#define BL_MAX_PAYLOAD 512u
#define BL_SOF 0x7Eu

/* Image format identifier: ASCII "BLIM" in little-endian representation. */
#define BL_IMG_MAGIC 0x4D494C42u 

/* Firmware image slots. */
#define BL_SLOT_A 0
#define BL_SLOT_B 1
#define BL_SLOT_COUNT 2u
#define BL_META_COUNT 2u

/*
 * Firmware image header.
 *
 * The header is stored at the beginning of a firmware image and
 * contains the metadata required to validate and boot the image.
 *
 * hdr_crc32 covers the header fields preceding hdr_crc32.
 */
typedef struct {
    uint32_t magic;       /* Must equal BL_IMG_MAGIC ("BLIM"). */
    uint16_t hdr_version; /* Image header format version. */
    uint16_t flags;       /* Image attributes/flags. */
    uint32_t img_size;    /* Firmware image size in bytes. */
    uint32_t img_crc32;   /* CRC-32 of the firmware image. */
    uint32_t fw_version;  /* Firmware version. */
    uint32_t entry_offset;/* Entry point offset within the image. */
    uint8_t  reserved[12];/* Reserved for future format extensions. */
    uint32_t hdr_crc32;   /* CRC-32 of the header metadata. */
} bl_img_hdr_t;

/*
 * The image header is part of the on-disk/wire format, so its layout
 * must not change accidentally because of compiler padding or edits.
 */
_Static_assert(sizeof(bl_img_hdr_t) == 40, "header size changed");
_Static_assert(offsetof(bl_img_hdr_t, hdr_crc32) == 36, "header layout changed");

/*
 * Commands accepted by the bootloader.
 *
 * Command values are part of the wire protocol and must not be changed
 * without updating the host-side protocol implementation.
 */
typedef enum{
  BL_CMD_HELLO = 0x01,
  BL_CMD_ERASE_SLOT = 0x02,
  BL_CMD_WRITE = 0x03,
  BL_CMD_VERIFY = 0x04,
  BL_CMD_SET_ACTIVE = 0x05,
  BL_CMD_RESET = 0x06
} bl_cmd_t;

/*
 * Bootloader operation result codes.
 *
 * Values are grouped by subsystem to make protocol responses easier
 * to identify and extend without changing existing error codes.
 */
typedef enum{
    BL_OK = 0,

    /* Header validation */
    BL_ERR_MAGIC = 0x10,
    BL_ERR_HDR_VERSION = 0x11,
    BL_ERR_HDR_CRC = 0x12,

    /* Image validation */
    BL_ERR_IMAGE_SIZE = 0x20,
    BL_ERR_IMAGE_CRC = 0x21,
    BL_ERR_ENTRY_OFFSET = 0x22,

    /* Flash operations */
    BL_ERR_FLASH_UNLOCK = 0x30,
    BL_ERR_FLASH_ERASE = 0x31,
    BL_ERR_FLASH_PROGRAM = 0x32,
    BL_ERR_FLASH_VERIFY = 0x33,

    /* Communication */
    BL_ERR_TIMEOUT = 0x40,
    BL_ERR_INVALID_PACKET = 0x41,
    BL_ERR_CRC = 0x42,

    /* General */
    BL_ERR_NULL_POINTER = 0x50,
    BL_ERR_INVALID_ARGUMENT = 0x51,
    BL_ERR_UNKNOWN = 0x52
} bl_result_t;

#endif // !BL_PROTO_H

