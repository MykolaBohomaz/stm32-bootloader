#ifndef BL_PORT_HOST_H
#define BL_PORT_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Test-only control interface for the host bootloader port.
 *
 * These hooks allow unit tests to provide transport input, inspect output,
 * control simulated time, observe reset/jump requests, and inject flash
 * failures. Production bootloader code should use bl_port.h only.
 */

/**
 * @brief Replace the simulated RX stream.
 *
 * @param data Bytes returned by subsequent bl_transport_read_byte() calls.
 *             May be NULL when len is zero.
 * @param len Number of bytes to load.
 */
void bl_host_rx_load(const uint8_t *data, size_t len);

/**
 * @brief Copy bytes currently captured in the simulated TX stream.
 *
 * Reading does not consume or clear the TX buffer.
 *
 * @param dst Destination buffer. May be NULL when max_len is zero.
 * @param max_len Maximum number of bytes to copy.
 *
 * @return Number of bytes copied.
 */
size_t bl_host_tx_read(uint8_t *dst, size_t max_len);

/**
 * @brief Return the number of bytes currently captured in the TX stream.
 */
size_t bl_host_tx_size(void);

/**
 * @brief Clear the simulated TX stream.
 */
void bl_host_tx_clear(void);

/**
 * @brief Set the simulated monotonic time.
 *
 * @param time_ms Time value returned by bl_time_ms().
 */
void bl_host_time_set(uint32_t time_ms);

/**
 * @brief Advance the simulated monotonic time.
 *
 * Unsigned overflow intentionally matches the bl_time_ms() contract.
 *
 * @param delta_ms Amount to advance the clock.
 */
void bl_host_time_advance(uint32_t delta_ms);

/**
 * @brief Return whether bl_jump_to_app() accepted a jump request.
 */
bool bl_host_did_jump(void);

/**
 * @brief Return the last accepted application jump target.
 */
uint32_t bl_host_jump_target(void);

/**
 * @brief Return whether bl_reset() was requested.
 */
bool bl_host_did_reset(void);

/**
 * @brief Inject flash-program failures after N successful writes.
 *
 * n == 0 causes the first write to fail.
 * n == 1 permits one successful write, then fails subsequent writes.
 *
 * Calling this function also resets the injected-write counter.
 *
 * @param n Number of successful writes allowed before failure begins.
 */
void bl_host_fail_after_n_writes(uint32_t n);

/**
 * @brief Disable injected flash failures and reset the write counter.
 */
void bl_host_clear_failures(void);

#endif /* BL_PORT_HOST_H */
