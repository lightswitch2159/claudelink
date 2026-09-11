/*
 * Status LED -- derived from legacy/periph/led/led.c
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "led.h"

LOG_MODULE_REGISTER(led, CONFIG_ORANGELINK_LOG_LEVEL);

/* Legacy timing constants, carried over verbatim from periph/led/led.c. */
#define LED_TIME1 30U      /* flash on */
#define LED_TIME2 300U     /* boot sequence step */
#define LED_TIME3 400U     /* boot sequence gap */
#define LED_TIME4 10000U   /* flash off */

/*
 * The overlay declares these aliases over the board's own led0/led1/led2, all
 * GPIO_ACTIVE_LOW. Blue is claimed but unused: nothing in the port maps to it,
 * and leaving it unclaimed would let it float visibly.
 */
static const struct gpio_dt_spec led_r =
	GPIO_DT_SPEC_GET(DT_ALIAS(orangelink_led_r), gpios);
static const struct gpio_dt_spec led_g =
	GPIO_DT_SPEC_GET(DT_ALIAS(orangelink_led_g), gpios);
static const struct gpio_dt_spec led_b =
	GPIO_DT_SPEC_GET(DT_ALIAS(orangelink_led_b), gpios);

struct led_step {
	uint16_t ms;
	enum led_colour colour;
};

/* Legacy LED_CROSS_PERIOD / LED_CROSS_OP. */
static const struct led_step boot_steps[] = {
	{ LED_TIME2, LED_COLOUR_RED },
	{ LED_TIME2, LED_COLOUR_YELLOW },
	{ LED_TIME3, LED_COLOUR_OFF },
};

/*
 * Legacy RED_LED_TWINKLE_PERIOD / _OP, colour-parameterised. Built at runtime
 * because the colour is now a variable where legacy had one table per colour.
 */
static struct led_step heartbeat_steps[2];

static void led_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_work, led_work_fn);
static K_MUTEX_DEFINE(led_lock);

static const struct led_step *cur_steps;
static uint8_t cur_step_count;
static uint8_t cur_step_idx;
static uint8_t cur_repeats;      /* 0 = forever */
static uint8_t cur_loop;
static bool ready;

static void apply_colour(enum led_colour colour)
{
	if (!ready) {
		return;
	}

	/* Yellow on a red/green/blue device is red and green together. */
	gpio_pin_set_dt(&led_r, colour == LED_COLOUR_RED ||
				colour == LED_COLOUR_YELLOW);
	gpio_pin_set_dt(&led_g, colour == LED_COLOUR_GREEN ||
				colour == LED_COLOUR_YELLOW);
	gpio_pin_set_dt(&led_b, 0);
}

/*
 * Pattern runner.
 *
 * Legacy drove this from an app_timer whose handler re-armed itself and indexed
 * the period/op tables with `curLedLoopCnt % ARRAY_SIZE(...)`. Same shape here on
 * a k_work_delayable; the two parallel arrays are folded into one table of steps,
 * which removes the "must be defined according to the order of led control
 * period" comment and the chance of the two drifting apart.
 */
static void led_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&led_lock, K_FOREVER);

	if (cur_steps == NULL) {
		apply_colour(LED_COLOUR_OFF);
		k_mutex_unlock(&led_lock);
		return;
	}

	apply_colour(cur_steps[cur_step_idx].colour);
	k_work_reschedule(&led_work, K_MSEC(cur_steps[cur_step_idx].ms));

	if (++cur_step_idx >= cur_step_count) {
		cur_step_idx = 0;
		/* repeats == 0 means run forever, as legacy actionTimes == 0 did. */
		if (cur_repeats != 0 && ++cur_loop >= cur_repeats) {
			cur_steps = NULL;
		}
	}

	k_mutex_unlock(&led_lock);
}

static void start_pattern(const struct led_step *steps, uint8_t count,
			  uint8_t repeats)
{
	k_mutex_lock(&led_lock, K_FOREVER);

	cur_steps = steps;
	cur_step_count = count;
	cur_step_idx = 0;
	cur_repeats = repeats;
	cur_loop = 0;

	k_mutex_unlock(&led_lock);

	/*
	 * Reschedule rather than calling the handler inline as legacy did. Inline
	 * would mean taking the mutex recursively from whichever thread called in,
	 * and the visible difference is one workqueue hop -- microseconds against a
	 * 30 ms flash.
	 */
	k_work_reschedule(&led_work, K_NO_WAIT);
}

int led_init(void)
{
	const struct gpio_dt_spec *pins[] = { &led_r, &led_g, &led_b };

	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		if (!gpio_is_ready_dt(pins[i])) {
			LOG_ERR("LED GPIO %u not ready", (unsigned int)i);
			return -ENODEV;
		}
		if (gpio_pin_configure_dt(pins[i], GPIO_OUTPUT_INACTIVE) != 0) {
			LOG_ERR("LED GPIO %u configure failed", (unsigned int)i);
			return -EIO;
		}
	}

	ready = true;
	apply_colour(LED_COLOUR_OFF);
	return 0;
}

void led_solid(enum led_colour colour)
{
	k_work_cancel_delayable(&led_work);

	k_mutex_lock(&led_lock, K_FOREVER);
	cur_steps = NULL;
	apply_colour(colour);
	k_mutex_unlock(&led_lock);
}

void led_heartbeat(enum led_colour colour)
{
	k_work_cancel_delayable(&led_work);

	k_mutex_lock(&led_lock, K_FOREVER);
	heartbeat_steps[0].ms = LED_TIME1;
	heartbeat_steps[0].colour = colour;
	heartbeat_steps[1].ms = LED_TIME4;
	heartbeat_steps[1].colour = LED_COLOUR_OFF;
	k_mutex_unlock(&led_lock);

	start_pattern(heartbeat_steps, ARRAY_SIZE(heartbeat_steps), 0);
}

void led_boot_flash(void)
{
	k_work_cancel_delayable(&led_work);
	start_pattern(boot_steps, ARRAY_SIZE(boot_steps), 1);
}

void led_off(void)
{
	led_solid(LED_COLOUR_OFF);
}
