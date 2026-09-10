/*
 * Insulin Pump Service (IPS) -- Zephyr port of lib/pump/ble_services/ble_ips.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * UUIDs transcribed from the legacy little-endian byte arrays in ble_ips.c.
 * Note that Response Count and Timer Tick share the 16-bit alias 0x7910 and
 * differ only in the full 128-bit value -- anything keyed off 16-bit aliases
 * will break. See docs/gatt-service-spec.md.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "ips.h"

LOG_MODULE_REGISTER(ips, CONFIG_ORANGELINK_LOG_LEVEL);

/* ------------------------------------------------------------------------- *
 * UUIDs -- exact, from the legacy source
 * ------------------------------------------------------------------------- */

/* 0235733b-99c5-4197-b856-69219c2a3845 (16-bit alias 0x733B) */
#define IPS_UUID_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x0235733b, 0x99c5, 0x4197, 0xb856, 0x69219c2a3845)

/* c842e849-5028-42e2-867c-016adada9155 (alias 0xE849) */
#define IPS_UUID_DATA_VAL \
	BT_UUID_128_ENCODE(0xc842e849, 0x5028, 0x42e2, 0x867c, 0x016adada9155)

/* 6e6c7910-b89e-43a5-a0fe-50c5e2b81f4a (alias 0x7910) */
#define IPS_UUID_RES_CNT_VAL \
	BT_UUID_128_ENCODE(0x6e6c7910, 0xb89e, 0x43a5, 0xa0fe, 0x50c5e2b81f4a)

/* 6e6c7910-b89e-43a5-78af-50c5e2b86f7e (alias 0x7910 -- same as above) */
#define IPS_UUID_TMR_TICK_VAL \
	BT_UUID_128_ENCODE(0x6e6c7910, 0xb89e, 0x43a5, 0x78af, 0x50c5e2b86f7e)

/* d93b2af0-1e28-11e4-8c21-0800200c9a66 (alias 0x2AF0) */
#define IPS_UUID_CUS_NAME_VAL \
	BT_UUID_128_ENCODE(0xd93b2af0, 0x1e28, 0x11e4, 0x8c21, 0x0800200c9a66)

/* 30d99dc9-7c91-4295-a051-0a104d238cf2 (alias 0x9DC9) */
#define IPS_UUID_FW_VER_VAL \
	BT_UUID_128_ENCODE(0x30d99dc9, 0x7c91, 0x4295, 0xa051, 0x0a104d238cf2)

/* c6d84241-f1a7-4f9c-a25f-fce16732f14e (alias 0x4241) */
#define IPS_UUID_LED_MODE_VAL \
	BT_UUID_128_ENCODE(0xc6d84241, 0xf1a7, 0x4f9c, 0xa25f, 0xfce16732f14e)

static const struct bt_uuid_128 ips_uuid_service  = BT_UUID_INIT_128(IPS_UUID_SERVICE_VAL);
static const struct bt_uuid_128 ips_uuid_data     = BT_UUID_INIT_128(IPS_UUID_DATA_VAL);
static const struct bt_uuid_128 ips_uuid_res_cnt  = BT_UUID_INIT_128(IPS_UUID_RES_CNT_VAL);
static const struct bt_uuid_128 ips_uuid_tmr_tick = BT_UUID_INIT_128(IPS_UUID_TMR_TICK_VAL);
static const struct bt_uuid_128 ips_uuid_cus_name = BT_UUID_INIT_128(IPS_UUID_CUS_NAME_VAL);
static const struct bt_uuid_128 ips_uuid_fw_ver   = BT_UUID_INIT_128(IPS_UUID_FW_VER_VAL);
static const struct bt_uuid_128 ips_uuid_led_mode = BT_UUID_INIT_128(IPS_UUID_LED_MODE_VAL);

/* Characteristic User Description strings -- exact. The app may read these. */
static const char desc_data[]     = "Data";
static const char desc_res_cnt[]  = "Response Count";
static const char desc_tmr_tick[] = "Timer Tick";
static const char desc_cus_name[] = "Custom Name";
static const char desc_fw_ver[]   = "Version";
static const char desc_led_mode[] = "LED Mode";

/* ------------------------------------------------------------------------- *
 * State
 * ------------------------------------------------------------------------- */

#define IPS_TIMER_TICK_INTERVAL_MS 60000   /* legacy BLE_TMR_TICK_ONE_MIN */

static ips_evt_handler_t app_handler;

static uint8_t data_value[IPS_DATA_MAX_LEN];
static uint16_t data_len;

static uint8_t cus_name[IPS_CUS_NAME_MAX_LEN];
static uint16_t cus_name_len;

static uint8_t response_count;
static uint8_t timer_tick;
static uint8_t led_mode;

static bool res_cnt_subscribed;
static bool tmr_tick_subscribed;

static void timer_tick_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(timer_tick_work, timer_tick_work_fn);

/* Attribute indices into the service below. Kept as named constants because
 * bt_gatt_notify() needs the value attribute, which sits one past its
 * declaration. Verified by ips_attr_sanity_check() at init.
 */
