/*
 * RileyLink APS command handler ("subg_rfspy 2.2") -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * See docs/aps-protocol-spec.md. Deviations from the legacy implementation are
 * called out inline and recorded in MIGRATION_NOTES.md.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include "aps.h"
#include "ble/ips.h"
#include "drivers/rf69/rf69.h"
#include "4b6b.h"
#include "manchester.h"

LOG_MODULE_REGISTER(aps, CONFIG_ORANGELINK_LOG_LEVEL);

/* ------------------------------------------------------------------------- *
 * Protocol constants
 * ------------------------------------------------------------------------- */

#define APS_SW_VERSION "subg_rfspy 2.2"
#define APS_STATE_OK   "OK"

/* Only the 916 MHz Minimed band is in scope. The legacy firmware also accepted
 * 866-870 (Minimed WWL) and 431-435 (Omnipod); those radios are not fitted.
 * An out-of-band frequency is logged and discarded, leaving the radio on its
 * previous setting -- legacy behaviour, preserved.
 */
#define APS_FREQ_916_MIN 914000000U
#define APS_FREQ_916_MAX 918000000U

/* RFM69 crystal used by the CC111x-compatible register encoding the host sends.
 * NOT the radio's own 32 MHz crystal -- see rf69.c.
 */
#define APS_RILEYLINK_FXOSC 24000000ULL

enum aps_cmd {
	CMD_GET_STATE       = 0x01,
	CMD_GET_VER         = 0x02,
	CMD_GET_PKT         = 0x03,
	CMD_SEND_PKT        = 0x04,
	CMD_SEND_AND_LISTEN = 0x05,
	CMD_UPDATE_REG      = 0x06,
	CMD_RESET           = 0x07,
	CMD_LED             = 0x08,
	CMD_READ_REG        = 0x09,
	CMD_SET_MODE_REG    = 0x0A,
	CMD_SET_SW_ENCODING = 0x0B,
	CMD_SET_PREAMBLE    = 0x0C,
	CMD_RESET_RADIO_CFG = 0x0D,
	CMD_GET_STATISTICS  = 0x0E,
};

enum aps_encoding {
	ENCODING_NONE = 0,
	ENCODING_MANCHESTER = 1,
	ENCODING_4B6B = 2,
};

struct aps_req {
	uint8_t cmd;
	int8_t rssi;
	uint16_t len;
	uint8_t param[APS_MAX_PARAM_LEN];
};

/* ------------------------------------------------------------------------- *
 * State
 * ------------------------------------------------------------------------- */

/*
 * Depth 1, matching legacy APS_CMD_QUEUE_SIZE 2 with a FIFO that reserves a slot.
 * A deeper queue would change observable behaviour: the legacy firmware dropped
 * commands that arrived while one was in flight.
 */
K_MSGQ_DEFINE(aps_msgq, sizeof(struct aps_req), 1, 4);

/*
 * DEDICATED THREAD, not the system workqueue.
 *
 * CMD_GET_PKT and CMD_SEND_AND_LISTEN block for a client-supplied timeout --
 * potentially seconds. On the system workqueue that would starve every other
 * work item; the legacy design got away with it only because of the nRF5
 * interrupt-priority arrangement. Priority is below the Bluetooth RX thread so
 * the BLE link keeps servicing ATT while the radio is listening.
 * See docs/aps-protocol-spec.md section 6.2.
 */
#define APS_THREAD_STACK_SIZE 2048
#define APS_THREAD_PRIORITY 7

static K_THREAD_STACK_DEFINE(aps_stack, APS_THREAD_STACK_SIZE);
static struct k_thread aps_thread_data;

static enum aps_encoding encoding = ENCODING_NONE;
static uint8_t freq_reg[3] = { 0x12, 0x14, 0x83 };  /* legacy default: 433.92 MHz */
static uint8_t use_pkt_len;
static bool active;

static uint32_t loop_count;   /* drives the statistics updTime field */
static uint16_t pkt_rx_count;
static uint16_t pkt_tx_count;

/* ------------------------------------------------------------------------- *
 * Response helpers
 * ------------------------------------------------------------------------- */

