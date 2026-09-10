/*
 * RFM69 sub-GHz transceiver driver -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Zephyr port of legacy/periph/rf69. Scope is deliberately narrower than the
 * original: this project targets 916 MHz Minimed only, so the 433 MHz (Omnipod)
 * and 868 MHz register tables and the second radio instance are not ported.
 * A single RFM69 is expected, described by the `rf69` devicetree node.
 *
 * The register map in rf69_registers.h is a byte-identical copy of the legacy
 * header and must not be edited.
 */

#ifndef ORANGELINK_RF69_H_
#define ORANGELINK_RF69_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Value REG_VERSION must return on a healthy RFM69/SX1231. */
#define RF69_EXPECTED_VERSION 0x24

/* 916 MHz table programs FRF = 0xE52312. */
#define RF69_FREQ_916_HZ 916548000U

/* Tolerance for the frequency read-back check. */
#define RF69_FREQ_TOLERANCE_HZ 200000U

enum rf69_mode {
	RF69_MODE_SLEEP = 0,
	RF69_MODE_STANDBY,
	RF69_MODE_SYNTH,
	RF69_MODE_RX,
	RF69_MODE_TX,
};

/**
 * @brief What the SPI link looks like, for diagnosis.
 *
 * The distinction matters during bring-up: a value of 0x00 or 0xFF read from
 * REG_VERSION means the bus is not working at all, and each points at a
 * different fault, whereas a plausible-but-wrong value means the bus works and
 * something else is on the other end.
 */
enum rf69_link_state {
	RF69_LINK_OK = 0,        /* REG_VERSION == 0x24 */
	RF69_LINK_MISO_LOW,      /* reads 0x00: no power, MISO grounded/unconnected */
	RF69_LINK_MISO_HIGH,     /* reads 0xFF: MISO floating, or MOSI/SCLK not arriving */
	RF69_LINK_WRONG_DEVICE,  /* bus works, but this is not an RFM69 */
	RF69_LINK_BUS_ERROR,     /* the SPI transfer itself failed */
};

struct rf69_selftest {
	/* Layer 1: is anything there at all */
	enum rf69_link_state link;
	uint8_t version;              /* raw REG_VERSION */

	/* Layer 2: can we write as well as read */
	bool write_readback;
	uint8_t wr_expected, wr_actual;

	/* Layer 3: did the 916 MHz configuration take */
	bool config_applied;
	uint32_t freq_hz;             /* read back from FRF registers */
	uint8_t frf_msb, frf_mid, frf_lsb;   /* raw, for diagnosing a mismatch */
	bool freq_ok;

	/* Layer 4: is the radio's own state machine alive.
	 * ModeReady only asserts once the crystal oscillator has started, so this
	 * distinguishes "SPI works" from "the radio is actually running".
	 */
	bool mode_ready;
	uint32_t mode_ready_us;       /* how long ModeReady took to assert */

	/* Layer 5: does the receive chain respond */
	int16_t rssi_dbm;
	bool rssi_plausible;

	/* Layer 6: is the DIO0 interrupt line wired */
	int dio0_level;               /* -1 if unreadable */
	bool dio0_readable;

	bool all_passed;
};

/**
 * @brief Bind the SPI device and DIO0 GPIO from devicetree.
 *
 * Does not touch the radio. Returns -ENODEV if the bus is not ready.
 */
int rf69_init(void);

/**
 * @brief Run the full self-test and fill @p out.
 *
 * Safe to call repeatedly. Leaves the radio in STANDBY with the 916 MHz
 * configuration applied when it succeeds. Returns 0 if every stage passed.
 */
int rf69_selftest_run(struct rf69_selftest *out);

/** @brief Log a human-readable self-test report, with fault hints on failure. */
void rf69_selftest_report(const struct rf69_selftest *r);

/* --- register / mode primitives, for the Phase 4 packet path --- */

int rf69_read_reg(uint8_t addr, uint8_t *value);
int rf69_write_reg(uint8_t addr, uint8_t value);
int rf69_set_mode(enum rf69_mode mode);
int rf69_config_916(void);
uint32_t rf69_get_freq(void);
int rf69_set_freq(uint32_t freq_hz);
int16_t rf69_read_rssi(bool trigger);
int rf69_dio0_get(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_RF69_H_ */
