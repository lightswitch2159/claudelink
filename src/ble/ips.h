/*
 * Insulin Pump Service (IPS) -- Zephyr port of lib/pump/ble_services/ble_ips.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Every UUID, property, length and descriptor string here is reproduced exactly
 * from the legacy nRF5 SDK implementation. The companion mobile app depends on
 * all of it. See docs/gatt-service-spec.md before changing anything.
 */

#ifndef ORANGELINK_BLE_IPS_H_
#define ORANGELINK_BLE_IPS_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum Data characteristic payload.
 *
 * Legacy: BLE_IPS_MAX_DATA_CHAR_LEN = min(150, MTU - 3) with
 * NRF_SDH_BLE_GATT_MAX_MTU_SIZE = 247, so the 150 from BLE_RESPONSE_MAX_LEN
 * binds. Unlike the SoftDevice helper, Zephyr will not clamp for us -- every
 * write is bounds-checked against this explicitly.
 */
#define IPS_DATA_MAX_LEN      150

/* Legacy BLE_IPS_MAX_CUS_NAME_CHAR_LEN. Also caps the persisted device name. */
#define IPS_CUS_NAME_MAX_LEN  30

/* Read back by the app as the firmware version. NOT the APS protocol version,
 * which is the separate string "subg_rfspy 2.2" returned by CMD_GET_VER.
 */
#define IPS_FW_VERSION_STR    "ble_rfspy 2.0"

/** Events delivered to the application, mirroring ble_ips_evt_type_t. */
enum ips_evt_type {
	IPS_EVT_DATA_RX,      /* Write to Data        -> APS command */
	IPS_EVT_CUS_NAME_RX,  /* Write to Custom Name -> persist + disconnect */
	IPS_EVT_LED_MODE_RX,  /* Write to LED Mode    -> legacy handler is empty */
};

struct ips_evt {
	enum ips_evt_type type;
	const uint8_t *data;
	uint16_t len;
	int8_t rssi;          /* Valid for IPS_EVT_DATA_RX only. */
};

typedef void (*ips_evt_handler_t)(const struct ips_evt *evt);

/**
 * @brief Register the application event handler.
 *
 * The GATT service itself is registered statically at build time by
 * BT_GATT_SERVICE_DEFINE, so this only wires up the callback.
 */
void ips_init(ips_evt_handler_t handler);

/**
 * @brief Deliver a response to the peer.
 *
 * Reproduces Ble_IpsNotifyRespCntAndSendData(). The ordering is mandatory and
 * is what the mobile app relies on:
 *
 *   1. store @p data as the Data characteristic value
 *   2. increment Response Count and notify it
 *
 * The peer reads Data only after seeing the Response Count notification, so the
 * value must be in place first.
 *
 * @param data Response payload.
 * @param len  Length, clamped to IPS_DATA_MAX_LEN.
 * @return 0 on success, negative errno on failure.
 */
int ips_send_response(const uint8_t *data, uint16_t len);

/**
 * @brief Current persisted advertising name.
 *
 * Backing store for the Custom Name characteristic. Points at a buffer of at
 * most IPS_CUS_NAME_MAX_LEN bytes; not NUL-terminated.
 */
const uint8_t *ips_cus_name_get(uint16_t *len);

/** @brief Seed the Custom Name value at boot, from settings. */
void ips_cus_name_set(const uint8_t *name, uint16_t len);

/**
 * @brief Start the 60 s Timer Tick.
 *
 * Legacy BLE_TMR_TICK_ONE_MIN = 60000 ms, repeating. The counter increments,
 * wraps via `if (tick >= 0xFF) tick = 0` so it cycles 0x00..0xFE and never
 * takes the value 0xFF, and notifies only when the peer has subscribed.
 */
void ips_timer_tick_start(void);
void ips_timer_tick_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_BLE_IPS_H_ */
