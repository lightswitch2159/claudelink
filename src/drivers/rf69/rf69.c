/*
 * RFM69 sub-GHz transceiver driver -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "rf69.h"
#include "rf69_registers.h"

LOG_MODULE_REGISTER(rf69, CONFIG_ORANGELINK_LOG_LEVEL);

#define RF69_NODE DT_NODELABEL(rf69)

#if !DT_NODE_EXISTS(RF69_NODE)
#error "devicetree node `rf69` not found -- check boards/xiao_ble.overlay"
#endif

static const struct spi_dt_spec rf69_bus = SPI_DT_SPEC_GET(
	RF69_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER, 0);

static const struct gpio_dt_spec rf69_dio1 =
	GPIO_DT_SPEC_GET_OR(RF69_NODE, dio1_gpios, {0});

/*
 * FSTEP = FXOSC / 2^19 = 32 MHz / 524288 = 61.03515625 Hz.
 *
 * The legacy driver used a float for this. Integer math here: exact, and no
 * reliance on the FPU in what will become an interrupt-adjacent path.
 * Note this 32 MHz is the RFM69's own crystal -- distinct from the 24 MHz
 * RILEY_LINK_FXOSC the APS layer uses to decode CC111x-style register values.
 */
#define RF69_FXOSC_HZ 32000000ULL
#define RF69_FSTEP_SHIFT 19

static bool initialised;

/* ------------------------------------------------------------------------- *
 * Raw register access
 *
 * The legacy driver issued two separate nrf_drv_spi transfers with NSS held low
 * across both, and de-inited the bus after each operation. A single 2-byte
 * transceive is equivalent on the wire and lets Zephyr manage chip select.
 * ------------------------------------------------------------------------- */

int rf69_read_reg(uint8_t addr, uint8_t *value)
{
	uint8_t tx[2] = { addr & 0x7F, 0x00 };   /* bit7 clear = read */
	uint8_t rx[2] = { 0 };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };
	int err;

	if (value == NULL) {
		return -EINVAL;
	}

	err = spi_transceive_dt(&rf69_bus, &txs, &rxs);
	if (err) {
		return err;
	}

	*value = rx[1];
	return 0;
}

int rf69_write_reg(uint8_t addr, uint8_t value)
{
	uint8_t tx[2] = { addr | 0x80, value };   /* bit7 set = write */
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };

	return spi_write_dt(&rf69_bus, &txs);
}

/* ------------------------------------------------------------------------- *
 * Frequency
 * ------------------------------------------------------------------------- */

uint32_t rf69_get_freq(void)
{
	uint8_t msb = 0, mid = 0, lsb = 0;
	uint32_t frf;

	if (rf69_read_reg(REG_FRFMSB, &msb) || rf69_read_reg(REG_FRFMID, &mid) ||
	    rf69_read_reg(REG_FRFLSB, &lsb)) {
		return 0;
	}

	frf = ((uint32_t)msb << 16) | ((uint32_t)mid << 8) | lsb;
	return (uint32_t)(((uint64_t)frf * RF69_FXOSC_HZ) >> RF69_FSTEP_SHIFT);
}

int rf69_set_freq(uint32_t freq_hz)
{
	uint32_t frf = (uint32_t)(((uint64_t)freq_hz << RF69_FSTEP_SHIFT) / RF69_FXOSC_HZ);
	int err;

	err = rf69_write_reg(REG_FRFMSB, (uint8_t)(frf >> 16));
	if (err) {
		return err;
	}
	err = rf69_write_reg(REG_FRFMID, (uint8_t)(frf >> 8));
	if (err) {
		return err;
	}
	return rf69_write_reg(REG_FRFLSB, (uint8_t)frf);
}

/* ------------------------------------------------------------------------- *
 * Mode control
 * ------------------------------------------------------------------------- */

static int rf69_wait_mode_ready(uint32_t timeout_us, uint32_t *elapsed_us)
{
	uint32_t waited = 0;
	uint8_t flags;

	while (waited < timeout_us) {
		if (rf69_read_reg(REG_IRQFLAGS1, &flags) == 0 &&
		    (flags & RF_IRQFLAGS1_MODEREADY)) {
			if (elapsed_us) {
				*elapsed_us = waited;
			}
			return 0;
		}
		k_busy_wait(100);
		waited += 100;
	}

	if (elapsed_us) {
		*elapsed_us = waited;
	}
	return -ETIMEDOUT;
}

