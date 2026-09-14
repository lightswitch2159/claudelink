/*
 * Battery monitor -- 1S LiPo on the XIAO nRF52840.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_BT_BAS)
#include <zephyr/bluetooth/services/bas.h>
#endif

#include "battery.h"
#include "indication/led.h"

LOG_MODULE_REGISTER(battery, CONFIG_ORANGELINK_LOG_LEVEL);

/* Legacy BAT_LOW_DET_INVL was 180000 ms. Kept -- a LiPo does not move fast. */
#define BATTERY_SAMPLE_INTERVAL_MS 180000

/* Long enough for led_boot_flash()'s 300+300+400 ms sequence to finish. */
#define BATTERY_FIRST_SAMPLE_DELAY_MS 1100

#define BATTERY_UNKNOWN_PERCENT 0xFFU

static const struct adc_dt_spec vbatt =
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

/*
 * Battery sense enable, P0.14, declared GPIO_ACTIVE_LOW in the overlay.
 *
 * Seeed document that with this pin high the sense path is disabled and P0.31 can
 * rise toward 3.6 V, which risks the pin. Legacy pulsed its ADC on and off around
 * each sample; here the sense path is enabled once at init and left enabled,
 * which removes that hazard window entirely. The cost is the divider's standing
 * current -- about 2.8 uA at 4.2 V through ~1.5 Mohm -- which is far below the
 * radio and BLE idle budget.
 */
#if DT_NODE_EXISTS(DT_NODELABEL(vbat_enable))
static const struct gpio_dt_spec vbatt_enable =
	GPIO_DT_SPEC_GET(DT_NODELABEL(vbat_enable), gpios);
#define HAS_VBAT_ENABLE 1
#endif

/*
 * Charger current select, P0.13. See the overlay for the polarity rationale.
 * Driven once at init; the charger is hardware and needs no further attention.
 */
#if DT_NODE_EXISTS(DT_NODELABEL(chg_current))
static const struct gpio_dt_spec chg_current =
	GPIO_DT_SPEC_GET(DT_NODELABEL(chg_current), gpios);
#define HAS_CHG_CURRENT 1
#endif

static uint16_t last_mv;
static uint8_t last_percent = BATTERY_UNKNOWN_PERCENT;

/*
 * 1S LiPo open-circuit discharge curve, mV -> percent, descending.
 *
 * Deliberately non-linear: a LiPo sits near 3.8 V for most of its usable charge
 * and then falls away quickly, so a straight line between 4.2 V and 3.0 V would
 * read ~50% for most of the discharge and then collapse. Interpolated linearly
 * between neighbouring points.
 *
 * These are nominal figures for a light load. They are good enough to pick a
 * colour; they are not a fuel gauge, and under a transmit burst the measured
 * voltage will sag and read low for the duration.
 */
struct curve_point {
	uint16_t mv;
	uint8_t percent;
};

static const struct curve_point lipo_curve[] = {
	{ 4200, 100 }, { 4150, 95 }, { 4110, 90 }, { 4080, 85 },
	{ 4020, 80 },  { 3980, 75 }, { 3950, 70 }, { 3910, 65 },
	{ 3870, 60 },  { 3850, 55 }, { 3840, 50 }, { 3820, 45 },
	{ 3800, 40 },  { 3790, 35 }, { 3770, 30 }, { 3750, 25 },
	{ 3730, 20 },  { 3710, 15 }, { 3690, 10 }, { 3610, 5 },
	{ 3270, 0 },
};

static uint8_t mv_to_percent(uint16_t mv)
{
	if (mv >= lipo_curve[0].mv) {
		return 100;
	}

	for (size_t i = 1; i < ARRAY_SIZE(lipo_curve); i++) {
		const struct curve_point *hi = &lipo_curve[i - 1];
		const struct curve_point *lo = &lipo_curve[i];

		if (mv >= lo->mv) {
			uint32_t span_mv = hi->mv - lo->mv;
			uint32_t span_pc = hi->percent - lo->percent;

			return lo->percent +
			       (uint8_t)(((uint32_t)(mv - lo->mv) * span_pc) / span_mv);
		}
	}

	return 0;
}

/*
 * Colour thresholds, with hysteresis.
 *
 * Without hysteresis a cell resting exactly on a boundary -- and one under a
 * bursty RF load will wander across it -- makes the LED change colour every
 * sample. A reading must climb CONFIG_ORANGELINK_BATTERY_HYSTERESIS above a
 * threshold before the colour is allowed back up.
 */
