/*
 * Semtech SX1276 (RFM95) driven in OOK -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Configuration values follow ecc1/gnarl lib/radio/rfm95.c (MIT), a working
 * SX1276 OOK driver for Medtronic pumps, rather than a transcription of this
 * project's SX1231 table. docs/sx1276-reference.md records the six places where
 * those two disagree; each of them fails silently on air.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "sx1276.h"
#include "sx1276_registers.h"

LOG_MODULE_REGISTER(sx1276, CONFIG_ORANGELINK_LOG_LEVEL);

#define SX1276_NODE DT_NODELABEL(sx1276)

#if !DT_NODE_EXISTS(SX1276_NODE)
#error "devicetree node `sx1276` not found -- see dts/bindings/spi/orangelink,sx1276.yaml"
#endif

static const struct spi_dt_spec sx_bus = SPI_DT_SPEC_GET(
	SX1276_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER, 0);

static const struct gpio_dt_spec sx_reset =
	GPIO_DT_SPEC_GET_OR(SX1276_NODE, reset_gpios, {0});
static const struct gpio_dt_spec sx_dio2 =
	GPIO_DT_SPEC_GET_OR(SX1276_NODE, dio2_gpios, {0});

/* FSTEP = FXOSC / 2^19. Same crystal and same encoding as the SX1231, so the
 * frequency arithmetic carries over unchanged.
 */
#define SX_FXOSC_HZ 32000000ULL
#define SX_FSTEP_SHIFT 19

/* 916.548 MHz -- the value the legacy 916 table used, kept so both radios land
 * on the same channel by default.
 */
#define SX_DEFAULT_FREQ_HZ 916548000U

static bool initialised;
static int16_t last_rssi_dbm = INT16_MIN;

K_MUTEX_DEFINE(sx_lock);
#define SX_LOCK()   k_mutex_lock(&sx_lock, K_FOREVER)
#define SX_UNLOCK() k_mutex_unlock(&sx_lock)

/* ------------------------------------------------------------------------- *
 * Raw register access. Bit 7 of the address selects write, as on the SX1231.
 * ------------------------------------------------------------------------- */