/* Bare status byte, e.g. an error code. */
static void respond_code(uint8_t code)
{
	ips_send_response(&code, 1);
}

/* Data-bearing response: [0xDD][payload...]. */
static void respond_data(const uint8_t *data, uint16_t len)
{
	uint8_t buf[APS_RESP_MAX_LEN];

	if (len > sizeof(buf) - 1) {
		LOG_WRN("response %u B truncated to %u", len, (unsigned)sizeof(buf) - 1);
		len = sizeof(buf) - 1;
	}

	buf[0] = APS_RESP_SUCCESS;
	memcpy(&buf[1], data, len);
	ips_send_response(buf, len + 1);
}

/* ------------------------------------------------------------------------- *
 * Encoding
 * ------------------------------------------------------------------------- */

static uint16_t encode(const uint8_t *src, uint8_t *dst, uint16_t len)
{
	switch (encoding) {
	case ENCODING_NONE:
		memcpy(dst, src, len);
		return len;
	case ENCODING_MANCHESTER:
		return encode_manchester(src, dst, len) ? len * 2 : 0;
	case ENCODING_4B6B:
		return encode_4b6b(src, dst, len);
	default:
		return 0;
	}
}

__maybe_unused static uint16_t decode(const uint8_t *src, uint8_t *dst, uint16_t len)
{
	switch (encoding) {
	case ENCODING_NONE:
		memcpy(dst, src, len);
		return len;
	case ENCODING_MANCHESTER:
		return decode_manchester(src, dst, len);
	case ENCODING_4B6B:
		return decode_4b6b(src, dst, len);
	default:
		return 0;
	}
}

/* ------------------------------------------------------------------------- *
 * Frequency
 * ------------------------------------------------------------------------- */

static uint32_t freq_from_regs(void)
{
	uint32_t reg = ((uint32_t)freq_reg[0] << 16) |
		       ((uint32_t)freq_reg[1] << 8) | freq_reg[2];

	return (uint32_t)(((uint64_t)reg * APS_RILEYLINK_FXOSC) >> 16);
}

static void apply_freq(void)
{
	uint32_t hz = freq_from_regs();

	if (hz < APS_FREQ_916_MIN || hz > APS_FREQ_916_MAX) {
		/* Legacy logged and discarded, leaving the radio unchanged. Kept --
		 * but note this build only carries the 916 MHz radio, so a host
		 * asking for 433 or 868 gets silently ignored rather than retuned.
		 */
		LOG_WRN("frequency %u Hz outside the 916 MHz band, ignored", hz);
		return;
	}

	LOG_INF("tuning to %u Hz", hz);
	if (rf69_set_freq(hz) != 0) {
		LOG_ERR("rf69_set_freq failed");
	}
}

/* ------------------------------------------------------------------------- *
 * Commands
 * ------------------------------------------------------------------------- */

static void cmd_get_state(void)
{
	respond_data((const uint8_t *)APS_STATE_OK, strlen(APS_STATE_OK));
}

static void cmd_get_version(void)
{
	respond_data((const uint8_t *)APS_SW_VERSION, strlen(APS_SW_VERSION));
}

static void cmd_set_sw_encoding(const uint8_t *p, uint16_t len)
{
	if (len < 1) {
		return;   /* legacy: no response on a short frame */
	}

	switch (p[0]) {
	case ENCODING_NONE:
	case ENCODING_MANCHESTER:
	case ENCODING_4B6B:
		encoding = (enum aps_encoding)p[0];
		LOG_INF("encoding set to %u", p[0]);
		respond_code(APS_RESP_SUCCESS);
		break;
	default:
		respond_code(APS_RESP_PARAM_ERROR);
		break;
	}
}

