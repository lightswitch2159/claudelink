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
#include <zephyr/kernel.h>

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

enum rf69_dio1_state {
	RF69_DIO1_OK = 0,        /* line follows FifoNotEmpty */
	RF69_DIO1_NOT_WIRED,     /* internal flag toggles, GPIO does not */
	RF69_DIO1_STUCK_HIGH,
	RF69_DIO1_STUCK_LOW,
	RF69_DIO1_NO_GPIO,       /* not described in devicetree */
	RF69_DIO1_INCONCLUSIVE,  /* the radio's own flag never changed */
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

	/* Layer 6: is the DIO1 interrupt line physically wired?
	 *
	 * DIO1 is mapped to FifoNotEmpty, which we can drive deterministically:
	 * pushing a byte into the FIFO must take the line high, draining it must
	 * take it low. Each step is cross-checked against REG_IRQFLAGS2 over SPI,
	 * so a radio fault is distinguishable from a missing wire.
	 */
	enum rf69_dio1_state dio1;
	int dio1_low_level, dio1_high_level;   /* observed GPIO levels */
	bool dio1_flag_toggled;                /* internal flag did change */

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

/* RFM69 hardware FIFO depth. Legacy RF_MODULE_FIFO_SIZE. */
#define RF69_FIFO_SIZE 66

/* --- register / mode primitives --- */

int rf69_read_reg(uint8_t addr, uint8_t *value);
int rf69_write_reg(uint8_t addr, uint8_t value);
int rf69_set_mode(enum rf69_mode mode);
int rf69_config_916(void);
uint32_t rf69_get_freq(void);
int rf69_set_freq(uint32_t freq_hz);
int16_t rf69_read_rssi(bool trigger);
int rf69_dio1_get(void);

/** @brief Deterministically verify the DIO1 line via FifoNotEmpty. */
enum rf69_dio1_state rf69_dio1_check(struct rf69_selftest *r);

/* --- FIFO --- */

int rf69_fifo_write(const uint8_t *data, uint16_t len);
int rf69_fifo_write_byte(uint8_t b);
int rf69_fifo_read_byte(uint8_t *b);
bool rf69_fifo_is_empty(void);
bool rf69_fifo_is_full(void);
int rf69_fifo_clear(void);
int rf69_set_payload_len(uint8_t len);
int rf69_set_power_level(uint8_t level);
bool rf69_packet_sent(void);

/** @brief Log the registers that decide whether we actually radiate. */
void rf69_dump_regs(void);

/**
 * @brief Sample RSSI in RX to tell a deaf receiver from a quiet band.
 *
 * A receiver with no antenna reads the floor and never moves. One that is
 * listening shows thermal variation. Returns the spread in dBm.
 */
int rf69_rssi_survey(int16_t *min_dbm, int16_t *max_dbm);

/* --- DIO1 interrupt ---
 *
 * The legacy hardware never routed DIO0, so the driver busy-polled REG_IRQFLAGS2
 * over SPI. On this board DIO0 is wired, so RX can block on a semaphore instead
 * of spinning. See docs/aps-protocol-spec.md section 6.2.
 */
int rf69_dio1_irq_enable(struct k_sem *sem);
int rf69_dio1_irq_disable(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_RF69_H_ */
