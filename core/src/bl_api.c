#include "bl_api.h"
#include "bl_core.h"
#include "bl_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The table the bootloader publishes to the application.
 *
 * Placed in its own section so the linker script can map it to
 * BL_API_ADDRESS. On the host build the section attribute has no effect
 * beyond naming the section, which keeps the table testable.
 */
__attribute__((used, section(".bl_api")))
const bl_api_t bl_api_table = {
    .magic = BL_API_MAGIC,
    .version = BL_API_VERSION,
    .size = (uint16_t)sizeof(bl_api_t),
    .confirm = bl_core_confirm,
};


/*
 * Whether an address lies inside the bootloader's flash region.
 *
 * Compared as an integer because the region is an address range rather
 * than an object this code owns, so pointer arithmetic against it is
 * not meaningful. Subtracting before comparing avoids overflowing when
 * the region sits at the top of the address space.
 */
static bool address_in_region(uintptr_t value,
                              uintptr_t region_base,
                              size_t region_size)
{
    if (value == 0u) {
        return false;
    }

    if (value < region_base) {
        return false;
    }

    return (size_t)(value - region_base) < region_size;
}


const bl_api_t *bl_api_lookup(const void *address,
                              uintptr_t region_base,
                              size_t region_size)
{
    if (address == NULL || region_size == 0u) {
        return NULL;
    }

    bl_api_t candidate;

    /*
     * Copied rather than dereferenced in place: the address may hold an
     * erased or partially written table, and copying keeps the reads
     * aligned regardless.
     */
    memcpy(&candidate, address, sizeof(candidate));

    if (candidate.magic != BL_API_MAGIC) {
        return NULL;
    }

    if (candidate.version != BL_API_VERSION) {
        return NULL;
    }

    if (candidate.size != (uint16_t)sizeof(bl_api_t)) {
        return NULL;
    }

    /*
     * A pointer outside the bootloader's region cannot be the
     * bootloader's code. Calling it would fault on a device whose
     * application has not yet confirmed itself working, which is the
     * worst possible moment for an unrecoverable fault.
     */
    if (!address_in_region((uintptr_t)candidate.confirm,
                           region_base,
                           region_size)) {
        return NULL;
    }

    return (const bl_api_t *)address;
}
