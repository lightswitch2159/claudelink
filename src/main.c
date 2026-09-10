/*
 * Orangelink -- sub-GHz to BLE bridge (RileyLink-compatible).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Phase 2 milestone: BLE bring-up. Advertising and the IPS GATT service match
 * the legacy firmware exactly (docs/gatt-service-spec.md). The APS command
 * handler, sub-GHz state machine and RFM69 driver are not wired up yet.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include "ble/ips.h"
#include "drivers/rf69/rf69.h"
#include "aps/aps.h"
#include "subg/subg.h"

LOG_MODULE_REGISTER(main, CONFIG_ORANGELINK_LOG_LEVEL);

/* ------------------------------------------------------------------------- *
 * Advertising -- legacy parameters
 *
 *   BLE_ADV_INTERVAL 480 units x 0.625 ms = 300 ms
 *     (the legacy source comment claims 187.5 ms and is simply wrong)
 *   BLE_ADV_DURATION 0 = advertise forever
 * ------------------------------------------------------------------------- */

#define ORANGELINK_ADV_INTERVAL 480

/* Default names from boards/bd_xh601_config.h and bd_xh_5102_config.h.
 * Overridden at boot by the persisted Custom Name once settings are wired up.
 */
#define ORANGELINK_DEFAULT_NAME CONFIG_BT_DEVICE_NAME

/* 0235733b-99c5-4197-b856-69219c2a3845 -- advertised as a complete 128-bit list,
 * matching the legacy advdata.uuids_complete.
 */
static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      BT_UUID_128_ENCODE(0x0235733b, 0x99c5, 0x4197,
					 0xb856, 0x69219c2a3845)),
};

/*
 * DEVIATION: the name goes in the scan response, not the advertising payload.
 *
 * The legacy code asked for BLE_ADVDATA_FULL_NAME in both advdata and srdata. A
 * 128-bit UUID (18 B) plus flags (3 B) leaves only 10 B of the 31 B payload,
 * i.e. 8 characters -- so "Orange" (6) fitted but "OrangePro" (9) could not, and
 * the two boards behaved differently. Because of that the companion app cannot
 * have depended on the name being in the advertising packet.
 *
 * Putting it in the scan response always fits and is what the legacy srdata
 * request implied. Standard central APIs merge the two. Tracked in
 * MIGRATION_NOTES.md; verify against the app.
 */
static const struct bt_data scan_rsp[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, ORANGELINK_DEFAULT_NAME,
		sizeof(ORANGELINK_DEFAULT_NAME) - 1),
};

/* Continuous, connectable, undirected. Interval fixed at 300 ms. */
static const struct bt_le_adv_param *adv_param =
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
			ORANGELINK_ADV_INTERVAL,
			ORANGELINK_ADV_INTERVAL,
			NULL);

static struct bt_conn *current_conn;

/* Set when a Custom Name write asks for a disconnect so advertising can restart
 * under the new name. Mirrors the legacy bleNameChangeFlg.
 */
static bool name_change_pending;

static int advertising_start(void)
{
	int err = bt_le_adv_start(adv_param, adv_data, ARRAY_SIZE(adv_data),
				  scan_rsp, ARRAY_SIZE(scan_rsp));
	if (err == -EALREADY) {
		LOG_DBG("advertising already active");
		return 0;
	}
	if (err) {
		LOG_ERR("advertising start failed (%d)", err);
		return err;
	}

	LOG_INF("advertising as \"%s\" at %u ms",
		bt_get_name(), (ORANGELINK_ADV_INTERVAL * 625) / 1000);
	return 0;
}

/*
 * Advertising must NOT be restarted from inside the disconnected callback.
 *
 * Found on hardware: after the first disconnect the device stopped advertising
 * entirely and had to be reset. Calling bt_le_adv_start() from the callback runs
 * while the connection object is still being torn down and fails. Deferring to
 * the system workqueue lets the teardown complete first.
 */
static void adv_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	advertising_start();
}
static K_WORK_DEFINE(adv_work, adv_work_fn);

/* ------------------------------------------------------------------------- *
 * Connection handling
 * ------------------------------------------------------------------------- */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("connection failed (0x%02x)", err);
		return;
	}

	current_conn = bt_conn_ref(conn);
	LOG_INF("connected");

	/* Legacy started the APS and config command loops plus the battery timer
	 * here. Aps_StartLoop() gated command processing on an active connection;
	 * preserved so frames outside a connection are dropped as before.
	 */
	aps_set_active(true);
	ips_timer_tick_start();
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("disconnected (reason 0x%02x)", reason);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	aps_set_active(false);
	ips_timer_tick_stop();

	if (name_change_pending) {
		name_change_pending = false;
		/* Re-apply the new name before advertising resumes. */
		uint16_t len;
		const uint8_t *name = ips_cus_name_get(&len);
		char buf[IPS_CUS_NAME_MAX_LEN + 1];

		len = MIN(len, IPS_CUS_NAME_MAX_LEN);
		memcpy(buf, name, len);
		buf[len] = '\0';

		if (bt_set_name(buf)) {
			LOG_WRN("bt_set_name(\"%s\") failed", buf);
		} else {
			LOG_INF("device name is now \"%s\"", buf);
		}
	}

	/* Deferred deliberately -- see adv_work_fn(). */
	k_work_submit(&adv_work);
}

