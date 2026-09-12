/*
 * Semtech SX1276 (RFM95) driven in OOK -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Alternative radio to src/drivers/rf69. The operation set deliberately mirrors
 * rf69.h so the sub-GHz layer can be switched between them; see
 * docs/sx1276-reference.md for why this is not a straight transcription of the
 * SX1231 driver.
 */

#ifndef ORANGELINK_SX1276_H_
#define ORANGELINK_SX1276_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sx1276_mode {
	SX1276_MODE_SLEEP = 0,
	SX1276_MODE_STANDBY,
	SX1276_MODE_FSTX,
	SX1276_MODE_TX,
	SX1276_MODE_FSRX,
	SX1276_MODE_RX,
};

/** @brief What the SPI link looks like, for diagnosis. Mirrors rf69_link. */
enum sx1276_link {
	SX1276_LINK_OK = 0,
	SX1276_LINK_NO_RESPONSE,   /* 0x00 or 0xFF -- bus dead or MISO floating */
	SX1276_LINK_WRONG_DEVICE,  /* responds, but RegVersion is not 0x12 */
};

struct sx1276_selftest {
	enum sx1276_link link;
	uint8_t version;
	bool spi_rw;
	uint8_t wr_expected, wr_actual;
	bool cfg_applied;
	uint8_t cfg_count;
	bool freq_ok;
	uint32_t freq_hz;
	bool all_passed;
};

int sx1276_init(void);
int sx1276_reset(void);
int sx1276_selftest_run(struct sx1276_selftest *out);
void sx1276_selftest_report(const struct sx1276_selftest *r);

int sx1276_read_reg(uint8_t addr, uint8_t *value);
int sx1276_write_reg(uint8_t addr, uint8_t value);

int sx1276_set_mode(enum sx1276_mode mode);
int sx1276_config_916(void);

uint32_t sx1276_get_freq(void);
int sx1276_set_freq(uint32_t freq_hz);

/** @brief Last latched RSSI in dBm. Sampled at sync match, not after draining. */
int16_t sx1276_read_rssi(void);

int sx1276_fifo_write(const uint8_t *data, uint16_t len);
int sx1276_fifo_write_byte(uint8_t b);
int sx1276_fifo_read_byte(uint8_t *b);
bool sx1276_fifo_is_empty(void);
bool sx1276_fifo_is_full(void);
bool sx1276_fifo_level_exceeded(void);
int sx1276_fifo_clear(void);

int sx1276_set_payload_len(uint8_t len);
int sx1276_set_power_level(uint8_t level);
bool sx1276_packet_sent(void);
bool sx1276_sync_matched(void);

/**
 * @brief Enable the receive-trigger interrupt, giving @p sem when it fires.
 *
 * DIO2 / SyncAddressMatch, not a FIFO-level signal. The SX1276 has no
 * FifoNotEmpty mapping (DIO1 offers only FifoLevel, FifoEmpty and FifoFull), so
 * receive is keyed on the sync word matching instead of on the first byte
 * landing. That is also the correct moment to latch RSSI -- see
 * sx1276_latch_rssi().
 */
int sx1276_rx_irq_enable(struct k_sem *sem);
int sx1276_rx_irq_disable(void);

/**
 * @brief Sample RSSI now and keep it for sx1276_read_rssi().
 *
 * Must be called while the carrier is still present -- i.e. at sync match, not
 * after the FIFO has been drained. Reading it late measures the noise floor;
 * see MIGRATION_NOTES 13.3.
 */
int sx1276_latch_rssi(void);

/**
 * @brief Sample RSSI repeatedly in RX and report the spread.
 *
 * A dead front end reads a constant value; a live one wanders. Boot-time
 * liveness check, mirroring rf69_rssi_survey().
 */
int sx1276_rssi_survey(int16_t *min_dbm, int16_t *max_dbm);

void sx1276_dump_regs(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_SX1276_H_ */
