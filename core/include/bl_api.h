#ifndef BL_API_H
#define BL_API_H

#include "bl_proto.h"

#include <stdint.h>
#include <stddef.h>

/*
 * Services the bootloader publishes to the running application.
 *
 * An image installed through an update boots in trial state and must
 * confirm itself, or the bootloader reverts to the previous image. The
 * confirmation writes boot metadata, which the application cannot do
 * on its own without duplicating the metadata format and a flash
 * driver.
 *
 * Instead the bootloader places a table of function pointers at a
 * fixed address in its own flash region. The application reads the
 * table, validates it, and calls through it. The bootloader's code
 * remains resident in flash after control is transferred, so those
 * functions are still executable.
 *
 * Constraints this imposes on anything exposed here:
 *
 * - The function runs on the application's stack, with the
 *   application's clock configuration and interrupt state.
 * - The application's vector table is active, so the bootloader's
 *   interrupt handlers are not. Nothing reachable through this table
 *   may depend on an interrupt, including bl_time_ms().
 * - Erasing flash stalls the CPU for tens of milliseconds, delaying
 *   the application's interrupts for that period.
 * - The two binaries are compiled separately, so both must use the
 *   same ABI. The magic, version and size fields are what detect a
 *   mismatch instead of jumping into nothing.
 */

/* ASCII "BAPI" in little-endian representation. */
#define BL_API_MAGIC 0x49504142u

#define BL_API_VERSION 1u

/*
 * Address of the published table.
 *
 * Placed immediately after the bootloader's own vector table, on a
 * 512-byte boundary, so the address does not move as the bootloader's
 * code changes size. The bootloader's linker script maps the .bl_api
 * section here; the application hardcodes the same value.
 */
#define BL_API_ADDRESS 0x08000200u

typedef struct {
    uint32_t magic;   /* BL_API_MAGIC. */
    uint16_t version; /* BL_API_VERSION. */
    uint16_t size;    /* sizeof(bl_api_t), for forward compatibility. */

    /*
     * Confirm the running image as healthy, clearing its trial state so
     * that later boots no longer consume attempts. Idempotent.
     *
     * Returns BL_OK, BL_ERR_NO_METADATA when the device holds no boot
     * record, or a flash error.
     */
    bl_result_t (*confirm)(void);
} bl_api_t;

_Static_assert(sizeof(bl_api_t) >= 12, "API table smaller than its header");

/*
 * The table the bootloader publishes.
 *
 * Linked into the .bl_api section, which the bootloader's linker script
 * maps to BL_API_ADDRESS. The application does not reference this
 * symbol: it reads the fixed address instead, because the two binaries
 * are linked separately.
 */
extern const bl_api_t bl_api_table;

/**
 * @brief Validate a published API table and return it.
 *
 * Intended to be called by the application with BL_API_ADDRESS and the
 * bounds of the bootloader's flash region.
 *
 * Every function pointer is checked to lie inside that region. A table
 * that survived a partial erase, or was built by an incompatible
 * toolchain, would otherwise be called into — and an invalid pointer
 * here faults on a device that is by definition not yet confirmed
 * working.
 *
 * The bounds are uintptr_t rather than uint32_t so the check remains
 * meaningful when the core is compiled for the host, where truncating
 * a pointer to 32 bits would compare unrelated halves of an address.
 *
 * @param address Address of the candidate table.
 * @param region_base Base of the bootloader's flash region.
 * @param region_size Size of the bootloader's flash region.
 *
 * @return The table, or NULL when it is absent or unusable.
 */
const bl_api_t *bl_api_lookup(const void *address,
                              uintptr_t region_base,
                              size_t region_size);

#endif /* BL_API_H */
