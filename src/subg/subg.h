/*
 * Sub-GHz packet path -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Zephyr port of the Minimed paths in legacy/project/app/src/app_subg.c. The
 * Omnipod (433 MHz) and Minimed WWL (868 MHz) paths are out of scope.
 *
 * FRAMING. Minimed packets are zero-terminated: TX appends a trailing 0x00 and
 * RX stops at the first 0x00. That is why the APS layer strips a caller-supplied
 * trailing zero before encoding -- the radio layer adds its own.
 */

#ifndef ORANGELINK_SUBG_H_
#define ORANGELINK_SUBG_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Legacy RX_PAYLAOD_LEN_MINIMED722 (sic). */
#define SUBG_MAX_PKT_LEN 107

enum subg_rx_status {
	SUBG_RX_OK = 0,
	SUBG_RX_TIMEOUT,
	SUBG_RX_INTERRUPTED,
};

struct subg_loopback {
	/* Stage 1: single-byte FIFO round-trip */
	bool fifo_byte;
	uint8_t fifo_byte_wrote, fifo_byte_read;

	/* Stage 2: burst write, byte-wise read back */
	bool fifo_burst;
	uint16_t burst_len, burst_matched;

	/* Stage 3: FIFO status flags track content */
	bool flags_track;

	/* Stage 4: the real transform chain, through the hardware FIFO */
	bool datapath_none;
	bool datapath_4b6b;
	bool datapath_manchester;

	/* Stage 5: the DIO1 interrupt fires on a FifoNotEmpty edge */
	bool dio1_irq_fired;

	/* Stage 6: TX completes (polled -- DIO1 carries FifoNotEmpty, not
	 * PacketSent, so it cannot signal completion)
	 */
	bool tx_packet_sent;
	uint32_t tx_wait_us;

	bool all_passed;
};

void subg_init(void);

/** @brief Transmit @p data, optionally repeated. Appends the 0x00 terminator. */
int subg_send_pkt(const uint8_t *data, uint8_t len, uint8_t repeat_cnt,
		  uint16_t repeat_interval_ms);

/**
 * @brief Listen for a packet.
 *
 * @param buf      Destination, at least SUBG_MAX_PKT_LEN bytes.
 * @param len      Bytes received, set only on SUBG_RX_OK.
 * @param timeout_ms 0 = wait indefinitely (bounded by the abort callback).
 */
enum subg_rx_status subg_get_pkt(uint8_t *buf, uint8_t *len, uint32_t timeout_ms);

/** @brief Abort an in-flight receive, mirroring the legacy Subg_SetIntFlg(). */
void subg_abort(void);

/**
 * @brief Clear a pending abort, mirroring the legacy Subg_ClrIntFlg().
 *
 * Must be called by the dispatcher immediately before running a command, NOT
 * inside the receive. Clearing it on entry to subg_get_pkt() loses any abort
 * that arrived while the preceding transmit was still in progress.
 */
void subg_clear_abort(void);

uint16_t subg_get_rx_count(void);
uint16_t subg_get_tx_count(void);
int16_t subg_get_last_rssi(void);

/**
 * @brief Run the RF-free loopback verification.
 *
 * Exercises the FIFO and the encode/decode chain through real hardware, and
 * confirms TX completion via the DIO0 interrupt. Does not prove the receiver has
 * sensitivity or that the radio interoperates with a pump -- that needs the 722.
 */
int subg_loopback_run(struct subg_loopback *out);
void subg_loopback_report(const struct subg_loopback *r);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_SUBG_H_ */