static enum led_colour colour_for(uint8_t percent, enum led_colour current)
{
	const uint8_t green = CONFIG_ORANGELINK_BATTERY_GREEN_PERCENT;
	const uint8_t yellow = CONFIG_ORANGELINK_BATTERY_YELLOW_PERCENT;
	const uint8_t hyst = CONFIG_ORANGELINK_BATTERY_HYSTERESIS;

	switch (current) {
	case LED_COLOUR_GREEN:
		/* Fall only when clearly below. */
		return (percent < green) ? ((percent < yellow) ? LED_COLOUR_RED
							       : LED_COLOUR_YELLOW)
					 : LED_COLOUR_GREEN;
	case LED_COLOUR_YELLOW:
		if (percent >= green + hyst) {
			return LED_COLOUR_GREEN;
		}
		return (percent < yellow) ? LED_COLOUR_RED : LED_COLOUR_YELLOW;
	case LED_COLOUR_RED:
		if (percent >= green + hyst) {
			return LED_COLOUR_GREEN;
		}
		return (percent >= yellow + hyst) ? LED_COLOUR_YELLOW : LED_COLOUR_RED;
	default:
		/* First reading: no hysteresis to apply. */
		if (percent >= green) {
			return LED_COLOUR_GREEN;
		}
		return (percent >= yellow) ? LED_COLOUR_YELLOW : LED_COLOUR_RED;
	}
}

static int sample_millivolts(uint16_t *out_mv, int16_t *out_raw, int32_t *out_pin_mv)
{
	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int32_t adc_mv;
	int err;

	err = adc_sequence_init_dt(&vbatt, &seq);
	if (err) {
		return err;
	}

	err = adc_read(vbatt.dev, &seq);
	if (err) {
		return err;
	}

	/*
	 * Guard against a mis-set gain. If the reading is pinned at full scale the
	 * divider output is above the reference window and every voltage above that
	 * point reads identically -- which would look like a healthy battery no
	 * matter how flat the cell is.
	 */
	if (raw >= (int16_t)((1 << vbatt.resolution) - 1)) {
		LOG_WRN("ADC at full scale (%d): gain too high for this divider, "
			"reading is not trustworthy", raw);
	}

	adc_mv = raw;
	err = adc_raw_to_millivolts_dt(&vbatt, &adc_mv);
	if (err) {
		return err;
	}

	/*
	 * Undo the onboard divider. Expressed as a ratio in Kconfig rather than a
	 * float constant so it can be corrected by bench measurement without
	 * touching code: measure the cell with a meter, compare against the mV this
	 * logs, and scale FULL_OHMS.
	 *
	 * The intermediate MUST be 64-bit. This was a uint32_t and silently wrapped
	 * once the Feather's real divider values went in: the XIAO's fudged 139300
	 * survived (3600 * 139300 = 5.0e8), but the Rev G schematic's 2806000 does
	 * not (3600 * 2806000 = 1.0e10 against a 4.29e9 ceiling). Every pin reading
	 * above ~1530 mV wrapped, so a healthy cell always reported nonsense -- 0%
	 * with the cell fitted, and a plausible-looking 73% without one. A BUILD_ASSERT
	 * cannot express this alone, because the bound depends on the ADC full scale,
	 * so the width is fixed here instead.
	 */
	*out_mv = (uint16_t)(((uint64_t)adc_mv * CONFIG_ORANGELINK_BATTERY_FULL_OHMS) /
			     CONFIG_ORANGELINK_BATTERY_OUTPUT_OHMS);
	*out_raw = raw;
	*out_pin_mv = adc_mv;
	return 0;
}

static void battery_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_work, battery_work_fn);

static void battery_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	static enum led_colour shown = LED_COLOUR_OFF;
	static bool logged_calibration;
	uint16_t mv;
	uint8_t percent;
	enum led_colour want;
	int16_t raw;
	int32_t pin_mv;

	if (sample_millivolts(&mv, &raw, &pin_mv) != 0) {
		LOG_ERR("battery sample failed");
		k_work_reschedule(&battery_work, K_MSEC(BATTERY_SAMPLE_INTERVAL_MS));
		return;
	}

	/*
	 * Emit the raw numbers once so the divider can be calibrated without a
	 * debugger: measure the cell across its terminals and compare against
	 * "cell" below. If they disagree, scale FULL_OHMS by the ratio of the two.
	 */
	if (!logged_calibration) {
		logged_calibration = true;
		LOG_INF("calibration: raw=%d pin=%d mV cell=%u mV "
			"(ratio %d/%d); measure the cell and correct "
			"CONFIG_ORANGELINK_BATTERY_FULL_OHMS if these disagree",
			raw, pin_mv, mv,
			CONFIG_ORANGELINK_BATTERY_FULL_OHMS,
			CONFIG_ORANGELINK_BATTERY_OUTPUT_OHMS);
	}

	percent = mv_to_percent(mv);
	last_mv = mv;
	last_percent = percent;

	/*
	 * Publish to the BLE Battery Service.
	 *
	 * CONFIG_BT_BAS was already enabled but nothing ever set a level, and
	 * Zephyr's BAS initialises its characteristic to 100 -- so every client,
	 * AndroidAPS included, read a permanent "100%" that had nothing to do with
	 * the cell. Legacy pushed this from a timer via
	 * ble_bas_battery_level_update(); the equivalent here is every sample.
	 */
