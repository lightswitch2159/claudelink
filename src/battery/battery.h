/*
 * Battery monitor -- 1S LiPo on the XIAO nRF52840.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * NOT a port of legacy/project/app/src/app_battery.c. That module was written for
 * a ~3.2 V cell:
 *
 *     #define BAT_MAX_VOLTAGE 3200
 *     voltageTable[]    = {3100, 3000, 2900, 2800, 2700, 2600, 2500, 2550, 2400};
 *     percentageTable[] = { 100,   90,   80,   70,   60,   50,   40,   30,   20};
 *
 * A single-cell LiPo is 4.2 V charged and must never be taken to 2.4 V, so that
 * curve cannot be reused -- it would report a full LiPo as off-scale and a flat
 * one as healthy. The scaling, the curve and the thresholds here are all new. See
 * MIGRATION_NOTES.md.
 *
 * (The legacy table also had a latent bug worth recording: 2500 precedes 2550, so
 * the search for "first entry below the measured voltage" could never reach the
 * 2550 mV row and 30% was unreachable.)
 */

#ifndef ORANGELINK_BATTERY_H_
#define ORANGELINK_BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Claim the ADC and the sense-enable GPIO, take a first reading. */
int battery_init(void);

/** @brief Last measured cell voltage in mV, or 0 before the first reading. */
uint16_t battery_millivolts(void);

/** @brief Last state of charge, 0-100, or 0xFF before the first reading. */
uint8_t battery_percent(void);

/** @brief Whether charge is below the red threshold. */
bool battery_is_low(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_BATTERY_H_ */