#define ATTR_IDX_DATA_VALUE      2
#define ATTR_IDX_RES_CNT_VALUE   5
#define ATTR_IDX_TMR_TICK_VALUE  9

/* ------------------------------------------------------------------------- *
 * Characteristic access callbacks
 * ------------------------------------------------------------------------- */

static ssize_t read_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, data_value, data_len);
}

static ssize_t write_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	struct ips_evt evt;
	int8_t rssi = 0;

	/*
	 * Bounds check both len and offset.
	 *
	 * The legacy firmware did neither, which is the remotely reachable stack
	 * overflow in Aps_PutCmd() -- see docs/aps-protocol-spec.md section 7.
	 * Offset is a second, independent vector that the SoftDevice helper used
	 * to absorb; Zephyr hands it to us raw.
	 */
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len > IPS_DATA_MAX_LEN) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(data_value, buf, len);
	data_len = len;

	/* Legacy attached the live connection RSSI to every DATA_RX event.
	 * Zephyr has no direct equivalent; an HCI Read RSSI is needed. Wired up
	 * with the connection layer in a later step -- 0 until then.
	 */
	evt.type = IPS_EVT_DATA_RX;
	evt.data = data_value;
	evt.len = len;
	evt.rssi = rssi;

	if (app_handler) {
		app_handler(&evt);
	}

	return len;
}

static ssize_t read_res_cnt(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &response_count, sizeof(response_count));
}

static ssize_t read_tmr_tick(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &timer_tick, sizeof(timer_tick));
}

static ssize_t read_cus_name(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, cus_name, cus_name_len);
}

static ssize_t write_cus_name(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	struct ips_evt evt;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len == 0 || len > IPS_CUS_NAME_MAX_LEN) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(cus_name, buf, len);
	cus_name_len = len;

	/*
	 * The legacy handler persisted the name, then DELIBERATELY DISCONNECTED
	 * so advertising could restart under the new name, reporting HCI reason
	 * 0x3B (BT_HCI_ERR_UNACCEPT_CONN_PARAM). The mobile app almost certainly
	 * special-cases that, so both the disconnect and the odd reason code are
	 * reproduced by the application handler, not here.
	 */
	evt.type = IPS_EVT_CUS_NAME_RX;
	evt.data = cus_name;
	evt.len = len;
	evt.rssi = 0;

	if (app_handler) {
		app_handler(&evt);
	}

	return len;
}

static ssize_t read_fw_ver(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 IPS_FW_VERSION_STR, sizeof(IPS_FW_VERSION_STR) - 1);
}

static ssize_t read_led_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &led_mode, sizeof(led_mode));
}

static ssize_t write_led_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	struct ips_evt evt;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != sizeof(led_mode)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	led_mode = *(const uint8_t *)buf;

	/* Legacy BLE_IPS_EVT_LED_MODE_RX handler is empty: the write is accepted
	 * and ignored. Preserved -- the app must not see an error.
	 */
	evt.type = IPS_EVT_LED_MODE_RX;
	evt.data = &led_mode;
	evt.len = sizeof(led_mode);
	evt.rssi = 0;

	if (app_handler) {
		app_handler(&evt);
	}

	return len;
}

static void res_cnt_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	res_cnt_subscribed = (value == BT_GATT_CCC_NOTIFY);
	LOG_DBG("Response Count notifications %s", res_cnt_subscribed ? "on" : "off");
}

static void tmr_tick_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	tmr_tick_subscribed = (value == BT_GATT_CCC_NOTIFY);
	LOG_DBG("Timer Tick notifications %s", tmr_tick_subscribed ? "on" : "off");
}

/* ------------------------------------------------------------------------- *
 * Service definition
 *
 * Order matters: it determines the handle layout the mobile app discovers.
 * Legacy order was Data, Response Count, Timer Tick, Custom Name, Version,
 * LED Mode -- preserved exactly.
 *
 * Permissions are plain READ/WRITE with no _ENCRYPT or _AUTHEN: the legacy
 * service used SEC_OPEN throughout and the device requires no pairing. That is
 * a compatibility requirement, which is precisely why every write callback
 * above bounds-checks its input.
 * ------------------------------------------------------------------------- */

