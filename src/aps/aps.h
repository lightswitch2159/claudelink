/*
 * RileyLink APS command handler ("subg_rfspy 2.2") -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Zephyr port of legacy/project/app/src/app_aps.c. Wire format is documented in
 * docs/aps-protocol-spec.md and is reproduced exactly; the companion app depends
 * on it.
 */

#ifndef ORANGELINK_APS_H_
#define ORANGELINK_APS_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Response codes -- legacy values, unchanged. */
#define APS_RESP_SUCCESS         0xDD
#define APS_RESP_RX_TIMEOUT      0xAA
#define APS_RESP_CMD_INTERRUPTED 0xBB
#define APS_RESP_PARAM_ERROR     0x11
#define APS_RESP_UNKNOWN_COMMAND 0x22

/* Legacy APS_MAX_PARA_LEN(16) + SUBG_MAX_PKT_LEN(107). */
#define APS_MAX_PARAM_LEN 123

/* Legacy BLE_RESPONSE_MAX_LEN. */
#define APS_RESP_MAX_LEN 150

/** @brief Start the APS command thread. */
void aps_init(void);

/**
 * @brief Queue a command frame received on the IPS Data characteristic.
 *
 * Frame layout is [length][opcode][params...], where length counts everything
 * after itself. Malformed frames are dropped, matching legacy behaviour.
 *
 * @param buf  Frame bytes.
 * @param len  Frame length.
 * @param rssi Connection RSSI at the time of the write.
 */
void aps_put_cmd(const uint8_t *buf, uint16_t len, int8_t rssi);

/** @brief Enable/disable command processing, mirroring Aps_StartLoop/StopLoop. */
void aps_set_active(bool active);

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_APS_H_ */