int rf69_set_mode(enum rf69_mode mode)
{
	uint8_t opmode, bits;
	int err;

	switch (mode) {
	case RF69_MODE_TX:      bits = RF_OPMODE_TRANSMITTER; break;
	case RF69_MODE_RX:      bits = RF_OPMODE_RECEIVER;    break;
	case RF69_MODE_SYNTH:   bits = RF_OPMODE_SYNTHESIZER; break;
	case RF69_MODE_STANDBY: bits = RF_OPMODE_STANDBY;     break;
	case RF69_MODE_SLEEP:   bits = RF_OPMODE_SLEEP;       break;
	default:                return -EINVAL;
	}

	err = rf69_read_reg(REG_OPMODE, &opmode);
	if (err) {
		return err;
	}

	/* Preserve sequencer/listen bits, replace mode -- same mask as legacy. */
	err = rf69_write_reg(REG_OPMODE, (opmode & 0xE3) | bits);
	if (err) {
		return err;
	}

	/*
	 * The legacy driver spun here forever:
	 *   while ((spi_read_reg(dev, REG_IRQFLAGS1) & RF_IRQFLAGS1_MODEREADY) == 0);
	 * An unpopulated or dead radio hung the firmware. Bounded here instead.
	 */
	return rf69_wait_mode_ready(10000, NULL);
}

/* ------------------------------------------------------------------------- *
 * 916 MHz configuration
 *
 * Transcribed from freq916CfgTbl in legacy/periph/rf69/rf69.c. OOK modulation,
 * 16384 bps, FRF = 0xE52312 (916.548 MHz), fixed-length packets, CRC off,
 * 4-byte sync word FF 00 FF 00. Values are unchanged from the legacy table.
 * ------------------------------------------------------------------------- */

static const uint8_t rf69_cfg_916[][2] = {
	{ REG_OPMODE,        RF_OPMODE_SEQUENCER_ON | RF_OPMODE_LISTEN_OFF |
			     RF_OPMODE_STANDBY },
	{ REG_DATAMODUL,     RF_DATAMODUL_DATAMODE_PACKET |
			     RF_DATAMODUL_MODULATIONTYPE_OOK |
			     RF_DATAMODUL_MODULATIONSHAPING_00 },
	{ REG_BITRATEMSB,    RF_BITRATEMSB_16384 },
	{ REG_BITRATELSB,    RF_BITRATELSB_16384 },
	{ REG_FRFMSB,        (uint8_t)RF_FRFMSB_916 },
	{ REG_FRFMID,        (uint8_t)RF_FRFMID_916 },
	{ REG_FRFLSB,        (uint8_t)RF_FRFLSB_916 },
	{ REG_RXBW,          RF_RXBW_DCCFREQ_000 | RF_RXBW_MANT_20 | RF_RXBW_EXP_0 },
	/* DEVIATION from the legacy table, enabled by new hardware: DIO1 is mapped
	 * to FifoNotEmpty (0b10) so receive can be interrupt-driven. Legacy wrote
	 * DIO0_00 only, leaving DIO1 at its 0b00 default of FifoLevel.
	 */
	{ REG_DIOMAPPING1,   RF_DIOMAPPING1_DIO0_00 | RF_DIOMAPPING1_DIO1_10 },
	{ REG_DIOMAPPING2,   RF_DIOMAPPING2_CLKOUT_OFF },
	{ REG_IRQFLAGS2,     RF_IRQFLAGS2_FIFOOVERRUN },
	{ REG_RSSITHRESH,    228 },
	{ REG_PREAMBLEMSB,   RF_PREAMBLESIZE_MSB_VALUE },
	{ REG_PREAMBLELSB,   RF_PREAMBLESIZE_LSB_VALUE },
	{ REG_SYNCCONFIG,    RF_SYNC_ON | RF_SYNC_FIFOFILL_AUTO | RF_SYNC_SIZE_4 |
			     RF_SYNC_TOL_0 },
	{ REG_SYNCVALUE1,    0xFF },
	{ REG_SYNCVALUE2,    0x00 },
	{ REG_SYNCVALUE3,    0xFF },
	{ REG_SYNCVALUE4,    0x00 },
	{ REG_PACKETCONFIG1, RF_PACKET1_FORMAT_FIXED | RF_PACKET1_DCFREE_OFF |
			     RF_PACKET1_CRC_OFF | RF_PACKET1_CRCAUTOCLEAR_OFF |
			     RF_PACKET1_ADRSFILTERING_OFF },
	{ REG_PAYLOADLENGTH, 0xFF },
	{ REG_FIFOTHRESH,    RF_FIFOTHRESH_TXSTART_FIFONOTEMPTY |
			     RF_FIFOTHRESH_VALUE },
	{ REG_PACKETCONFIG2, RF_PACKET2_RXRESTARTDELAY_NONE |
			     RF_PACKET2_AUTORXRESTART_OFF | RF_PACKET2_AES_OFF },
	{ REG_TESTDAGC,      RF_DAGC_IMPROVED_LOWBETA0 },
};

