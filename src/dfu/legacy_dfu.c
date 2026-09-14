/*
 * Nordic legacy buttonless DFU trigger, for the Adafruit nRF52 bootloader.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This does NOT implement DFU. It implements the one thing the application has to
 * provide so that an over-the-air update can start without touching the board: a
 * GATT service whose control point, when written, reboots into the bootloader.
 *
 * The bootloader's side is already there and needs no button. From its main.c:
 *
 *     _ota_dfu = (gpregret == DFU_MAGIC_OTA_APPJUM) || (gpregret == DFU_MAGIC_OTA_RESET);
 *
 * so writing a magic byte to GPREGRET and resetting comes up straight in BLE OTA.
 * Adafruit's own BLEDfu uses DFU_MAGIC_OTA_APPJUM (0xB1) because it app-jumps with
 * the SoftDevice still running. We do a full system reset instead, after which
 * nothing is initialised, so the correct value here is DFU_MAGIC_OTA_RESET (0xA8) --
 * the bootloader keys _sd_inited off APPJUM specifically, and would be wrong about
 * our state if we claimed it.
 *
 * UUIDs, opcode and revision value are the legacy Nordic set, taken from Adafruit's
 * BLEDfu so that nRF Connect recognises this as an updatable application:
 *
 *     service        00001530-1212-EFDE-1523-785FEABCD123
 *     control point  00001531-...   write 0x01 (START_DFU), notify
 *     packet         00001532-...   present for discovery; unused in app mode
 *     revision       00001534-...   0x0001 = application mode
 *
 * Notifications must be subscribed before the control point is accepted, matching
 * Adafruit's CCCD check -- a client that cannot receive the response has no way to
 * know the jump was taken, and nRF Connect subscribes as a matter of course.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(legacy_dfu, CONFIG_ORANGELINK_LOG_LEVEL);

#define DFU_MAGIC_OTA_RESET 0xA8
#define DFU_OP_START        0x01

#define UUID_DFU_SVC BT_UUID_DECLARE_128( \
	BT_UUID_128_ENCODE(0x00001530, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123))
#define UUID_DFU_CONTROL BT_UUID_DECLARE_128( \
	BT_UUID_128_ENCODE(0x00001531, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123))
#define UUID_DFU_PACKET BT_UUID_DECLARE_128( \
	BT_UUID_128_ENCODE(0x00001532, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123))
#define UUID_DFU_REVISION BT_UUID_DECLARE_128( \
	BT_UUID_128_ENCODE(0x00001534, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123))

static const uint16_t dfu_revision = 0x0001;   /* application mode */
static bool notify_on;

static void reboot_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_fn);

static void reboot_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("rebooting into the bootloader for OTA");
	nrf_power_gpregret_set(NRF_POWER, 0, DFU_MAGIC_OTA_RESET);
	sys_reboot(SYS_REBOOT_COLD);
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	notify_on = (value == BT_GATT_CCC_NOTIFY);
}

static ssize_t control_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset,
			     uint8_t flags)
{
	const uint8_t *p = buf;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (!notify_on) {
		/* Same refusal Adafruit's BLEDfu gives: without a subscription the
		 * client cannot be told the jump was accepted.
		 */
		return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
	}
	if (p[0] != DFU_OP_START) {
		LOG_WRN("DFU control: unexpected opcode 0x%02x", p[0]);
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	/*
	 * Deferred, not immediate. The write response still has to go out, and the
	 * link wants tearing down cleanly -- resetting inside the callback drops both
	 * and the client reports a failure for an update that is actually starting.
	 */
	LOG_INF("DFU requested over BLE");
	k_work_reschedule(&reboot_work, K_MSEC(500));

	return len;
}

static ssize_t revision_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &dfu_revision,
				 sizeof(dfu_revision));
}

BT_GATT_SERVICE_DEFINE(legacy_dfu_svc,
	BT_GATT_PRIMARY_SERVICE(UUID_DFU_SVC),
	BT_GATT_CHARACTERISTIC(UUID_DFU_CONTROL,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, control_write, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(UUID_DFU_PACKET,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, NULL, NULL),
	BT_GATT_CHARACTERISTIC(UUID_DFU_REVISION,
			       BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       revision_read, NULL, NULL),
);
