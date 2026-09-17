#include "bl_meta.h"
#include "bl_crc32.h"
#include "bl_port.h"
#include "bl_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Compute the CRC covering every field preceding crc32.
 */
static uint32_t meta_crc(const bl_meta_t *record)
{
    return bl_crc32(0u, record, offsetof(bl_meta_t, crc32));
}


static bool state_is_valid(uint8_t state)
{
    return state == (uint8_t)BL_BOOT_CONFIRMED ||
           state == (uint8_t)BL_BOOT_TRIAL;
}


/*
 * Read one metadata region and report whether it holds a valid record.
 *
 * A record is valid only if its CRC matches and its fields are within
 * range. An erased or partially written region therefore reads as absent
 * rather than as a record with implausible contents.
 */
static bool read_region(uint32_t index, bl_meta_t *out)
{
    const bl_layout_t *layout = bl_port_layout();
    bl_meta_t record;

    if (bl_flash_read(layout->meta[index].base,
                      &record,
                      (uint32_t)sizeof(record)) != BL_OK) {
        return false;
    }

    if (record.crc32 != meta_crc(&record)) {
        return false;
    }

    if (record.active_slot >= BL_SLOT_COUNT) {
        return false;
    }

    if (!state_is_valid(record.state)) {
        return false;
    }

    *out = record;

    return true;
}


/*
 * Locate the authoritative record and the region holding it.
 *
 * Sequence numbers are compared directly. The counter is 32 bits wide and
 * advances once per metadata commit, so it cannot realistically wrap
 * within the lifetime of a device.
 */
static bool find_newest(bl_meta_t *out, uint32_t *index_out)
{
    bl_meta_t candidate;
    bool found = false;

    for (uint32_t i = 0u; i < BL_META_COUNT; ++i) {
        if (!read_region(i, &candidate)) {
            continue;
        }

        if (!found || candidate.seq > out->seq) {
            *out = candidate;
            *index_out = i;
            found = true;
        }
    }

    return found;
}


bl_result_t bl_meta_read(bl_meta_t *out)
{
    if (out == NULL) {
        return BL_ERR_NULL_POINTER;
    }

    bl_meta_t newest;
    uint32_t index = 0u;

    if (!find_newest(&newest, &index)) {
        return BL_ERR_NO_METADATA;
    }

    *out = newest;

    return BL_OK;
}


bl_result_t bl_meta_commit(uint8_t active_slot,
                           uint8_t state,
                           uint8_t boot_attempts)
{
    if (active_slot >= BL_SLOT_COUNT || !state_is_valid(state)) {
        return BL_ERR_INVALID_ARGUMENT;
    }

    const bl_layout_t *layout = bl_port_layout();

    bl_meta_t newest;
    uint32_t newest_index = 0u;
    uint32_t next_seq = 1u;
    uint32_t target_index = 0u;

    if (find_newest(&newest, &newest_index)) {
        next_seq = newest.seq + 1u;

        /*
         * Write to the region that is not currently authoritative so the
         * existing record survives an interrupted erase or program.
         */
        target_index = (newest_index + 1u) % BL_META_COUNT;
    }

    bl_meta_t record;

    memset(&record, 0, sizeof(record));

    record.seq = next_seq;
    record.active_slot = active_slot;
    record.state = state;
    record.boot_attempts = boot_attempts;
    record.crc32 = meta_crc(&record);

    const uint32_t base = layout->meta[target_index].base;
    const uint32_t size = layout->meta[target_index].size;

    bl_result_t result = bl_flash_erase(base, size);
    if (result != BL_OK) {
        return result;
    }

    result = bl_flash_write(base, &record, (uint32_t)sizeof(record));
    if (result != BL_OK) {
        return result;
    }

    /*
     * Read the record back before treating the commit as durable. A
     * write that reports success but does not persist would otherwise
     * leave the bootloader acting on a record that is not in flash.
     */
    bl_meta_t verify;

    if (!read_region(target_index, &verify)) {
        return BL_ERR_FLASH_VERIFY;
    }

    if (verify.seq != record.seq ||
        verify.active_slot != record.active_slot ||
        verify.state != record.state ||
        verify.boot_attempts != record.boot_attempts) {
        return BL_ERR_FLASH_VERIFY;
    }

    return BL_OK;
}


bl_result_t bl_meta_erase_all(void)
{
    const bl_layout_t *layout = bl_port_layout();

    for (uint32_t i = 0u; i < BL_META_COUNT; ++i) {
        const bl_result_t result =
            bl_flash_erase(layout->meta[i].base, layout->meta[i].size);

        if (result != BL_OK) {
            return result;
        }
    }

    return BL_OK;
}
