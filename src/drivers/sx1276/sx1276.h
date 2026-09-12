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

void sx1276_dump_regs(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_SX1276_H_ */