static void cmd_update_reg(const uint8_t *p, uint16_t len)
{
	uint8_t addr, value;

	/* AndroidAPS sends 2 bytes, Loop sends 10; only the first two are read.
	 * Legacy returned WITHOUT any response on a short frame -- preserved, so
	 * the client times out exactly as before.
	 */
	if (len < 2) {
		LOG_WRN("CMD_UPDATE_REG len %u < 2, no response", len);
		return;
	}

	addr = p[0];
	value = p[1];

	switch (addr) {
	case 0x02:
		use_pkt_len = 1;
		LOG_INF("fixed RX payload length = %u", value);
		break;
	case 0x09:
	case 0x0A:
	case 0x0B:
		freq_reg[addr - 0x09] = value;
		apply_freq();
		break;
	case 0x0C:
		/* Legacy switched to MINIMED_WWL (868 MHz) on 0x59. That radio is not
		 * fitted in this build, so the 916 configuration is simply reapplied.
		 */
		if (value == 0x59) {
			LOG_INF("host requested Minimed mode; reapplying 916 config");
			rf69_config_916();
		}
		break;
	default:
		LOG_DBG("CMD_UPDATE_REG addr 0x%02x ignored", addr);
		break;
	}

	respond_code(APS_RESP_SUCCESS);
}

static void cmd_read_reg(const uint8_t *p, uint16_t len)
{
	uint8_t value;

	if (len < 1) {
		LOG_WRN("CMD_READ_REG len 0, no response");
		return;
	}

	switch (p[0]) {
	case 0x09:
	case 0x0A:
	case 0x0B:
		value = freq_reg[p[0] - 0x09];
		break;
	default:
		/* Legacy stub: every other address returns 0x5A. Not a real read. */
		value = 0x5A;
		break;
	}

	respond_data(&value, 1);
}

static void cmd_set_preamble(const uint8_t *p, uint16_t len)
{
	if (len < 2) {
		return;
	}
	/* Big-endian on the wire. */
	LOG_INF("preamble = %u", sys_get_be16(p));
	respond_code(APS_RESP_SUCCESS);
}

static void cmd_reset_radio_cfg(void)
{
	rf69_config_916();
	encoding = ENCODING_NONE;
	respond_code(APS_RESP_SUCCESS);
}

static void cmd_get_statistics(void)
{
	/* 20 bytes, all multi-byte fields big-endian. Field order is part of the
	 * wire format -- see docs/aps-protocol-spec.md section 3.
	 */
	uint8_t buf[20] = { 0 };

	sys_put_be32(loop_count * 10, &buf[0]);   /* updTime, ms */
	sys_put_be16(0, &buf[4]);                 /* rxOverflowCnt      (always 0) */
	sys_put_be16(0, &buf[6]);                 /* rxFifoOverflowCnt  (always 0) */
	sys_put_be16(pkt_rx_count, &buf[8]);
	sys_put_be16(pkt_tx_count, &buf[10]);
	sys_put_be16(0, &buf[12]);                /* crcFailCnt         (always 0) */
	sys_put_be16(0, &buf[14]);                /* spiSyncFailCnt     (always 0) */
	sys_put_be16(0, &buf[16]);                /* placeholder0 */
	sys_put_be16(0, &buf[18]);                /* placeholder1 */

	respond_data(buf, sizeof(buf));
}

/*
 * The three radio commands need the sub-GHz packet path, which is not ported yet.
 *
 * They answer RX_TIMEOUT rather than staying silent: the wire format is already
 * correct, so a host sees a well-formed "nothing received" instead of hanging.
 * That is deliberately distinguishable from the finished behaviour -- the log line
 * says plainly that the path is missing, so a passing integration test cannot be
 * mistaken for working radio traffic.
 */
static void cmd_radio_not_ported(uint8_t cmd)
{
	LOG_WRN("cmd 0x%02x needs the sub-GHz packet path (not ported yet)", cmd);
	respond_code(APS_RESP_RX_TIMEOUT);
}

/* ------------------------------------------------------------------------- *
 * Dispatch
 * ------------------------------------------------------------------------- */

