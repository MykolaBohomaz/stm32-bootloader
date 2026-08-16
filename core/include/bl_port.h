#ifndef BL_PORT_H
#define BL_PORT_H

#include "bl_proto.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Describes a contiguous region of flash memory.
 *
 * Both address and size are expressed in bytes.
 */
typedef struct {
    uint32_t base;
    uint32_t size;
} bl_region_t;
/**
 * @brief Describes the bootloader flash layout.
 *
 * Contains the firmware slots and their corresponding metadata regions.
 * The layout is provided by the platform-specific port implementation.
 */
typedef struct {
    bl_region_t slot[BL_SLOT_COUNT];
    bl_region_t meta[BL_META_COUNT];
} bl_layout_t;

/**
 * Indices into bl_layout_t::meta.
 *
 * The two metadata regions are interchangeable ping-pong copies, not one
 * region per firmware slot. A metadata record is always written to the copy
 * that is not currently authoritative, so that an interruption during the
 * write always leaves one intact copy behind. Either copy may describe
 * either slot.
 */
#define BL_META_PRIMARY   0u
#define BL_META_SECONDARY 1u

/**
 * @brief Initialize platform-specific bootloader resources.
 *
 * Must be called before using flash, transport, or other port services.
 *
 * @return BL_OK on success, otherwise an appropriate error code.
 */
bl_result_t bl_port_init(void);

/**
 * @brief Return the platform-specific flash layout.
 *
 * @return Pointer to a valid flash layout.
 *
 * The returned pointer is owned by the port layer, remains valid for the
 * lifetime of the application, and is never NULL.
 */
const bl_layout_t *bl_port_layout(void);

/**
 * @brief Erase a region of flash memory.
 *
 * The flash address must be aligned to an erase boundary and len must be
 * an exact multiple of the erase granularity for the affected region.
 *
 * Flash is unlocked internally before the operation and is left locked
 * when the function returns, including on error paths.
 *
 * After a successful erase, the affected flash region reads as 0xFF.
 *
 * @param addr Start address of the region to erase.
 * @param len Number of bytes to erase.
 *
 * @return BL_OK on success, otherwise a flash-related error.
 */
bl_result_t bl_flash_erase(uint32_t addr, uint32_t len);

/**
 * @brief Write data to flash memory.
 *
 * addr and len must both be multiples of the value returned by
 * bl_flash_write_granularity().
 *
 * Writing to the same flash location more than once without an erase
 * operation in between is undefined. On some targets, conflicting ECC
 * programming can make subsequent reads fault.
 *
 * Flash is unlocked internally before the operation and is left locked
 * when the function returns, including on error paths.
 *
 * @param addr Destination flash address.
 * @param data Source data buffer.
 * @param len Number of bytes to write.
 *
 * @return BL_OK on success, otherwise a flash-related error.
 */
bl_result_t bl_flash_write(uint32_t addr, const void *data, uint32_t len);

/**
 * @brief Read data from flash memory.
 *
 * No alignment requirements apply to addr or len.
 *
 * @param addr Source flash address.
 * @param dst Destination buffer.
 * @param len Number of bytes to read.
 *
 * @return BL_OK on success, otherwise a flash-related error.
 */
bl_result_t bl_flash_read(uint32_t addr, void *dst, uint32_t len);

/**
 * @brief Return the minimum flash write granularity.
 *
 * Callers must ensure that both flash write addresses and lengths are
 * aligned to this granularity.
 *
 * @return Write granularity in bytes.
 */
uint32_t bl_flash_write_granularity(void);

/**
 * @brief Return the erase granularity for a flash address.
 *
 * Erase granularity may vary depending on the flash region or sector
 * containing addr.
 *
 * @param addr Flash address whose erase granularity is required.
 *
 * @return Erase granularity in bytes.
 */
uint32_t bl_flash_erase_granularity(uint32_t addr);

/**
 * @brief Read one byte from the bootloader transport.
 *
 * The function waits until a byte is received or timeout_ms expires.
 * On timeout, the function returns BL_ERR_TIMEOUT.
 *
 * @param out Destination for the received byte.
 * @param timeout_ms Maximum time to wait, in milliseconds.
 *
 * @return BL_OK when a byte is received, BL_ERR_TIMEOUT when the timeout
 * expires, or another transport-related error on failure.
 */
bl_result_t bl_transport_read_byte(uint8_t *out, uint32_t timeout_ms);

/**
 * @brief Write a buffer to the bootloader transport.
 *
 * @param buf Buffer containing the data to transmit.
 * @param len Number of bytes to transmit.
 *
 * @return BL_OK on success, otherwise a transport-related error.
 */
bl_result_t bl_transport_write(const uint8_t *buf, uint32_t len);

/**
 * @brief Return the current platform time in milliseconds.
 *
 * The counter wraps at 2^32 milliseconds. Callers must handle wraparound
 * using unsigned subtraction, for example:
 *
 *     if ((now - start) > timeout)
 *
 * Callers must not compare timestamps against an absolute deadline.
 *
 * @return Current system time in milliseconds.
 */
uint32_t bl_time_ms(void);

/**
 * @brief Reset the target device.
 *
 * This function does not return.
 */
void bl_reset(void);

/**
 * @brief Transfer execution to the application.
 *
 * The port implementation validates the application entry point and
 * performs the platform-specific preparation required before the jump.
 *
 * If the jump is refused, this function returns and the caller must
 * remain in the bootloader.
 *
 * @param base_addr Base address of the application image.
 */
void bl_jump_to_app(uint32_t base_addr);

#endif /* BL_PORT_H */