#if defined(CONFIG_BT_BAS)
	{
		int bas_err = bt_bas_set_battery_level(percent);

		if (bas_err) {
			LOG_WRN("bt_bas_set_battery_level(%u) failed (%d)", percent,
				bas_err);
		}
	}
#endif

	want = colour_for(percent, shown);
	if (want != shown) {
		shown = want;
		led_heartbeat(want);
		LOG_INF("battery %u mV, %u%% -> %s", mv, percent,
			want == LED_COLOUR_GREEN ? "green" :
			want == LED_COLOUR_YELLOW ? "yellow" : "red");
	} else {
		LOG_DBG("battery %u mV, %u%% (raw %d)", mv, percent, raw);
	}

	k_work_reschedule(&battery_work, K_MSEC(BATTERY_SAMPLE_INTERVAL_MS));
}

int battery_init(void)
{
	int err;

	if (!adc_is_ready_dt(&vbatt)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}

	err = adc_channel_setup_dt(&vbatt);
	if (err) {
		LOG_ERR("ADC channel setup failed (%d)", err);
		return err;
	}

#if defined(HAS_VBAT_ENABLE)
	if (!gpio_is_ready_dt(&vbatt_enable)) {
		LOG_ERR("VBAT enable GPIO not ready");
		return -ENODEV;
	}
#endif

#if defined(HAS_VBAT_ENABLE)
	/* ACTIVE_LOW in devicetree, so "active" drives the pin low -- see the
	 * hazard note above. Boards without a sense-enable pin simply omit the node.
	 */
	err = gpio_pin_configure_dt(&vbatt_enable, GPIO_OUTPUT_ACTIVE);
	if (err) {
		LOG_ERR("VBAT enable configure failed (%d)", err);
		return err;
	}
#endif

	/*
	 * Charge current. ACTIVE_LOW in devicetree, so OUTPUT_ACTIVE drives P0.13
	 * low and selects ~100 mA; GPIO_INPUT leaves it high-impedance for ~50 mA.
	 * A failure here is not fatal -- it only means the charger stays on its
	 * default rate.
	 */
#if defined(HAS_CHG_CURRENT)
	if (gpio_is_ready_dt(&chg_current)) {
		int cerr = gpio_pin_configure_dt(&chg_current,
			IS_ENABLED(CONFIG_ORANGELINK_BATTERY_FAST_CHARGE)
				? GPIO_OUTPUT_ACTIVE : GPIO_INPUT);

		if (cerr) {
			LOG_WRN("charge current select failed (%d)", cerr);
		} else {
			LOG_INF("charge current ~%u mA",
				IS_ENABLED(CONFIG_ORANGELINK_BATTERY_FAST_CHARGE)
					? 100U : 50U);
		}
	} else {
		LOG_WRN("charge current GPIO not ready");
	}
#else
	/* No charge-current select on this board; the charger runs at its default. */
#endif

	/* The divider needs a moment to settle before the first conversion. */
	k_sleep(K_MSEC(5));

	/*
	 * Deliberately not immediate. led_boot_flash() is a 1 s sequence started at
	 * the top of main(), and taking the LED over at K_NO_WAIT cut it off partway
	 * through. Delaying the first sample lets the boot announcement complete,
	 * then the charge colour replaces it.
	 */
	k_work_reschedule(&battery_work, K_MSEC(BATTERY_FIRST_SAMPLE_DELAY_MS));
	return 0;
}

uint16_t battery_millivolts(void)
{
	return last_mv;
}

uint8_t battery_percent(void)
{
	return last_percent;
}

bool battery_is_low(void)
{
	return last_percent != BATTERY_UNKNOWN_PERCENT &&
	       last_percent < CONFIG_ORANGELINK_BATTERY_YELLOW_PERCENT;
}