static void aps_dispatch(const struct aps_req *req)
{
	switch (req->cmd) {
	case CMD_GET_STATE:
		cmd_get_state();
		break;
	case CMD_GET_VER:
		cmd_get_version();
		break;
	case CMD_GET_PKT:
	case CMD_SEND_PKT:
	case CMD_SEND_AND_LISTEN:
		cmd_radio_not_ported(req->cmd);
		break;
	case CMD_UPDATE_REG:
		cmd_update_reg(req->param, req->len);
		break;
	case CMD_LED:
		/* Accepted and ignored, as in the legacy firmware. */
		respond_code(APS_RESP_SUCCESS);
		break;
	case CMD_READ_REG:
		cmd_read_reg(req->param, req->len);
		break;
	case CMD_SET_MODE_REG:
		/* Accepted and ignored. */
		respond_code(APS_RESP_SUCCESS);
		break;
	case CMD_SET_SW_ENCODING:
		cmd_set_sw_encoding(req->param, req->len);
		break;
	case CMD_SET_PREAMBLE:
		cmd_set_preamble(req->param, req->len);
		break;
	case CMD_RESET_RADIO_CFG:
		cmd_reset_radio_cfg();
		break;
	case CMD_GET_STATISTICS:
		cmd_get_statistics();
		break;
	case CMD_RESET:
		/* Legacy has NO case for 0x07: it falls through to default, is logged
		 * as unknown, and produces no response at all. The client times out.
		 * Reproduced deliberately -- changing it is a protocol change.
		 */
	default:
		LOG_WRN("unknown command 0x%02x (no response, as in legacy)", req->cmd);
		break;
	}
}

static void aps_thread_fn(void *a, void *b, void *c)
{
	struct aps_req req;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		if (k_msgq_get(&aps_msgq, &req, K_FOREVER) != 0) {
			continue;
		}
		loop_count++;
		aps_dispatch(&req);
	}
}

/* ------------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------------- */

void aps_put_cmd(const uint8_t *buf, uint16_t len, int8_t rssi)
{
	struct aps_req req;
	uint16_t param_len;

	if (buf == NULL || len == 0) {
		return;
	}

	/* Legacy self-consistency check: byte 0 counts everything after itself. */
	if (len == 1 || buf[0] != len - 1) {
		LOG_DBG("malformed frame (len %u, byte0 0x%02x), dropped", len, buf[0]);
		return;
	}

	param_len = len - 2;

	/*
	 * BOUNDS CHECK -- the fix for the overflow in docs/aps-protocol-spec.md
	 * section 7.
	 *
	 * Legacy Aps_PutCmd() did:
	 *     req.pktLen = len - 2;
	 *     memcpy(req.pkt, pBuf + 2, req.pktLen);
	 * with req.pkt only 123 bytes and len up to 150 from the Data
	 * characteristic -- a 25-byte stack overflow reachable over an
	 * unauthenticated BLE link.
	 */
	if (param_len > APS_MAX_PARAM_LEN) {
		LOG_WRN("frame params %u B exceed %u, rejected",
			param_len, APS_MAX_PARAM_LEN);
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	if (!active) {
		LOG_DBG("APS inactive, frame dropped");
		return;
	}

	req.cmd = buf[1];
	req.rssi = rssi;
	req.len = param_len;
	memcpy(req.param, &buf[2], param_len);

	/*
	 * DEVIATION: legacy executed CMD_UPDATE_REG synchronously here, inside the
	 * BLE callback, where it could drive the RFM69 over SPI while the command
	 * loop was mid-transaction on the same bus -- an unsynchronised race. It is
	 * queued like everything else now, so all radio access is serialised onto
	 * the APS thread. Costs up to one queue hop of latency.
	 * See MIGRATION_NOTES.md section 2.4.
	 */
	if (k_msgq_put(&aps_msgq, &req, K_NO_WAIT) != 0) {
		/* Legacy also dropped commands arriving while one was in flight. */
		LOG_WRN("queue full, command 0x%02x dropped", req.cmd);
	}
}

void aps_set_active(bool on)
{
	active = on;
	LOG_INF("APS %s", on ? "active" : "inactive");
}

void aps_init(void)
{
	k_thread_create(&aps_thread_data, aps_stack, K_THREAD_STACK_SIZEOF(aps_stack),
			aps_thread_fn, NULL, NULL, NULL,
			APS_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&aps_thread_data, "aps");
	LOG_INF("APS ready (%s)", APS_SW_VERSION);
}