int rf69_config_916(void)
{
	int err;

	for (size_t i = 0; i < ARRAY_SIZE(rf69_cfg_916); i++) {
		err = rf69_write_reg(rf69_cfg_916[i][0], rf69_cfg_916[i][1]);
		if (err) {
			LOG_ERR("config write failed at reg 0x%02x (%d)",
				rf69_cfg_916[i][0], err);
			return err;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------- *
 * RSSI and DIO1
 * ------------------------------------------------------------------------- */

int16_t rf69_read_rssi(bool trigger)
{
	uint8_t v = 0;

	if (trigger) {
		if (rf69_write_reg(REG_RSSICONFIG, RF_RSSI_START)) {
			return 0;
		}
		/* Bounded, unlike the legacy unbounded spin. */
		for (int i = 0; i < 100; i++) {
			if (rf69_read_reg(REG_RSSICONFIG, &v) == 0 &&
			    (v & RF_RSSI_DONE)) {
				break;
			}
			k_busy_wait(50);
		}
	}

	if (rf69_read_reg(REG_RSSIVALUE, &v)) {
		return 0;
	}

	/* RSSI is reported as -value/2 dBm. */
	return -(int16_t)(v >> 1);
}

int rf69_dio1_get(void)
{
	if (rf69_dio1.port == NULL) {
		return -ENODEV;
	}
	return gpio_pin_get_dt(&rf69_dio1);
}

/* ------------------------------------------------------------------------- *
 * FIFO
 *
 * REG_FIFO (0x00) is the FIFO access register: repeated reads pop bytes,
 * repeated writes push them. A burst write sends the address byte once followed
 * by the payload, which is what the legacy Rf69_XmitBuf did.
 * ------------------------------------------------------------------------- */

int rf69_fifo_write(const uint8_t *data, uint16_t len)
{
	uint8_t addr = REG_FIFO | 0x80;
	const struct spi_buf txb[2] = {
		{ .buf = &addr, .len = 1 },
		{ .buf = (void *)data, .len = len },
	};
	const struct spi_buf_set txs = { .buffers = txb, .count = 2 };

	if (data == NULL || len == 0) {
		return -EINVAL;
	}
	return spi_write_dt(&rf69_bus, &txs);
}

int rf69_fifo_write_byte(uint8_t b)
{
	return rf69_write_reg(REG_FIFO, b);
}

int rf69_fifo_read_byte(uint8_t *b)
{
	return rf69_read_reg(REG_FIFO, b);
}

bool rf69_fifo_is_empty(void)
{
	uint8_t f = 0;

	if (rf69_read_reg(REG_IRQFLAGS2, &f)) {
		return true;   /* fail closed: treat an unreadable FIFO as empty */
	}
	return (f & RF_IRQFLAGS2_FIFONOTEMPTY) == 0;
}

bool rf69_fifo_is_full(void)
{
	uint8_t f = 0;

	if (rf69_read_reg(REG_IRQFLAGS2, &f)) {
		return true;   /* fail closed: do not push into an unknown FIFO */
	}
	return (f & RF_IRQFLAGS2_FIFOFULL) != 0;
}

bool rf69_packet_sent(void)
{
	uint8_t f = 0;

	if (rf69_read_reg(REG_IRQFLAGS2, &f)) {
		return false;
	}
	return (f & RF_IRQFLAGS2_PACKETSENT) != 0;
}

int rf69_fifo_clear(void)
{
	/* Writing FIFOOVERRUN resets the FIFO and the status flags. */
	return rf69_write_reg(REG_IRQFLAGS2, RF_IRQFLAGS2_FIFOOVERRUN);
}

int rf69_set_payload_len(uint8_t len)
{
	return rf69_write_reg(REG_PAYLOADLENGTH, len);
}

int rf69_set_power_level(uint8_t level)
{
	uint8_t pa = 0;
	int err = rf69_read_reg(REG_PALEVEL, &pa);

	if (err) {
		return err;
	}
	return rf69_write_reg(REG_PALEVEL, (pa & 0xE0) | (level & 0x1F));
}

/* ------------------------------------------------------------------------- *
 * DIO1 interrupt
 * ------------------------------------------------------------------------- */

static struct gpio_callback dio1_cb_data;
static struct k_sem *dio1_sem;

static void dio1_handler(const struct device *port, struct gpio_callback *cb,
			 gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (dio1_sem) {
		k_sem_give(dio1_sem);
	}
}

int rf69_dio1_irq_enable(struct k_sem *sem)
{
	int err;

	if (rf69_dio1.port == NULL) {
		return -ENODEV;
	}

	dio1_sem = sem;

	gpio_init_callback(&dio1_cb_data, dio1_handler, BIT(rf69_dio1.pin));
	err = gpio_add_callback(rf69_dio1.port, &dio1_cb_data);
	if (err) {
		return err;
	}

	return gpio_pin_interrupt_configure_dt(&rf69_dio1, GPIO_INT_EDGE_TO_ACTIVE);
}

int rf69_dio1_irq_disable(void)
{
	if (rf69_dio1.port == NULL) {
		return -ENODEV;
	}
	gpio_pin_interrupt_configure_dt(&rf69_dio1, GPIO_INT_DISABLE);
	gpio_remove_callback(rf69_dio1.port, &dio1_cb_data);
	dio1_sem = NULL;
	return 0;
}

/* ------------------------------------------------------------------------- *
 * DIO1 connectivity
 *
 * DIO1 is mapped to FifoNotEmpty, a signal we control completely: the FIFO is
 * empty or it is not. So the line can be driven to a known state and read back,
 * which is a real connectivity test rather than sampling whatever a floating
 * input happens to sit at.
 * ------------------------------------------------------------------------- */

enum rf69_dio1_state rf69_dio1_check(struct rf69_selftest *r)
{
	uint8_t flags;
	bool flag_empty, flag_full;
	int level_empty, level_notempty;
	uint8_t scratch;

	if (rf69_dio1.port == NULL) {
		return RF69_DIO1_NO_GPIO;
	}

	rf69_set_mode(RF69_MODE_STANDBY);

	/* Make sure DIO1 really is FifoNotEmpty before trusting the line. */
	rf69_write_reg(REG_DIOMAPPING1,
		       RF_DIOMAPPING1_DIO0_00 | RF_DIOMAPPING1_DIO1_10);

	/* State A: FIFO drained -> flag low, line should be low. */
	rf69_fifo_clear();
	while (!rf69_fifo_is_empty()) {
		if (rf69_fifo_read_byte(&scratch) != 0) {
			break;
		}
	}
	k_busy_wait(200);
	rf69_read_reg(REG_IRQFLAGS2, &flags);
	flag_empty = (flags & RF_IRQFLAGS2_FIFONOTEMPTY) != 0;
	level_empty = gpio_pin_get_dt(&rf69_dio1);

	/* State B: one byte in the FIFO -> flag high, line should be high. */
	rf69_fifo_write_byte(0x69);
	k_busy_wait(200);
	rf69_read_reg(REG_IRQFLAGS2, &flags);
	flag_full = (flags & RF_IRQFLAGS2_FIFONOTEMPTY) != 0;
	level_notempty = gpio_pin_get_dt(&rf69_dio1);

	/* Leave the FIFO as we found it. */
	rf69_fifo_clear();

	if (r) {
		r->dio1_low_level = level_empty;
		r->dio1_high_level = level_notempty;
		r->dio1_flag_toggled = (!flag_empty && flag_full);
	}

	/* If the radio's own flag never moved, the test says nothing about wiring. */
	if (flag_empty || !flag_full) {
		return RF69_DIO1_INCONCLUSIVE;
	}

	if (level_empty == 0 && level_notempty == 1) {
		return RF69_DIO1_OK;
	}
	if (level_empty == 1 && level_notempty == 1) {
		return RF69_DIO1_STUCK_HIGH;
	}
	if (level_empty == 0 && level_notempty == 0) {
		return RF69_DIO1_STUCK_LOW;
	}
	return RF69_DIO1_NOT_WIRED;
}

/* ------------------------------------------------------------------------- *
 * Init and self-test
 * ------------------------------------------------------------------------- */

int rf69_init(void)
{
	int err;

	if (!spi_is_ready_dt(&rf69_bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	if (rf69_dio1.port != NULL) {
		err = gpio_pin_configure_dt(&rf69_dio1, GPIO_INPUT);
		if (err) {
			LOG_WRN("DIO1 configure failed (%d)", err);
		}
	} else {
		LOG_WRN("DIO1 not described in devicetree");
	}

	initialised = true;
	LOG_INF("bound to %s, CS via devicetree, DIO1 %s",
		rf69_bus.bus->name,
		rf69_dio1.port ? "present" : "absent");
	return 0;
}

static enum rf69_link_state classify_version(int err, uint8_t v)
{
	if (err) {
		return RF69_LINK_BUS_ERROR;
	}
	if (v == RF69_EXPECTED_VERSION) {
		return RF69_LINK_OK;
	}
	if (v == 0x00) {
		return RF69_LINK_MISO_LOW;
	}
	if (v == 0xFF) {
		return RF69_LINK_MISO_HIGH;
	}
	return RF69_LINK_WRONG_DEVICE;
}

int rf69_selftest_run(struct rf69_selftest *out)
{
	struct rf69_selftest r = { 0 };
	uint8_t v = 0;
	int err;

	if (!initialised) {
		err = rf69_init();
		if (err) {
			r.link = RF69_LINK_BUS_ERROR;
			goto done;
		}
	}

	/* --- 1: presence. REG_VERSION is the canonical RFM69 identity check. --- */
	err = rf69_read_reg(REG_VERSION, &v);
	r.version = v;
	r.link = classify_version(err, v);
	if (r.link != RF69_LINK_OK) {
		goto done;
	}

	/* --- 2: bidirectional SPI. Write a pattern to a sync-word byte and read
	 * it back. Harmless: the 916 config below rewrites it anyway.
	 */
	r.wr_expected = 0xA5;
	if (rf69_write_reg(REG_SYNCVALUE1, r.wr_expected) == 0 &&
	    rf69_read_reg(REG_SYNCVALUE1, &r.wr_actual) == 0) {
		r.write_readback = (r.wr_actual == r.wr_expected);
	}

	/* --- 3: apply the 916 MHz table and confirm FRF took --- */
	r.config_applied = (rf69_config_916() == 0);
	if (r.config_applied) {
		rf69_read_reg(REG_FRFMSB, &r.frf_msb);
		rf69_read_reg(REG_FRFMID, &r.frf_mid);
		rf69_read_reg(REG_FRFLSB, &r.frf_lsb);
		r.freq_hz = rf69_get_freq();
		uint32_t delta = (r.freq_hz > RF69_FREQ_916_HZ)
					 ? r.freq_hz - RF69_FREQ_916_HZ
					 : RF69_FREQ_916_HZ - r.freq_hz;
		r.freq_ok = (delta <= RF69_FREQ_TOLERANCE_HZ);
	}

	/* --- 4: the radio's own state machine. ModeReady only asserts once the
	 * crystal is running, so this separates "SPI works" from "radio alive".
	 */
	if (rf69_write_reg(REG_OPMODE, RF_OPMODE_SEQUENCER_ON |
					RF_OPMODE_LISTEN_OFF |
					RF_OPMODE_STANDBY) == 0) {
		r.mode_ready = (rf69_wait_mode_ready(20000, &r.mode_ready_us) == 0);
	}

	/* --- 5: receive chain --- */
	r.rssi_dbm = rf69_read_rssi(true);
	/* A real reading lands well inside the RFM69's range; 0 or the rails mean
	 * nothing measured.
	 */
	r.rssi_plausible = (r.rssi_dbm < 0 && r.rssi_dbm > -128);

	/* --- 6: DIO1 wiring, deterministically --- */
	r.dio1 = rf69_dio1_check(&r);

done:
	r.all_passed = (r.link == RF69_LINK_OK) && r.write_readback &&
		       r.config_applied && r.freq_ok && r.mode_ready &&
		       r.rssi_plausible && (r.dio1 == RF69_DIO1_OK);

	if (out) {
		*out = r;
	}
	return r.all_passed ? 0 : -EIO;
}

void rf69_selftest_report(const struct rf69_selftest *r)
{
	if (r == NULL) {
		return;
	}

	LOG_INF("---- RFM69 self-test ----");

	switch (r->link) {
	case RF69_LINK_OK:
		LOG_INF("[PASS] present        REG_VERSION=0x%02x (RFM69/SX1231)",
			r->version);
		break;
	case RF69_LINK_MISO_LOW:
		LOG_ERR("[FAIL] present        REG_VERSION=0x00");
		LOG_ERR("       reads all zero: module unpowered, MISO not connected,");
		LOG_ERR("       or MISO shorted to GND. Check 3V3 and GND to the module.");
		break;
	case RF69_LINK_MISO_HIGH:
		LOG_ERR("[FAIL] present        REG_VERSION=0xFF");
		LOG_ERR("       reads all ones: MISO floating (nothing driving it), or");
		LOG_ERR("       MOSI/SCLK/NSS not reaching the module. Check D8/D9/D10 and D0.");
		break;
	case RF69_LINK_WRONG_DEVICE:
		LOG_ERR("[FAIL] present        REG_VERSION=0x%02x, expected 0x24",
			r->version);
		LOG_ERR("       bus works but this is not an RFM69 -- wrong device on CS,");
		LOG_ERR("       or MOSI/MISO swapped.");
		break;
	case RF69_LINK_BUS_ERROR:
		LOG_ERR("[FAIL] present        SPI transfer error -- bus not ready");
		break;
	}

	if (r->link != RF69_LINK_OK) {
		LOG_ERR("---- aborted: no usable SPI link ----");
		return;
	}

	LOG_INF("[%s] spi read/write  wrote 0x%02x, read 0x%02x",
		r->write_readback ? "PASS" : "FAIL", r->wr_expected, r->wr_actual);
	if (!r->write_readback) {
		LOG_ERR("       reads work but writes do not: check MOSI (D10).");
	}

	LOG_INF("[%s] 916 MHz config applied %u registers",
		r->config_applied ? "PASS" : "FAIL",
		(unsigned)ARRAY_SIZE(rf69_cfg_916));

	/* Split across two lines on purpose: a single call with nine arguments was
	 * silently dropped by CONFIG_LOG_MODE_DEFERRED.
	 */
	LOG_INF("[%s] frequency      %u Hz (expected %u)",
		r->freq_ok ? "PASS" : "FAIL", r->freq_hz, RF69_FREQ_916_HZ);
	LOG_INF("       FRF read back 0x%02x%02x%02x", r->frf_msb, r->frf_mid, r->frf_lsb);
	LOG_INF("       FRF written   0x%02x%02x%02x", (uint8_t)RF_FRFMSB_916,
		(uint8_t)RF_FRFMID_916, (uint8_t)RF_FRFLSB_916);

	LOG_INF("[%s] radio alive    ModeReady after %u us",
		r->mode_ready ? "PASS" : "FAIL", r->mode_ready_us);
	if (!r->mode_ready) {
		LOG_ERR("       SPI works but ModeReady never asserted: the crystal is");
		LOG_ERR("       not oscillating. Check the 32 MHz xtal and module supply.");
	}

	LOG_INF("[%s] rssi           %d dBm",
		r->rssi_plausible ? "PASS" : "FAIL", r->rssi_dbm);

	switch (r->dio1) {
	case RF69_DIO1_OK:
		LOG_INF("[PASS] dio1 wiring    follows FifoNotEmpty (low=%d high=%d)",
			r->dio1_low_level, r->dio1_high_level);
		break;
	case RF69_DIO1_NOT_WIRED:
		LOG_ERR("[FAIL] dio1 wiring    FifoNotEmpty toggles in the radio but the");
		LOG_ERR("       GPIO does not follow (low=%d high=%d) -- DIO1 is not",
			r->dio1_low_level, r->dio1_high_level);
		LOG_ERR("       connected to D2/P0.28, or is on a different DIO pin.");
		break;
	case RF69_DIO1_STUCK_HIGH:
		LOG_ERR("[FAIL] dio1 wiring    line stuck HIGH -- shorted to 3V3, or a");
		LOG_ERR("       pull-up is overriding the radio.");
		break;
	case RF69_DIO1_STUCK_LOW:
		LOG_ERR("[FAIL] dio1 wiring    line stuck LOW -- shorted to GND, or the");
		LOG_ERR("       radio pin is not driving.");
		break;
	case RF69_DIO1_NO_GPIO:
		LOG_ERR("[FAIL] dio1 wiring    no dio1-gpios in devicetree");
		break;
	case RF69_DIO1_INCONCLUSIVE:
		LOG_ERR("[FAIL] dio1 wiring    FifoNotEmpty never changed in the radio;");
		LOG_ERR("       cannot judge the wiring. Check the FIFO stage above.");
		break;
	}

	LOG_INF("---- %s ----", r->all_passed ? "ALL PASSED" : "FAILURES PRESENT");
}
