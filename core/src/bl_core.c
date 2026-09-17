#include "bl_core.h"
#include "bl_crc32.h"
#include "bl_frame.h"
#include "bl_meta.h"
#include "bl_port.h"
#include "bl_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Buffer used to checksum a payload without holding it in memory. */
#define BL_CRC_CHUNK 256u

/* Largest response data field produced by any command (HELLO). */
#define BL_MAX_RESPONSE_DATA 20u


/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}


static uint32_t get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}


static bool slot_is_valid(uint8_t slot)
{
    return slot < BL_SLOT_COUNT;
}


/*
 * Encode a response frame carrying a status byte and optional data.
 *
 * Every command path returns through this function, which is what
 * guarantees that a BL_OK return is always accompanied by a complete
 * response frame and a correct *response_len.
 */
static bl_result_t respond(uint8_t cmd,
                           bl_result_t status,
                           const uint8_t *data,
                           uint16_t data_len,
                           uint8_t *response,
                           size_t response_size,
                           size_t *response_len)
{
    uint8_t payload[1u + BL_MAX_RESPONSE_DATA];

    if (data_len > BL_MAX_RESPONSE_DATA) {
        return BL_ERR_UNKNOWN;
    }

    payload[0] = (uint8_t)status;

    if (data_len > 0u) {
        memcpy(&payload[1], data, data_len);
    }

    const size_t len = bl_frame_encode(cmd,
                                       payload,
                                       (uint16_t)(data_len + 1u),
                                       response,
                                       response_size);

    if (len == 0u) {
        return BL_ERR_UNKNOWN;
    }

    *response_len = len;

    return BL_OK;
}


/* ------------------------------------------------------------------ */
/* Image validation                                                    */
/* ------------------------------------------------------------------ */

