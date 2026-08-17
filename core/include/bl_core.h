#ifndef BL_CORE_H
#define BL_CORE_H

#include "bl_proto.h"

/**
 * @brief Validate a firmware image slot.
 * Validates the selected slot's image header and firmware payload.
 *
 * Guarantees:
 * - On BL_OK, *hdr_out contains the fully validated image header.
 * - hdr_out is written only when this function returns BL_OK.
 * - On failure, hdr_out is left unmodified.
 *
 * @param slot     Slot to validate. Must be BL_SLOT_A or BL_SLOT_B.
 * @param hdr_out  Destination for the validated image header.
 *
 * @return BL_OK                   The slot contains a valid image.
 * @return BL_ERR_NULL_POINTER     hdr_out is NULL.
 * @return BL_ERR_INVALID_ARGUMENT slot is not a valid firmware slot.
 * @return BL_ERR_MAGIC            The image magic is invalid.
 * @return BL_ERR_HDR_VERSION      The image header version is unsupported.
 * @return BL_ERR_HDR_CRC          The image header CRC is invalid.
 * @return BL_ERR_IMAGE_SIZE       The image size is invalid for the slot.
 * @return BL_ERR_IMAGE_CRC        The firmware payload CRC is invalid.
 * @return BL_ERR_ENTRY_OFFSET     The payload entry offset is invalid.
 */
bl_result_t bl_core_validate_slot(uint8_t slot, bl_img_hdr_t *hdr_out);

/**
 * @brief Select the firmware image slot that should run.
 *
 * Evaluates the available firmware slots and selects the valid image that
 * satisfies the boot-selection policy.
 *
 * Guarantees:
 * - On BL_OK, *slot_out identifies the selected slot and *hdr_out contains
 *   that slot's fully validated image header.
 * - slot_out and hdr_out are written only when this function returns BL_OK.
 * - On failure, both output objects are left unmodified.
 *
 * @param slot_out Destination for the selected slot.
 * @param hdr_out  Destination for the selected image header.
 *
 * @return BL_OK               A bootable image was selected.
 * @return BL_ERR_NULL_POINTER slot_out or hdr_out is NULL.
 * @return BL_ERR_MAGIC        No selectable image passed magic validation.
 * @return BL_ERR_HDR_VERSION  No selectable image passed header-version validation.
 * @return BL_ERR_HDR_CRC      No selectable image passed header CRC validation.
 * @return BL_ERR_IMAGE_SIZE   No selectable image passed image-size validation.
 * @return BL_ERR_IMAGE_CRC    No selectable image passed payload CRC validation.
 * @return BL_ERR_ENTRY_OFFSET No selectable image passed entry-offset validation.
 * @return BL_ERR_NO_VALID_IMAGE No slot holds a bootable image.
 */
bl_result_t bl_core_select_slot(uint8_t *slot_out, bl_img_hdr_t *hdr_out);

/**
 * @brief Select and boot the firmware image that should run.
 *
 * Selects a bootable image and transfers control to it.
 *
 * Guarantees:
 * - A successful boot never returns.
 * - This function returns only on failure.
 *
 * @return BL_ERR_MAGIC        Image selection failed magic validation.
 * @return BL_ERR_HDR_VERSION  Image selection failed header-version validation.
 * @return BL_ERR_HDR_CRC      Image selection failed header CRC validation.
 * @return BL_ERR_IMAGE_SIZE   Image selection failed image-size validation.
 * @return BL_ERR_IMAGE_CRC    Image selection failed payload CRC validation.
 * @return BL_ERR_ENTRY_OFFSET Image selection failed entry-offset validation.
 * @return BL_ERR_NO_VALID_IMAGE No slot holds a bootable image.
 * @return BL_ERR_JUMP_REFUSED The selected image passed validation but the
 *                             port refused to transfer control to it.
 */
bl_result_t bl_core_boot(void);

#endif /* BL_CORE_H */
