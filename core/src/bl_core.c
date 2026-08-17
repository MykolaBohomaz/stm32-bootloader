#include "bl_core.h"
#include "bl_port.h"
#include "bl_proto.h"
#include "bl_crc32.h"

#include <stddef.h>
#include <stdint.h>

bl_result_t bl_core_validate_slot(uint8_t slot, bl_img_hdr_t *hdr_out){

   if (hdr_out == NULL) {
        return BL_ERR_NULL_POINTER;
    }

    if (slot != BL_SLOT_A && slot != BL_SLOT_B) {
        return BL_ERR_INVALID_ARGUMENT;
    }

    const bl_layout_t *layout = bl_port_layout();
    const uint32_t slot_base = layout->slot[slot].base;
    const uint32_t slot_size = layout->slot[slot].size;
    bl_img_hdr_t hdr;

    bl_result_t result = bl_flash_read(slot_base, &hdr, sizeof(hdr));
    if (result != BL_OK) {
        return result;
    }

    if (hdr.magic != BL_IMG_MAGIC) {
        return BL_ERR_MAGIC;
    }

    if (hdr.hdr_version != BL_IMG_HDR_VERSION) {
        return BL_ERR_HDR_VERSION;
    }

    uint32_t hdr_crc = 0u;
    hdr_crc = bl_crc32(hdr_crc, &hdr, offsetof(bl_img_hdr_t, hdr_crc32));
    if (hdr.hdr_crc32 != hdr_crc) {
        return BL_ERR_HDR_CRC;
    }

    if (hdr.entry_offset < sizeof(bl_img_hdr_t)) {
        return BL_ERR_ENTRY_OFFSET;
    }

    if ((hdr.entry_offset & (BL_IMG_HDR_REGION - 1u)) != 0u) {
        return BL_ERR_ENTRY_OFFSET;
    }

    if (hdr.entry_offset >= slot_size) {
        return BL_ERR_ENTRY_OFFSET;
    }

    if (hdr.img_size == 0u) {
        return BL_ERR_IMAGE_SIZE;
    }

    if (hdr.img_size > (slot_size - hdr.entry_offset)) {
        return BL_ERR_IMAGE_SIZE;
    }

    uint8_t buffer[256];
    uint32_t payload_crc = 0u;
    uint32_t remaining = hdr.img_size;
    uint32_t address = slot_base + hdr.entry_offset;

    while (remaining > 0u) {
        uint32_t chunk = remaining;
        if (chunk > sizeof(buffer)) {
            chunk = sizeof(buffer);
        }

        result = bl_flash_read(address, buffer, chunk);
        if (result != BL_OK) {
            return result;
        }

        payload_crc = bl_crc32(payload_crc, buffer, chunk);
        address += chunk;
        remaining -= chunk;
    }

    if (payload_crc != hdr.img_crc32) {
        return BL_ERR_IMAGE_CRC;
    }

    *hdr_out = hdr;
    return BL_OK;
}

bl_result_t bl_core_select_slot(uint8_t *slot_out, bl_img_hdr_t *hdr_out){

    if (hdr_out == NULL || slot_out == NULL) {
        return BL_ERR_NULL_POINTER;
    }

    bl_img_hdr_t hdr_a;
    bl_img_hdr_t hdr_b;

    bl_result_t result_a = bl_core_validate_slot(BL_SLOT_A, &hdr_a);
    bl_result_t result_b = bl_core_validate_slot(BL_SLOT_B, &hdr_b);

    if(result_a == BL_OK && result_b != BL_OK){
        *hdr_out = hdr_a;
        *slot_out = BL_SLOT_A;
        return BL_OK;
    }

    if(result_a != BL_OK && result_b == BL_OK){
        *hdr_out = hdr_b;
        *slot_out = BL_SLOT_B;
        return BL_OK;
    }

    if(result_a == BL_OK && result_b == BL_OK){
        if(hdr_a.fw_version < hdr_b.fw_version){
            *hdr_out = hdr_b;
            *slot_out = BL_SLOT_B;
            return BL_OK; 
        }
        else {
            *hdr_out = hdr_a;
            *slot_out = BL_SLOT_A;
            return BL_OK;
        }
    }

    return BL_ERR_NO_VALID_IMAGE;
}

bl_result_t bl_core_boot(void){

    uint8_t slot;
    bl_img_hdr_t hdr;

    bl_result_t result = bl_core_select_slot(&slot, &hdr);

    if (result != BL_OK) {
        return result;
    }

    const bl_layout_t *layout = bl_port_layout();

    const uint32_t app_base = layout->slot[slot].base + hdr.entry_offset;

    /*
     * The port validates the entry point before transferring control and
     * returns only when it refuses. A return therefore means the image
     * passed integrity validation but does not present a usable Cortex-M
     * vector table.
     */
    bl_jump_to_app(app_base);

    return BL_ERR_JUMP_REFUSED;
}
