#ifndef BL_CORE_H
#define BL_CORE_H

#include "bl_frame.h"
#include "bl_proto.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Update-session state.
 *
 * Owned by the caller and zero-initialised before the first request.
 * The session exists to make writes verifiable: the target flash cannot
 * be programmed twice between erases, so the handler must know what has
 * already been written rather than trusting the host.
 */
typedef struct{
  uint8_t target_slot;      /* Slot selected by the last ERASE_SLOT. */
  bool erased;              /* Target slot has been erased this session. */
  uint32_t highest_offset;  /* End of the highest payload write so far. */
  bool header_written;      /* The image header at offset 0 was written. */
  bool reset_requested;     /* RESET was acknowledged; caller must reset. */
} bl_session_t;

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

/**
 * @brief Nominate a slot for a trial boot.
 *
 * Writes a boot metadata record selecting the slot with state
 * BL_BOOT_TRIAL and a zeroed attempt counter. The image is granted
 * BL_MAX_BOOT_ATTEMPTS boots in which to confirm itself, after which the
 * bootloader reverts to the other slot.
 *
 * The slot is validated first, so an incomplete or corrupt update cannot
 * arm a trial boot.
 *
 * @param slot Slot to nominate.
 *
 * @return BL_OK                   The slot was nominated.
 * @return BL_ERR_INVALID_ARGUMENT slot is not a valid firmware slot.
 * @return Any bl_core_validate_slot() failure for the requested slot.
 * @return Any bl_meta_commit() failure.
 */
bl_result_t bl_core_mark_pending(uint8_t slot);

/**
 * @brief Confirm the running image as healthy.
 *
 * Clears the trial state of the active image so that subsequent boots no
 * longer consume attempts and the bootloader will not revert to the
 * previous image.
 *
 * Intended to be called by the application once it has verified that it
 * is functioning; the call is idempotent.
 *
 * @return BL_OK              The active image is confirmed.
 * @return BL_ERR_NO_METADATA No boot metadata record exists.
 * @return Any bl_meta_commit() failure.
 */
bl_result_t bl_core_confirm(void);

/*
 * Smallest response buffer bl_core_handle_frame() can be called with.
 *
 * A response is a frame carrying a status byte and, for HELLO, the
 * largest command-specific data field. Supplying less is reported as a
 * failure rather than producing a truncated frame.
 */
#define BL_MIN_RESPONSE_SIZE 29u

/**
 * @brief Handle one decoded bootloader protocol request.
 *
 * Processes a complete request frame and produces a complete encoded
 * response frame in the caller-provided response buffer.
 *
 * This function performs no transport I/O. The caller is responsible for
 * receiving bytes, feeding them into the frame parser, and transmitting the
 * response bytes produced here.
 *
 * Supported Stage 4 commands:
 * - BL_CMD_HELLO
 * - BL_CMD_ERASE_SLOT
 * - BL_CMD_WRITE
 * - BL_CMD_VERIFY
 * - BL_CMD_RESET
 *
 * BL_CMD_SET_ACTIVE is not handled until persistent boot metadata is added.
 *
 * Request/response contract:
 * - The response uses the normal bootloader frame format.
 * - The response command echoes request->cmd.
 * - response payload byte 0 contains the bl_result_t status code.
 * - Additional response payload bytes contain command-specific data.
 *
 * Session rules:
 * - ERASE_SLOT selects the target slot and marks it erased for this session.
 * - WRITE is permitted only after the target slot has been erased during the
 *   current session.
 * - WRITE offsets and lengths must satisfy the target flash write granularity.
 * - WRITE must remain entirely inside the selected slot.
 * - WRITE requests are not required to arrive in ascending order; this permits
 *   writing the firmware payload first and committing the image header last.
 * - VERIFY validates the requested slot using bl_core_validate_slot().
 *
 * Output guarantees:
 * - On successful response generation, *response_len contains the exact number
 *   of encoded response bytes written to response.
 * - response_len is written only after a complete response has been encoded.
 * - The function never writes more than response_size bytes.
 * - The function performs no direct transport reads or writes.
 *
 * @param session       Update-session state owned by the caller.
 * @param request       Complete decoded request frame to process.
 * @param response      Destination buffer for the encoded response frame.
 * @param response_size Capacity of response in bytes.
 * @param response_len  Destination for the encoded response length.
 *
 * @return BL_OK
 *         The request was handled and a response frame was encoded successfully.
 *
 * @return BL_ERR_NULL_POINTER
 *         session, request, response, or response_len is NULL.
 *
 * @return BL_ERR_INVALID_PACKET
 *         The request payload length or command-specific payload format is
 *         invalid.
 *
 * @return BL_ERR_INVALID_ARGUMENT
 *         A slot identifier, write offset, write length, or other command
 *         argument is invalid.
 *
 * @return BL_ERR_FLASH_ERASE
 *         ERASE_SLOT failed while erasing flash.
 *
 * @return BL_ERR_FLASH_PROGRAM
 *         WRITE failed while programming flash.
 *
 * @return BL_ERR_FLASH_VERIFY
 *         A flash verification operation failed.
 *
 * @return BL_ERR_NO_VALID_IMAGE
 *         VERIFY found no valid image where required.
 *
 * @return BL_ERR_UNKNOWN
 *         The request could not be handled and no more specific result code
 *         describes the failure.
 *
 * Additional port-layer errors may be propagated unchanged when appropriate.
 */
bl_result_t bl_core_handle_frame(bl_session_t *session, const bl_frame_t *request, uint8_t *response, size_t response_size, size_t *response_len);

#endif /* BL_CORE_H */
