#ifndef BL_META_H
#define BL_META_H

#include "bl_proto.h"

#include <stdint.h>
#include <stddef.h>

/*
 * Persistent boot metadata.
 *
 * The metadata records which slot the bootloader should start and whether
 * that image has proven itself. It is stored in two interchangeable flash
 * regions which are written alternately: a new record always goes to the
 * region that is not currently authoritative, so an interruption during
 * the erase or the write always leaves one intact record behind.
 *
 * The authoritative record is the valid one with the highest sequence
 * number. A record is valid only if its CRC matches, which means a
 * partially written record is indistinguishable from an absent one.
 *
 * All multi-byte fields are little-endian, matching the image header.
 */

/*
 * Number of boot attempts a trial image is granted before the bootloader
 * reverts to the previous image.
 *
 * The counter is incremented before control is transferred, so an image
 * that hangs or resets without confirming still consumes an attempt.
 */
#define BL_MAX_BOOT_ATTEMPTS 3u

/*
 * Boot state of the active image.
 *
 * Erased flash reads as 0xFF, which is deliberately not a valid state, so
 * an erased region cannot be mistaken for a meaningful record.
 */
typedef enum {
    BL_BOOT_CONFIRMED = 0x01, /* Image has confirmed itself healthy. */
    BL_BOOT_TRIAL     = 0x02  /* Image is on trial and may be reverted. */
} bl_boot_state_t;

typedef struct {
    uint32_t seq;           /* Monotonic record sequence number. */
    uint8_t  active_slot;   /* Slot the bootloader should start. */
    uint8_t  state;         /* bl_boot_state_t. */
    uint8_t  boot_attempts; /* Attempts consumed while in trial. */
    uint8_t  reserved[5];   /* Reserved; written as zero. */
    uint32_t crc32;         /* CRC-32 of the preceding fields. */
} bl_meta_t;

/*
 * The record is part of the on-flash format. Its size must also be a
 * multiple of the target's flash write granularity so that a record can
 * be programmed in whole units.
 */
_Static_assert(sizeof(bl_meta_t) == 16, "metadata record size changed");
_Static_assert(offsetof(bl_meta_t, crc32) == 12, "metadata layout changed");
_Static_assert((sizeof(bl_meta_t) % 8u) == 0u,
               "metadata record must be a whole number of write granules");

/**
 * @brief Read the authoritative boot metadata record.
 *
 * Returns the valid record with the highest sequence number.
 *
 * Guarantees:
 * - out is written only when this function returns BL_OK.
 *
 * @param out Destination for the authoritative record.
 *
 * @return BL_OK              A valid record was found.
 * @return BL_ERR_NULL_POINTER out is NULL.
 * @return BL_ERR_NO_METADATA  Neither region holds a valid record.
 */
bl_result_t bl_meta_read(bl_meta_t *out);

/**
 * @brief Write a new authoritative boot metadata record.
 *
 * The record is assigned the next sequence number and written to the
 * region that is not currently authoritative. The previously
 * authoritative record remains intact and readable until the new record
 * has been written successfully.
 *
 * @param active_slot   Slot the bootloader should start.
 * @param state         bl_boot_state_t value for the active image.
 * @param boot_attempts Attempts consumed while in trial.
 *
 * @return BL_OK                   The record was written and read back.
 * @return BL_ERR_INVALID_ARGUMENT active_slot or state is invalid.
 * @return BL_ERR_FLASH_ERASE      The target region could not be erased.
 * @return BL_ERR_FLASH_PROGRAM    The record could not be programmed.
 * @return BL_ERR_FLASH_VERIFY     The record did not read back correctly.
 */
bl_result_t bl_meta_commit(uint8_t active_slot,
                           uint8_t state,
                           uint8_t boot_attempts);

/**
 * @brief Erase both metadata regions.
 *
 * Leaves the device with no boot metadata, causing the bootloader to fall
 * back to selecting the highest-version valid image.
 *
 * @return BL_OK              Both regions were erased.
 * @return BL_ERR_FLASH_ERASE A region could not be erased.
 */
bl_result_t bl_meta_erase_all(void);

#endif /* BL_META_H */