static void on_le_param_updated(struct bt_conn *conn, uint16_t interval,
				uint16_t latency, uint16_t timeout)
{
	LOG_INF("conn params updated: interval %u, latency %u, timeout %u",
		interval, latency, timeout);
}

static void on_le_phy_updated(struct bt_conn *conn,
			      struct bt_conn_le_phy_info *param)
{
	/* Legacy only ever RESPONDED to a peer PHY request, answering PHY_AUTO --
	 * it never initiated one. Support is enabled; the central drives it.
	 * Actively requesting 2M here would be a behaviour change, not a port.
	 */
	LOG_INF("PHY updated: tx %u, rx %u", param->tx_phy, param->rx_phy);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.le_param_updated = on_le_param_updated,
	.le_phy_updated = on_le_phy_updated,
};

/* ------------------------------------------------------------------------- *
 * IPS events
 * ------------------------------------------------------------------------- */

static void ips_event_handler(const struct ips_evt *evt)
{
	switch (evt->type) {
	case IPS_EVT_DATA_RX:
		LOG_HEXDUMP_DBG(evt->data, evt->len, "Data write");
		aps_put_cmd(evt->data, evt->len, evt->rssi);
		break;

	case IPS_EVT_CUS_NAME_RX:
		LOG_INF("custom name write, %u B", evt->len);
		/*
		 * Legacy behaviour, preserved exactly: persist the name, then
		 * disconnect so advertising restarts under it, reporting HCI reason
		 * 0x3B. The app very likely special-cases that reason code.
		 *
		 * TODO(phase4): persist via the settings subsystem.
		 */
		name_change_pending = true;
		if (current_conn) {
			bt_conn_disconnect(current_conn,
					   BT_HCI_ERR_UNACCEPT_CONN_PARAM);
		}
		break;

	case IPS_EVT_LED_MODE_RX:
		/* Accepted and ignored, as in the legacy firmware. */
		LOG_DBG("LED mode write: 0x%02x (ignored)", evt->data[0]);
		break;
	}
}

/* ------------------------------------------------------------------------- *
 * RFM69 self-test
 *
 * Run at boot, then re-run every 5 s for as long as it fails. That way the radio
 * can be wired up (or fixed) with the board already powered and the result is
 * visible in the log without a reflash. Once it passes, the retry stops.
 * ------------------------------------------------------------------------- */

#define RF69_RETEST_INTERVAL_MS 5000

static void rf69_check_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(rf69_check_work, rf69_check_fn);

static void rf69_check_fn(struct k_work *work)
{
	static bool passed_once;
	static unsigned int attempt;
	struct rf69_selftest r;

	ARG_UNUSED(work);
	attempt++;

	if (rf69_selftest_run(&r) == 0) {
		if (!passed_once) {
			LOG_INF("RFM69 self-test passed on attempt %u", attempt);
			rf69_selftest_report(&r);
			passed_once = true;

			/* Radio confirmed good: bring up the packet path and run the
			 * RF-free loopback so the FIFO and encoding chain are verified
			 * before any pump traffic is attempted.
			 */
			subg_init();
			struct subg_loopback lb;
			subg_loopback_run(&lb);
			subg_loopback_report(&lb);
		}
		return;   /* stop retrying */
	}

	/* Full report on the first failure; a one-line reminder afterwards so the
	 * log stays readable while waiting for the module to be connected.
	 */
	if (attempt == 1) {
		rf69_selftest_report(&r);
		LOG_WRN("retrying every %u ms until the RFM69 responds",
			RF69_RETEST_INTERVAL_MS);
	} else {
		LOG_WRN("RFM69 still not ready (attempt %u, REG_VERSION=0x%02x)",
			attempt, r.version);
	}

	k_work_reschedule(&rf69_check_work, K_MSEC(RF69_RETEST_INTERVAL_MS));
}

/* ------------------------------------------------------------------------- *
 * Entry point
 * ------------------------------------------------------------------------- */

int main(void)
{
	int err;

	LOG_INF("Orangelink NCS starting (%s)", CONFIG_BOARD_TARGET);

	ips_init(ips_event_handler);
	ips_cus_name_set((const uint8_t *)ORANGELINK_DEFAULT_NAME,
			 sizeof(ORANGELINK_DEFAULT_NAME) - 1);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	LOG_INF("Bluetooth initialised");

	err = advertising_start();
	if (err) {
		return err;
	}

	/* Sub-GHz radio. Failure is not fatal: BLE stays up so the device remains
	 * reachable and the self-test keeps retrying in the background.
	 */
	if (rf69_init() != 0) {
		LOG_ERR("RFM69 SPI bus unavailable");
	}
	k_work_schedule(&rf69_check_work, K_NO_WAIT);

	aps_init();

	/*
	 * Nothing to do in the main thread. The legacy super-loop called
	 * nrf_pwr_mgmt_run(); under Zephyr the idle thread handles low power, so
	 * main simply returns and the kernel keeps running.
	 */
	return 0;
}
