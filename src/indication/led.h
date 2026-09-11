/*
 * Status LED -- derived from legacy/periph/led/led.{c,h}
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * WHAT CHANGED, AND WHY
 *
 * Legacy had two discrete active-low LEDs -- LED_0 (yellow) and LED_1 (red) --
 * driven as a bitmask, and used them to show *BLE state*: a 30 ms yellow flash
 * every 10 s when connected, the same in red when advertising or disconnected.
 * There was no green, because there was no green LED.
 *
 * Here the LED shows *battery charge* instead (green / yellow / red), which is a
 * deliberate behaviour change -- see MIGRATION_NOTES.md. The XIAO nRF52840 has one
 * onboard RGB device, so this is a colour model rather than a two-pin bitmask, and
 * yellow is red+green lit together.
 *
 * The timings are legacy's, unchanged:
 *   LED_TIME1 30 ms    flash on
 *   LED_TIME2 300 ms   boot sequence step
 *   LED_TIME3 400 ms   boot sequence gap
 *   LED_TIME4 10000 ms flash off
 *
 * A 30 ms flash every 10 s is ~0.3% duty, which is what makes a permanently-on
 * indicator affordable on a battery.
 */

#ifndef ORANGELINK_LED_H_
#define ORANGELINK_LED_H_

#ifdef __cplusplus
extern "C" {
#endif

enum led_colour {
	LED_COLOUR_OFF = 0,
	LED_COLOUR_RED,
	LED_COLOUR_YELLOW,   /* red + green together */
	LED_COLOUR_GREEN,
};

/** @brief Claim the LED GPIOs and blank them. 0 on success, negative errno. */
int led_init(void);

/** @brief Steady on. */
void led_solid(enum led_colour colour);

/** @brief 30 ms flash every 10 s, repeating until changed. Legacy "twinkle". */
void led_heartbeat(enum led_colour colour);

/**
 * @brief Legacy LED_ACT_CROSS_TWINKLE, once: red 300 ms, yellow 300 ms, off.
 *
 * Kept as the power-on "I am alive" announcement. It runs before the first
 * battery reading exists, so it deliberately carries no charge meaning.
 */
void led_boot_flash(void);

/** @brief All off, cancelling any pattern. */
void led_off(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_LED_H_ */