BT_GATT_SERVICE_DEFINE(ips_svc,
	BT_GATT_PRIMARY_SERVICE(&ips_uuid_service),

	/* [1][2] Data -- read + write, variable length, max 150 */
	BT_GATT_CHARACTERISTIC(&ips_uuid_data.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_data, write_data, NULL),
	BT_GATT_CUD(desc_data, BT_GATT_PERM_READ),

	/* [4][5] Response Count -- read + notify, 1 byte */
	BT_GATT_CHARACTERISTIC(&ips_uuid_res_cnt.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       read_res_cnt, NULL, NULL),
	BT_GATT_CCC(res_cnt_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD(desc_res_cnt, BT_GATT_PERM_READ),

	/* [8][9] Timer Tick -- read + notify, 1 byte */
	BT_GATT_CHARACTERISTIC(&ips_uuid_tmr_tick.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       read_tmr_tick, NULL, NULL),
	BT_GATT_CCC(tmr_tick_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CUD(desc_tmr_tick, BT_GATT_PERM_READ),

	/* Custom Name -- read + write, variable length, max 30 */
	BT_GATT_CHARACTERISTIC(&ips_uuid_cus_name.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_cus_name, write_cus_name, NULL),
	BT_GATT_CUD(desc_cus_name, BT_GATT_PERM_READ),

	/* Version -- read only, fixed "ble_rfspy 2.0" */
	BT_GATT_CHARACTERISTIC(&ips_uuid_fw_ver.uuid,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ,
			       read_fw_ver, NULL, NULL),
	BT_GATT_CUD(desc_fw_ver, BT_GATT_PERM_READ),

	/* LED Mode -- read + write, 1 byte, accepted and ignored */
	BT_GATT_CHARACTERISTIC(&ips_uuid_led_mode.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_led_mode, write_led_mode, NULL),
	BT_GATT_CUD(desc_led_mode, BT_GATT_PERM_READ),
);

/* ------------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------------- */

int ips_send_response(const uint8_t *data, uint16_t len)
{
	int err;

	if (data == NULL) {
		return -EINVAL;
	}
	if (len > IPS_DATA_MAX_LEN) {
		LOG_WRN("response %u B clamped to %u", len, IPS_DATA_MAX_LEN);
		len = IPS_DATA_MAX_LEN;
	}

	/* Step 1: the value must be in place BEFORE the notification, because the
	 * peer reads Data in response to it.
	 */
	memcpy(data_value, data, len);
	data_len = len;

	/* Step 2: advance the counter and notify.
	 *
	 * Legacy wrap: `if (count >= 0xFF) count = 0`, so the sequence cycles
	 * 0x00..0xFE and never takes the value 0xFF. The counter advances whether
	 * or not the notification is delivered, matching the legacy ordering where
	 * the increment happened at the top of ble_ips_response_cnt_notify().
	 */
	response_count++;
	if (response_count >= 0xFF) {
		response_count = 0;
	}

	if (!res_cnt_subscribed) {
		return 0;
	}

	err = bt_gatt_notify(NULL, &ips_svc.attrs[ATTR_IDX_RES_CNT_VALUE],
			     &response_count, sizeof(response_count));
	if (err) {
		LOG_WRN("Response Count notify failed (%d)", err);
	}

	return err;
}

const uint8_t *ips_cus_name_get(uint16_t *len)
{
	if (len) {
		*len = cus_name_len;
	}
	return cus_name;
}

void ips_cus_name_set(const uint8_t *name, uint16_t len)
{
	if (name == NULL || len == 0) {
		return;
	}
	if (len > IPS_CUS_NAME_MAX_LEN) {
		len = IPS_CUS_NAME_MAX_LEN;
	}
	memcpy(cus_name, name, len);
	cus_name_len = len;
}

static void timer_tick_work_fn(struct k_work *work)
{
	int err;

	timer_tick++;
	if (timer_tick >= 0xFF) {
		timer_tick = 0;
	}

	if (tmr_tick_subscribed) {
		err = bt_gatt_notify(NULL, &ips_svc.attrs[ATTR_IDX_TMR_TICK_VALUE],
				     &timer_tick, sizeof(timer_tick));
		if (err) {
			LOG_WRN("Timer Tick notify failed (%d)", err);
		}
	}

	k_work_reschedule(&timer_tick_work, K_MSEC(IPS_TIMER_TICK_INTERVAL_MS));
}

void ips_timer_tick_start(void)
{
	k_work_reschedule(&timer_tick_work, K_MSEC(IPS_TIMER_TICK_INTERVAL_MS));
}

void ips_timer_tick_stop(void)
{
	k_work_cancel_delayable(&timer_tick_work);
}

/*
 * The notify calls index ips_svc.attrs directly, so a reordering of the service
 * definition would silently notify the wrong attribute. Catch that at init
 * rather than in the field.
 */
static void ips_attr_sanity_check(void)
{
	__ASSERT(bt_uuid_cmp(ips_svc.attrs[ATTR_IDX_DATA_VALUE].uuid,
			     &ips_uuid_data.uuid) == 0,
		 "Data value attribute index moved");
	__ASSERT(bt_uuid_cmp(ips_svc.attrs[ATTR_IDX_RES_CNT_VALUE].uuid,
			     &ips_uuid_res_cnt.uuid) == 0,
		 "Response Count value attribute index moved");
	__ASSERT(bt_uuid_cmp(ips_svc.attrs[ATTR_IDX_TMR_TICK_VALUE].uuid,
			     &ips_uuid_tmr_tick.uuid) == 0,
		 "Timer Tick value attribute index moved");
}

void ips_init(ips_evt_handler_t handler)
{
	app_handler = handler;

	response_count = 0;
	timer_tick = 0;
	data_len = 0;
	led_mode = 0;

	ips_attr_sanity_check();

	LOG_INF("IPS ready (Data max %u B, name max %u B)",
		IPS_DATA_MAX_LEN, IPS_CUS_NAME_MAX_LEN);
}
