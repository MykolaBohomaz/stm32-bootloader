/*
 * Simulated device, exposed as a shared library.
 *
 * Wraps the real bootloader core and the simulated port behind a byte
 * oriented interface so host tooling can be exercised against the
 * firmware logic rather than against a reimplementation of it.
 *
 * The protocol has two independent implementations — the device
 * firmware and tools/blflash.py — and no external specification to
 * check them against. Driving the actual core is therefore the only way
 * to confirm the two agree.
 *
 * Not part of the firmware build.
 */

#include "bl_core.h"
#include "bl_frame.h"
#include "bl_meta.h"
#include "bl_port.h"
#include "bl_port_host.h"
#include "bl_proto.h"

#include <stdint.h>
#include <string.h>

/* Room for the largest response plus framing. */
#define SIM_RESPONSE_CAPACITY (BL_MAX_PAYLOAD + 8u)

static bl_frame_parser_t g_parser;
static bl_session_t g_session;


/*
 * Bring the device to its factory state: erased flash, no metadata, no
 * update session.
 */
void sim_init(void)
{
    bl_port_init();
    bl_frame_parser_init(&g_parser);
    memset(&g_session, 0, sizeof(g_session));
}


/*
 * Simulate a power cycle: flash persists, volatile state does not.
 */
void sim_reboot(void)
{
    bl_host_reboot();
    bl_frame_parser_init(&g_parser);
    memset(&g_session, 0, sizeof(g_session));
}


/*
 * Feed request bytes to the device and collect any response.
 *
 * Bytes are fed one at a time through the same parser the firmware
 * uses, so partial and malformed input behave exactly as they would on
 * the target. Returns the number of response bytes written, which is
 * zero until a complete, valid frame has arrived.
 */
uint32_t sim_feed(const uint8_t *data,
                  uint32_t len,
                  uint8_t *response,
                  uint32_t response_capacity)
{
    if (data == NULL || response == NULL) {
        return 0u;
    }

    for (uint32_t i = 0u; i < len; ++i) {
        const bl_frame_status_t status = bl_frame_feed(&g_parser, data[i]);

        if (status != BL_FRAME_COMPLETE) {
            continue;
        }

        size_t response_len = 0u;

        const bl_result_t result = bl_core_handle_frame(&g_session,
                                                        &g_parser.frame,
                                                        response,
                                                        response_capacity,
                                                        &response_len);

        if (result != BL_OK) {
            return 0u;
        }

        return (uint32_t)response_len;
    }

    return 0u;
}


/*
 * Run the boot path. Returns the address control was transferred to, or
 * zero when nothing was bootable.
 */
uint32_t sim_boot(void)
{
    (void)bl_core_boot();

    if (!bl_host_did_jump()) {
        return 0u;
    }

    return bl_host_jump_target();
}


/*
 * Confirm the running image, as the application would once it has
 * established that it is healthy.
 */
uint32_t sim_confirm(void)
{
    return (uint32_t)bl_core_confirm();
}


/* Whether the last handled request asked the device to reset. */
uint32_t sim_reset_requested(void)
{
    return g_session.reset_requested ? 1u : 0u;
}


/*
 * Current boot metadata, for assertions that would otherwise require a
 * protocol round trip.
 */
uint32_t sim_meta(uint32_t *active_slot,
                  uint32_t *state,
                  uint32_t *boot_attempts)
{
    bl_meta_t meta;

    const bl_result_t result = bl_meta_read(&meta);

    if (result != BL_OK) {
        return (uint32_t)result;
    }

    if (active_slot != NULL) {
        *active_slot = meta.active_slot;
    }

    if (state != NULL) {
        *state = meta.state;
    }

    if (boot_attempts != NULL) {
        *boot_attempts = meta.boot_attempts;
    }

    return (uint32_t)BL_OK;
}


/* Geometry, so the driver need not duplicate the layout. */
uint32_t sim_slot_base(uint32_t slot)
{
    if (slot >= BL_SLOT_COUNT) {
        return 0u;
    }

    return bl_port_layout()->slot[slot].base;
}


uint32_t sim_slot_size(uint32_t slot)
{
    if (slot >= BL_SLOT_COUNT) {
        return 0u;
    }

    return bl_port_layout()->slot[slot].size;
}


/*
 * Corrupt stored flash, bypassing the write path.
 *
 * Models data changing underneath the bootloader, which no legitimate
 * sequence of protocol requests can produce.
 */
void sim_corrupt(uint32_t addr, uint32_t len)
{
    uint8_t buffer[256];

    while (len > 0u) {
        uint32_t chunk = len;

        if (chunk > sizeof(buffer)) {
            chunk = (uint32_t)sizeof(buffer);
        }

        if (bl_flash_read(addr, buffer, chunk) != BL_OK) {
            return;
        }

        for (uint32_t i = 0u; i < chunk; ++i) {
            buffer[i] = (uint8_t)(buffer[i] ^ 0xFFu);
        }

        bl_host_flash_poke(addr, buffer, chunk);

        addr += chunk;
        len -= chunk;
    }
}


/* Fail flash programming from the nth write onward. */
void sim_fail_after_n_writes(uint32_t n)
{
    bl_host_fail_after_n_writes(n);
}


void sim_clear_failures(void)
{
    bl_host_clear_failures();
}