int sx1276_read_reg(uint8_t addr, uint8_t *value)
{
	uint8_t tx[2] = { addr & 0x7F, 0x00 };
	uint8_t rx[2] = { 0 };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf rxb = { .buf = rx, .len = sizeof(rx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };
	int err;

	if (value == NULL) {
		return -EINVAL;
	}

	SX_LOCK();
	err = spi_transceive_dt(&sx_bus, &txs, &rxs);
	SX_UNLOCK();
	if (err) {
		return err;
	}

	*value = rx[1];
	return 0;
}

int sx1276_write_reg(uint8_t addr, uint8_t value)
{
	uint8_t tx[2] = { addr | 0x80, value };
	const struct spi_buf txb = { .buf = tx, .len = sizeof(tx) };
	const struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	int err;

	SX_LOCK();
	err = spi_write_dt(&sx_bus, &txs);
	SX_UNLOCK();
	return err;
}

/* ------------------------------------------------------------------------- *
 * Reset and mode
 * ------------------------------------------------------------------------- */

int sx1276_reset(void)
{
	/* Reset is optional in devicetree: on some modules it is tied off and only
	 * power-on reset is available. Not an error when absent.
	 */
	if (sx_reset.port == NULL) {
		LOG_DBG("no reset-gpios; relying on power-on reset");
		return 0;
	}

	if (!gpio_is_ready_dt(&sx_reset)) {
		return -ENODEV;
	}

	/* Active low, per the datasheet: hold ~100 us, then allow 5 ms to settle. */
	gpio_pin_configure_dt(&sx_reset, GPIO_OUTPUT_ACTIVE);
	k_busy_wait(150);
	gpio_pin_set_dt(&sx_reset, 0);
	k_sleep(K_MSEC(5));
	return 0;
}

static int sx_write_opmode(uint8_t mode)
{
	/*
	 * Modulation is re-asserted on every mode write.
	 *
	 * On the SX1231 the modulation lives in its own RegDataModul, written once
	 * at init. Here it shares RegOpMode with the mode bits, so writing the mode
	 * alone would clear LongRangeMode/OOK back toward the LoRa default.
	 */
	return sx1276_write_reg(SX_REG_OPMODE,
		SX_OPMODE_FSK_OOK | SX_OPMODE_MODULATION_OOK | (mode & SX_OPMODE_MASK));
}

static int sx_wait_mode_ready(uint32_t timeout_us)
{
	uint32_t waited = 0;
	uint8_t flags;

	while (waited < timeout_us) {
		if (sx1276_read_reg(SX_REG_IRQFLAGS1, &flags) == 0 &&
		    (flags & SX_IRQ1_MODEREADY)) {
			return 0;
		}
		k_busy_wait(100);
		waited += 100;
	}
	return -ETIMEDOUT;
}

int sx1276_set_mode(enum sx1276_mode mode)
{
	static const uint8_t map[] = {
		[SX1276_MODE_SLEEP]   = SX_MODE_SLEEP,
		[SX1276_MODE_STANDBY] = SX_MODE_STANDBY,
		[SX1276_MODE_FSTX]    = SX_MODE_FSTX,
		[SX1276_MODE_TX]      = SX_MODE_TX,
		[SX1276_MODE_FSRX]    = SX_MODE_FSRX,
		[SX1276_MODE_RX]      = SX_MODE_RX,
	};
	int err;

	if ((size_t)mode >= ARRAY_SIZE(map)) {
		return -EINVAL;
	}

	err = sx_write_opmode(map[mode]);
	if (err) {
		return err;
	}

	/* Sleep has no ModeReady to wait for. */
	if (mode == SX1276_MODE_SLEEP) {
		return 0;
	}
	return sx_wait_mode_ready(10000);
}

/* ------------------------------------------------------------------------- *
 * 916 MHz OOK configuration
 *
 * Values from gnarl's rfm95_init(), which is known to work against real pumps.
 * Where this differs from our SX1231 table the reason is noted -- those are the
 * traps, not stylistic choices.
 * ------------------------------------------------------------------------- */

int sx1276_config_916(void)
{
	int err;

	/*
	 * Sleep, twice.
	 *
	 * LongRangeMode can only be changed while in Sleep, and the first write is
	 * what gets us there -- it cannot also change the modulation. gnarl calls
	 * set_mode_sleep() twice for exactly this reason. Doing it once leaves the
	 * part in LoRa mode and every subsequent FSK/OOK register write lands in the
	 * wrong bank.
	 */
	err = sx1276_set_mode(SX1276_MODE_SLEEP);
	if (err) {
		return err;
	}
	err = sx1276_set_mode(SX1276_MODE_SLEEP);
	if (err) {
		return err;
	}

	static const uint8_t cfg[][2] = {
		/* 16384 bps nominal; 0x07A1 works out at 16385 bps. Same divisor as
		 * the SX1231, since both run a 32 MHz crystal.
		 */
		{ SX_REG_BITRATEMSB,    0x07 },
		{ SX_REG_BITRATELSB,    0xA1 },

		/* 64 samples of RSSI averaging. */
		{ SX_REG_RSSICONFIG,    0x05 },

		/* 200 kHz. Mantissa 20, exponent 1 -- NOT exponent 0 as on the
		 * SX1231, whose OOK bandwidth formula has a different power term.
		 */
		{ SX_REG_RXBW,          SX_RXBW_MANT_20 | 0x01 },

		/* 24 preamble bytes. The SX1231 table used 16; gnarl raised it with
		 * the note "make sure enough preamble bytes are sent".
		 */
		{ SX_REG_PREAMBLEMSB,   0x00 },
		{ SX_REG_PREAMBLELSB,   0x18 },

		/* 4-byte sync word FF 00 FF 00, as the pump expects. */
		{ SX_REG_SYNCCONFIG,    SX_SYNC_ON | SX_SYNC_SIZE_4 },
		{ SX_REG_SYNCVALUE1,    0xFF },
		{ SX_REG_SYNCVALUE2,    0x00 },
		{ SX_REG_SYNCVALUE3,    0xFF },
		{ SX_REG_SYNCVALUE4,    0x00 },

		/* Unlimited-length packet format (datasheet 4.2.13.2): fixed format
		 * with PayloadLength 0. Framing is the zero terminator, as on the
		 * SX1231 path.
		 */
		{ SX_REG_PACKETCONFIG1, SX_PACKET1_FORMAT_FIXED | SX_PACKET1_DCFREE_OFF |
					SX_PACKET1_CRC_OFF },
		{ SX_REG_PAYLOADLENGTH, 0x00 },
		{ SX_REG_PACKETCONFIG2, SX_PACKET2_PACKET_MODE },

		/* DIO2 = SyncAddressMatch. This is the receive trigger and the moment
		 * RSSI must be sampled -- see MIGRATION_NOTES 13.3.
		 */
		{ SX_REG_DIOMAPPING1,   SX_DIO2_SYNCADDRESS },
	};

	for (size_t i = 0; i < ARRAY_SIZE(cfg); i++) {
		err = sx1276_write_reg(cfg[i][0], cfg[i][1]);
		if (err) {
			LOG_ERR("config write %u (reg 0x%02x) failed (%d)",
				(unsigned int)i, cfg[i][0], err);
			return err;
		}
	}

	err = sx1276_set_freq(SX_DEFAULT_FREQ_HZ);
	if (err) {
		return err;
	}

	return sx1276_set_mode(SX1276_MODE_SLEEP);
}

/* ------------------------------------------------------------------------- *
 * Frequency
 * ------------------------------------------------------------------------- */

uint32_t sx1276_get_freq(void)
{
	uint8_t msb = 0, mid = 0, lsb = 0;

	if (sx1276_read_reg(SX_REG_FRFMSB, &msb) ||
	    sx1276_read_reg(SX_REG_FRFMID, &mid) ||
	    sx1276_read_reg(SX_REG_FRFLSB, &lsb)) {
		return 0;
	}

	uint32_t frf = ((uint32_t)msb << 16) | ((uint32_t)mid << 8) | lsb;

	return (uint32_t)(((uint64_t)frf * SX_FXOSC_HZ) >> SX_FSTEP_SHIFT);
}

int sx1276_set_freq(uint32_t freq_hz)
{
	/* Rounded, not truncated: the half-step term keeps the programmed channel
	 * within half an FSTEP of the request.
	 */
	uint32_t frf = (uint32_t)((((uint64_t)freq_hz << SX_FSTEP_SHIFT) +
				   SX_FXOSC_HZ / 2) / SX_FXOSC_HZ);
	int err;

	err = sx1276_write_reg(SX_REG_FRFMSB, (uint8_t)(frf >> 16));
	if (err) {
		return err;
	}
	err = sx1276_write_reg(SX_REG_FRFMID, (uint8_t)(frf >> 8));
	if (err) {
		return err;
	}
	return sx1276_write_reg(SX_REG_FRFLSB, (uint8_t)frf);
}

/* ------------------------------------------------------------------------- *
 * FIFO and status
 * ------------------------------------------------------------------------- */

static bool sx_irq2(uint8_t bit, bool on_error)
{
	uint8_t f = 0;

	if (sx1276_read_reg(SX_REG_IRQFLAGS2, &f)) {
		return on_error;
	}
	return (f & bit) != 0;
}

bool sx1276_fifo_is_empty(void)      { return sx_irq2(SX_IRQ2_FIFOEMPTY, true); }
bool sx1276_fifo_is_full(void)       { return sx_irq2(SX_IRQ2_FIFOFULL, false); }
bool sx1276_fifo_level_exceeded(void){ return sx_irq2(SX_IRQ2_FIFOLEVEL, false); }
bool sx1276_packet_sent(void)        { return sx_irq2(SX_IRQ2_PACKETSENT, false); }

bool sx1276_sync_matched(void)
{
	uint8_t f = 0;

	if (sx1276_read_reg(SX_REG_IRQFLAGS1, &f)) {
		return false;
	}
	return (f & SX_IRQ1_SYNCMATCH) != 0;
}

int sx1276_fifo_write_byte(uint8_t b)
{
	return sx1276_write_reg(SX_REG_FIFO, b);
}

int sx1276_fifo_write(const uint8_t *data, uint16_t len)
{
	uint8_t addr = SX_REG_FIFO | 0x80;
	const struct spi_buf txb[2] = {
		{ .buf = &addr, .len = 1 },
		{ .buf = (void *)data, .len = len },
	};
	const struct spi_buf_set txs = { .buffers = txb, .count = 2 };
	int err;

	if (data == NULL || len == 0) {
		return -EINVAL;
	}

	SX_LOCK();
	err = spi_write_dt(&sx_bus, &txs);
	SX_UNLOCK();
	return err;
}

int sx1276_fifo_read_byte(uint8_t *b)
{
	return sx1276_read_reg(SX_REG_FIFO, b);
}

int sx1276_fifo_clear(void)
{
	uint8_t scratch;

	/* No flush bit in FSK/OOK mode -- drain it. Bounded by the FIFO depth so a
	 * stuck FIFOEMPTY cannot spin forever.
	 */
	for (int i = 0; i < SX1276_FIFO_SIZE + 1; i++) {
		if (sx1276_fifo_is_empty()) {
			return 0;
		}
		if (sx1276_fifo_read_byte(&scratch) != 0) {
			return -EIO;
		}
	}
	return -EIO;
}

int sx1276_set_payload_len(uint8_t len)
{
	return sx1276_write_reg(SX_REG_PAYLOADLENGTH, len);
}

int sx1276_latch_rssi(void)
{
	uint8_t raw = 0;
	int err = sx1276_read_reg(SX_REG_RSSIVALUE, &raw);

	if (err) {
		return err;
	}

	/* RegRssiValue is -RssiValue/2 dBm on this part, where the SX1231 used
	 * RssiValue/2 - 73. Different scale, same trap if sampled late.
	 */
	last_rssi_dbm = -(int16_t)(raw / 2);
	return 0;
}

int16_t sx1276_read_rssi(void)
{
	return last_rssi_dbm;
}

/* ------------------------------------------------------------------------- *
 * Receive trigger interrupt -- DIO2 / SyncAddressMatch
 * ------------------------------------------------------------------------- */

static struct gpio_callback dio2_cb_data;
static struct k_sem *dio2_sem;

static void dio2_handler(const struct device *port, struct gpio_callback *cb,
			 gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (dio2_sem) {
		k_sem_give(dio2_sem);
	}
}

int sx1276_rx_irq_enable(struct k_sem *sem)
{
	int err;

	if (sx_dio2.port == NULL) {
		return -ENODEV;
	}

	dio2_sem = sem;

	err = gpio_pin_configure_dt(&sx_dio2, GPIO_INPUT);
	if (err) {
		return err;
	}

	gpio_init_callback(&dio2_cb_data, dio2_handler, BIT(sx_dio2.pin));
	err = gpio_add_callback(sx_dio2.port, &dio2_cb_data);
	if (err) {
		return err;
	}

	return gpio_pin_interrupt_configure_dt(&sx_dio2, GPIO_INT_EDGE_TO_ACTIVE);
}

int sx1276_rx_irq_disable(void)
{
	if (sx_dio2.port == NULL) {
		return -ENODEV;
	}

	gpio_pin_interrupt_configure_dt(&sx_dio2, GPIO_INT_DISABLE);
	gpio_remove_callback(sx_dio2.port, &dio2_cb_data);
	dio2_sem = NULL;
	return 0;
}

/* ------------------------------------------------------------------------- *
 * Power
 * ------------------------------------------------------------------------- */

int sx1276_set_power_level(uint8_t level)
{
	/*
	 * PA_BOOST, which is what RFM95 modules bond to the antenna -- the RFO pin
	 * is not connected on them. Same class of trap as the RFM69HCW PA0/PA1+PA2
	 * mistake recorded in MIGRATION_NOTES 9.x: choosing the wrong output means
	 * nothing radiates while every register reads back correctly.
	 */
	int err = sx1276_write_reg(SX_REG_PACONFIG,
				   SX_PACONFIG_PABOOST | (level & 0x0F));
	if (err) {
		return err;
	}
	return sx1276_write_reg(SX_REG_PADAC, SX_PADAC_20DBM_OFF);
}

/* ------------------------------------------------------------------------- *
 * Init and self-test
 * ------------------------------------------------------------------------- */

int sx1276_init(void)
{
	if (!spi_is_ready_dt(&sx_bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	if (sx_dio2.port != NULL && !gpio_is_ready_dt(&sx_dio2)) {
		LOG_WRN("dio2-gpios present but not ready");
	}

	sx1276_reset();
	initialised = true;
	LOG_INF("bound to %s, CS via devicetree, DIO2 %s",
		sx_bus.bus->name, sx_dio2.port ? "present" : "absent");
	return 0;
}

int sx1276_selftest_run(struct sx1276_selftest *out)
{
	struct sx1276_selftest r = { .link = SX1276_LINK_NO_RESPONSE };
	uint8_t v = 0;

	if (!initialised && sx1276_init() != 0) {
		goto done;
	}

	if (sx1276_read_reg(SX_REG_VERSION, &v) != 0) {
		goto done;
	}
	r.version = v;

	if (v == 0x00 || v == 0xFF) {
		r.link = SX1276_LINK_NO_RESPONSE;
		goto done;
	}
	if (v != SX1276_VERSION) {
		r.link = SX1276_LINK_WRONG_DEVICE;
		goto done;
	}
	r.link = SX1276_LINK_OK;

	/* Prove writes stick, using a register with no side effects. */
	r.wr_expected = 0xA5;
	if (sx1276_write_reg(SX_REG_SYNCVALUE1, r.wr_expected) == 0 &&
	    sx1276_read_reg(SX_REG_SYNCVALUE1, &r.wr_actual) == 0) {
		r.spi_rw = (r.wr_actual == r.wr_expected);
	}

	if (sx1276_config_916() == 0) {
		r.cfg_applied = true;
	}

	/* Read the frequency back: a write that does not land is otherwise silent,
	 * which cost this project a long detour on the SX1231 side.
	 */
	r.freq_hz = sx1276_get_freq();
	r.freq_ok = (r.freq_hz > SX_DEFAULT_FREQ_HZ - 2000) &&
		    (r.freq_hz < SX_DEFAULT_FREQ_HZ + 2000);

	r.all_passed = r.spi_rw && r.cfg_applied && r.freq_ok;

done:
	if (out) {
		*out = r;
	}
	return r.all_passed ? 0 : -EIO;
}

void sx1276_selftest_report(const struct sx1276_selftest *r)
{
	if (r == NULL) {
		return;
	}

	LOG_INF("---- SX1276 self-test ----");

	switch (r->link) {
	case SX1276_LINK_NO_RESPONSE:
		LOG_ERR("[FAIL] present        RegVersion=0x%02x", r->version);
		LOG_ERR("       reads all ones or all zeros: MISO floating, or");
		LOG_ERR("       MOSI/SCK/NSS not reaching the module.");
		LOG_ERR("---- aborted: no usable SPI link ----");
		return;
	case SX1276_LINK_WRONG_DEVICE:
		LOG_ERR("[FAIL] present        RegVersion=0x%02x, expected 0x%02x",
			r->version, SX1276_VERSION);
		LOG_ERR("       bus works but this is not an SX1276 -- wrong device on");
		LOG_ERR("       CS, or MOSI/MISO swapped.");
		return;
	case SX1276_LINK_OK:
		LOG_INF("[PASS] present        RegVersion=0x%02x (SX1276)", r->version);
		break;
	}

	LOG_INF("%s spi read/write  wrote 0x%02x, read 0x%02x",
		r->spi_rw ? "[PASS]" : "[FAIL]", r->wr_expected, r->wr_actual);
	LOG_INF("%s 916 OOK config applied", r->cfg_applied ? "[PASS]" : "[FAIL]");
	LOG_INF("%s frequency       reads %u Hz",
		r->freq_ok ? "[PASS]" : "[FAIL]", r->freq_hz);
	LOG_INF("---- %s ----", r->all_passed ? "ALL PASSED" : "FAILURES ABOVE");
}

int sx1276_rssi_survey(int16_t *min_dbm, int16_t *max_dbm)
{
	int16_t lo = INT16_MAX, hi = INT16_MIN;
	int err;

	err = sx1276_set_mode(SX1276_MODE_RX);
	if (err) {
		return err;
	}

	for (int i = 0; i < 24; i++) {
		uint8_t raw = 0;

		if (sx1276_read_reg(SX_REG_RSSIVALUE, &raw) == 0) {
			int16_t dbm = -(int16_t)(raw / 2);

			lo = MIN(lo, dbm);
			hi = MAX(hi, dbm);
		}
		k_sleep(K_MSEC(1));
	}

	sx1276_set_mode(SX1276_MODE_SLEEP);

	if (min_dbm) {
		*min_dbm = lo;
	}
	if (max_dbm) {
		*max_dbm = hi;
	}
	return (hi > lo) ? (hi - lo) : 0;
}

void sx1276_dump_regs(void)
{
	static const struct { uint8_t addr; const char *name; } regs[] = {
		{ SX_REG_OPMODE,        "OPMODE       " },
		{ SX_REG_BITRATEMSB,    "BITRATEMSB   " },
		{ SX_REG_BITRATELSB,    "BITRATELSB   " },
		{ SX_REG_PACONFIG,      "PACONFIG     " },
		{ SX_REG_LNA,           "LNA          " },
		{ SX_REG_RXBW,          "RXBW         " },
		{ SX_REG_OOKPEAK,       "OOKPEAK      " },
		{ SX_REG_PREAMBLELSB,   "PREAMBLELSB  " },
		{ SX_REG_SYNCCONFIG,    "SYNCCONFIG   " },
		{ SX_REG_PACKETCONFIG1, "PACKETCONFIG1" },
		{ SX_REG_PAYLOADLENGTH, "PAYLOADLENGTH" },
		{ SX_REG_FIFOTHRESH,    "FIFOTHRESH   " },
		{ SX_REG_DIOMAPPING1,   "DIOMAPPING1  " },
		{ SX_REG_VERSION,       "VERSION      " },
	};

	LOG_INF("---- SX1276 register state ----");
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		uint8_t v = 0;

		if (sx1276_read_reg(regs[i].addr, &v) == 0) {
			LOG_INF("  0x%02x %s = 0x%02x", regs[i].addr, regs[i].name, v);
		}
	}
	LOG_INF("  freq = %u Hz", sx1276_get_freq());
}