bl_result_t bl_core_validate_slot(uint8_t slot, bl_img_hdr_t *hdr_out)
{
    if (hdr_out == NULL) {
        return BL_ERR_NULL_POINTER;
    }

    if (!slot_is_valid(slot)) {
        return BL_ERR_INVALID_ARGUMENT;
    }

    const bl_layout_t *layout = bl_port_layout();
    const uint32_t slot_base = layout->slot[slot].base;
    const uint32_t slot_size = layout->slot[slot].size;

    bl_img_hdr_t hdr;

    bl_result_t result = bl_flash_read(slot_base, &hdr, (uint32_t)sizeof(hdr));
    if (result != BL_OK) {
        return result;
    }

    if (hdr.magic != BL_IMG_MAGIC) {
        return BL_ERR_MAGIC;
    }

    if (hdr.hdr_version != BL_IMG_HDR_VERSION) {
        return BL_ERR_HDR_VERSION;
    }

    /*
     * Nothing below this point may use a header field before the header
     * CRC has been verified: a corrupt length would otherwise be used to
     * read past the end of the slot.
     */
    if (hdr.hdr_crc32 !=
        bl_crc32(0u, &hdr, offsetof(bl_img_hdr_t, hdr_crc32))) {
        return BL_ERR_HDR_CRC;
    }

    if (hdr.entry_offset < sizeof(bl_img_hdr_t)) {
        return BL_ERR_ENTRY_OFFSET;
    }

    /*
     * The payload begins with the application's vector table, so the
     * offset must preserve the alignment the slot base already provides.
     */
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

    uint8_t buffer[BL_CRC_CHUNK];
    uint32_t payload_crc = 0u;
    uint32_t remaining = hdr.img_size;
    uint32_t address = slot_base + hdr.entry_offset;

    while (remaining > 0u) {
        uint32_t chunk = remaining;

        if (chunk > sizeof(buffer)) {
            chunk = (uint32_t)sizeof(buffer);
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


/* ------------------------------------------------------------------ */
/* Boot selection                                                      */
/* ------------------------------------------------------------------ */

/*
 * Select a slot without consulting boot metadata.
 *
 * Used when no metadata record exists, which is the state of a device
 * whose firmware was programmed directly rather than through an update.
 */
static bl_result_t select_by_version(uint8_t *slot_out, bl_img_hdr_t *hdr_out)
{
    bl_img_hdr_t hdr_a;
    bl_img_hdr_t hdr_b;

    const bool a_ok = bl_core_validate_slot(BL_SLOT_A, &hdr_a) == BL_OK;
    const bool b_ok = bl_core_validate_slot(BL_SLOT_B, &hdr_b) == BL_OK;

    if (a_ok && b_ok) {
        /* Ties resolve towards slot A so selection cannot oscillate. */
        if (hdr_b.fw_version > hdr_a.fw_version) {
            *slot_out = BL_SLOT_B;
            *hdr_out = hdr_b;
        } else {
            *slot_out = BL_SLOT_A;
            *hdr_out = hdr_a;
        }

        return BL_OK;
    }

    if (a_ok) {
        *slot_out = BL_SLOT_A;
        *hdr_out = hdr_a;
        return BL_OK;
    }

    if (b_ok) {
        *slot_out = BL_SLOT_B;
        *hdr_out = hdr_b;
        return BL_OK;
    }

    return BL_ERR_NO_VALID_IMAGE;
}


bl_result_t bl_core_select_slot(uint8_t *slot_out, bl_img_hdr_t *hdr_out)
{
    if (slot_out == NULL || hdr_out == NULL) {
        return BL_ERR_NULL_POINTER;
    }

    bl_meta_t meta;

    if (bl_meta_read(&meta) != BL_OK) {
        return select_by_version(slot_out, hdr_out);
    }

    bl_img_hdr_t hdr;

    /* The metadata nominates a slot; use it while it still validates. */
    if (bl_core_validate_slot(meta.active_slot, &hdr) == BL_OK) {
        *slot_out = meta.active_slot;
        *hdr_out = hdr;
        return BL_OK;
    }

    /*
     * The nominated slot is unusable. Falling back to the other slot is
     * what keeps a device with one damaged image bootable.
     */
    const uint8_t other = (uint8_t)((meta.active_slot + 1u) % BL_SLOT_COUNT);

    if (bl_core_validate_slot(other, &hdr) == BL_OK) {
        *slot_out = other;
        *hdr_out = hdr;
        return BL_OK;
    }

    return BL_ERR_NO_VALID_IMAGE;
}


/*
 * Advance trial bookkeeping before control is transferred.
 *
 * The attempt counter is incremented before the jump so that an image
 * which hangs, faults, or resets without confirming still consumes an
 * attempt. Incrementing afterwards would let such an image retry
 * indefinitely.
 */
static void advance_trial_state(void)
{
    bl_meta_t meta;

    if (bl_meta_read(&meta) != BL_OK) {
        return;
    }

    if (meta.state != (uint8_t)BL_BOOT_TRIAL) {
        return;
    }

    /*
     * A failed metadata commit is deliberately not propagated. Boot
     * continues either way: the counter simply does not advance, so a
     * failing image is granted further attempts rather than the device
     * refusing to start. Flash that cannot be written is a fault the
     * bootloader cannot repair, and declining to boot would remove the
     * operator's ability to recover over the update interface.
     */
    if (meta.boot_attempts < BL_MAX_BOOT_ATTEMPTS) {
        (void)bl_meta_commit(meta.active_slot,
                             (uint8_t)BL_BOOT_TRIAL,
                             (uint8_t)(meta.boot_attempts + 1u));
        return;
    }

    /*
     * The trial image has exhausted its attempts. Revert to the other
     * slot when it holds a valid image; otherwise leave the record
     * unchanged, since booting a failing image is preferable to booting
     * nothing at all.
     */
    const uint8_t other = (uint8_t)((meta.active_slot + 1u) % BL_SLOT_COUNT);
    bl_img_hdr_t hdr;

    if (bl_core_validate_slot(other, &hdr) == BL_OK) {
        (void)bl_meta_commit(other, (uint8_t)BL_BOOT_CONFIRMED, 0u);
    }
}


bl_result_t bl_core_boot(void)
{
    advance_trial_state();

    uint8_t slot = 0u;
    bl_img_hdr_t hdr;

    const bl_result_t result = bl_core_select_slot(&slot, &hdr);
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


/* ------------------------------------------------------------------ */
/* Boot metadata operations                                            */
/* ------------------------------------------------------------------ */

bl_result_t bl_core_mark_pending(uint8_t slot)
{
    if (!slot_is_valid(slot)) {
        return BL_ERR_INVALID_ARGUMENT;
    }

    bl_img_hdr_t hdr;

    /*
     * Refuse to nominate a slot that does not hold a valid image, so an
     * incomplete or failed update cannot arm a trial boot.
     */
    const bl_result_t result = bl_core_validate_slot(slot, &hdr);
    if (result != BL_OK) {
        return result;
    }

    return bl_meta_commit(slot, (uint8_t)BL_BOOT_TRIAL, 0u);
}


bl_result_t bl_core_confirm(void)
{
    bl_meta_t meta;

    const bl_result_t result = bl_meta_read(&meta);
    if (result != BL_OK) {
        return result;
    }

    if (meta.state == (uint8_t)BL_BOOT_CONFIRMED) {
        return BL_OK;
    }

    return bl_meta_commit(meta.active_slot, (uint8_t)BL_BOOT_CONFIRMED, 0u);
}


/* ------------------------------------------------------------------ */
/* Command handling                                                    */
/* ------------------------------------------------------------------ */

static bl_result_t handle_hello(uint8_t cmd,
                                uint16_t request_len,
                                uint8_t *response,
                                size_t response_size,
                                size_t *response_len)
{
    if (request_len != 0u) {
        return respond(cmd, BL_ERR_INVALID_PACKET, NULL, 0u,
                       response, response_size, response_len);
    }

    const bl_layout_t *layout = bl_port_layout();

    uint8_t data[20];

    /*
     * The protocol version is first so that a host may read it, decide
     * whether it understands the remainder, and stop cleanly if not.
     */
    data[0] = (uint8_t)BL_PROTO_VERSION;
    data[1] = (uint8_t)BL_IMG_HDR_VERSION;
    data[2] = (uint8_t)BL_SLOT_COUNT;
    put_u32_le(&data[3], layout->slot[BL_SLOT_A].size);
    put_u32_le(&data[7], bl_flash_write_granularity());
    put_u32_le(&data[11],
               bl_flash_erase_granularity(layout->slot[BL_SLOT_A].base));
    /*
     * The usable WRITE data size is reported rather than the raw frame
     * payload limit: a WRITE payload carries a four-byte offset ahead of
     * the data, so a host that sized its chunks from BL_MAX_PAYLOAD
     * would build requests the frame encoder cannot represent.
     */
    data[15] = (uint8_t)(BL_MAX_WRITE_DATA & 0xFFu);
    data[16] = (uint8_t)((BL_MAX_WRITE_DATA >> 8) & 0xFFu);

    /*
     * Current boot state.
     *
     * The host needs the active slot to choose an update target: erasing
     * the slot that would currently boot removes the only fallback, so
     * an interrupted update would leave the device with nothing to run.
     *
     * A device with no metadata reports BL_SLOT_NONE. The host must then
     * treat both slots as potentially bootable and decide using VERIFY.
     */
    bl_meta_t meta;

    if (bl_meta_read(&meta) == BL_OK) {
        data[17] = meta.active_slot;
        data[18] = meta.state;
        data[19] = meta.boot_attempts;
    } else {
        data[17] = BL_SLOT_NONE;
        data[18] = BL_BOOT_STATE_NONE;
        data[19] = 0u;
    }

    return respond(cmd, BL_OK, data, (uint16_t)sizeof(data),
                   response, response_size, response_len);
}


static bl_result_t handle_erase_slot(bl_session_t *session,
                                     uint8_t cmd,
                                     const bl_frame_t *request,
                                     uint8_t *response,
                                     size_t response_size,
                                     size_t *response_len)
{
    if (request->len != 1u) {
        return respond(cmd, BL_ERR_INVALID_PACKET, NULL, 0u,
                       response, response_size, response_len);
    }

    const uint8_t slot = request->payload[0];

    if (!slot_is_valid(slot)) {
        return respond(cmd, BL_ERR_INVALID_ARGUMENT, NULL, 0u,
                       response, response_size, response_len);
    }

    const bl_layout_t *layout = bl_port_layout();

    const bl_result_t result =
        bl_flash_erase(layout->slot[slot].base, layout->slot[slot].size);

    if (result != BL_OK) {
        return respond(cmd, result, NULL, 0u,
                       response, response_size, response_len);
    }

    session->target_slot = slot;
    session->erased = true;
    session->highest_offset = 0u;
    session->header_written = false;

    return respond(cmd, BL_OK, NULL, 0u,
                   response, response_size, response_len);
}


static bl_result_t handle_write(bl_session_t *session,
                                uint8_t cmd,
                                const bl_frame_t *request,
                                uint8_t *response,
                                size_t response_size,
                                size_t *response_len)
{
    /* Four offset bytes plus at least one data byte. */
    if (request->len < 5u) {
        return respond(cmd, BL_ERR_INVALID_PACKET, NULL, 0u,
                       response, response_size, response_len);
    }

    /*
     * Writing to a slot that has not been erased in this session would
     * program flash twice without an intervening erase, which the target
     * does not support.
     */
    if (!session->erased) {
        return respond(cmd, BL_ERR_NOT_ERASED, NULL, 0u,
                       response, response_size, response_len);
    }

    const uint32_t offset = get_u32_le(request->payload);
    const uint32_t data_len = (uint32_t)(request->len - 4u);
    const uint32_t granularity = bl_flash_write_granularity();

    if ((offset % granularity) != 0u || (data_len % granularity) != 0u) {
        return respond(cmd, BL_ERR_INVALID_ARGUMENT, NULL, 0u,
                       response, response_size, response_len);
    }

    /*
     * Reject any write that could program a location twice between
     * erases. On the target this produces conflicting ECC bits and the
     * next read of that location faults, so it cannot be left to the
     * host to avoid: a retransmission after a lost acknowledgement is
     * ordinary behaviour, not misuse.
     *
     * Payload writes must therefore ascend. Offset zero is permitted
     * once, out of order, because the image header is committed last:
     * an interrupted update then leaves a slot with no valid header
     * rather than a valid header over an incomplete payload.
     */
    if (offset == 0u) {
        if (session->header_written) {
            return respond(cmd, BL_ERR_NOT_ERASED, NULL, 0u,
                           response, response_size, response_len);
        }
    } else if (offset < session->highest_offset) {
        return respond(cmd, BL_ERR_NOT_ERASED, NULL, 0u,
                       response, response_size, response_len);
    }

    const bl_layout_t *layout = bl_port_layout();
    const uint32_t slot_base = layout->slot[session->target_slot].base;
    const uint32_t slot_size = layout->slot[session->target_slot].size;

    if (offset > slot_size || data_len > (slot_size - offset)) {
        return respond(cmd, BL_ERR_IMAGE_SIZE, NULL, 0u,
                       response, response_size, response_len);
    }

    const bl_result_t result =
        bl_flash_write(slot_base + offset, &request->payload[4], data_len);

    if (result != BL_OK) {
        return respond(cmd, result, NULL, 0u,
                       response, response_size, response_len);
    }

    if (offset == 0u) {
        session->header_written = true;
    }

    const uint32_t end = offset + data_len;

    if (end > session->highest_offset) {
        session->highest_offset = end;
    }

    uint8_t data[4];

    /* The offset is echoed so a retry after a timeout is unambiguous. */
    put_u32_le(data, offset);

    return respond(cmd, BL_OK, data, (uint16_t)sizeof(data),
                   response, response_size, response_len);
}


static bl_result_t handle_verify(uint8_t cmd,
                                 const bl_frame_t *request,
                                 uint8_t *response,
                                 size_t response_size,
                                 size_t *response_len)
{
    if (request->len != 1u) {
        return respond(cmd, BL_ERR_INVALID_PACKET, NULL, 0u,
                       response, response_size, response_len);
    }

    const uint8_t slot = request->payload[0];

    if (!slot_is_valid(slot)) {
        return respond(cmd, BL_ERR_INVALID_ARGUMENT, NULL, 0u,
                       response, response_size, response_len);
    }

    bl_img_hdr_t hdr;

    const bl_result_t result = bl_core_validate_slot(slot, &hdr);

    if (result != BL_OK) {
        /* The specific validation failure is reported to the host. */
        return respond(cmd, result, NULL, 0u,
                       response, response_size, response_len);
    }

    uint8_t data[12];

    put_u32_le(&data[0], hdr.img_size);
    put_u32_le(&data[4], hdr.img_crc32);
    put_u32_le(&data[8], hdr.fw_version);

    return respond(cmd, BL_OK, data, (uint16_t)sizeof(data),
                   response, response_size, response_len);
}


static bl_result_t handle_set_active(uint8_t cmd,
                                     const bl_frame_t *request,
                                     uint8_t *response,
                                     size_t response_size,
                                     size_t *response_len)
{
    if (request->len != 1u) {
        return respond(cmd, BL_ERR_INVALID_PACKET, NULL, 0u,
                       response, response_size, response_len);
    }

    const bl_result_t result = bl_core_mark_pending(request->payload[0]);

    return respond(cmd, result, NULL, 0u,
                   response, response_size, response_len);
}


static bl_result_t handle_reset(bl_session_t *session,
                                uint8_t cmd,
                                const bl_frame_t *request,
                                uint8_t *response,
                                size_t response_size,
                                size_t *response_len)
{
    if (request->len != 0u) {
        return respond(cmd, BL_ERR_INVALID_PACKET, NULL, 0u,
                       response, response_size, response_len);
    }

    /*
     * The reset is deferred to the caller so the acknowledgement is
     * transmitted first; resetting here would discard it.
     */
    session->reset_requested = true;

    return respond(cmd, BL_OK, NULL, 0u,
                   response, response_size, response_len);
}


bl_result_t bl_core_handle_frame(bl_session_t *session,
                                 const bl_frame_t *request,
                                 uint8_t *response,
                                 size_t response_size,
                                 size_t *response_len)
{
    if (session == NULL || request == NULL ||
        response == NULL || response_len == NULL) {
        return BL_ERR_NULL_POINTER;
    }

    switch (request->cmd) {
        case BL_CMD_HELLO:
            return handle_hello(request->cmd, request->len,
                                response, response_size, response_len);

        case BL_CMD_ERASE_SLOT:
            return handle_erase_slot(session, request->cmd, request,
                                     response, response_size, response_len);

        case BL_CMD_WRITE:
            return handle_write(session, request->cmd, request,
                                response, response_size, response_len);

        case BL_CMD_VERIFY:
            return handle_verify(request->cmd, request,
                                 response, response_size, response_len);

        case BL_CMD_SET_ACTIVE:
            return handle_set_active(request->cmd, request,
                                     response, response_size, response_len);

        case BL_CMD_RESET:
            return handle_reset(session, request->cmd, request,
                                response, response_size, response_len);

        default:
            /*
             * The command byte is echoed so the host can correlate the
             * rejection with the request it sent.
             */
            return respond(request->cmd, BL_ERR_INVALID_PACKET, NULL, 0u,
                           response, response_size, response_len);
    }
}
