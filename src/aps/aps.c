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
#include "subg/subg.h"
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
 * Depth 1, as in the legacy firmware -- and the depth matters.
 *
 * A deeper queue looks like an improvement and is not: AndroidAPS fires several
 * CMD_SEND_AND_LISTEN commands back to back, and since each queued radio command
 * preempts the one in flight, accepting them all means every listen is aborted
 * by its successor and none ever hears anything. With one slot the extras are
 * refused and the in-flight command runs its full window, which is what the
 * legacy design did and why it worked.
 *
 * Register writes are not affected because CMD_UPDATE_REG never enters the queue
 * -- see aps_put_cmd().
 */
K_MSGQ_DEFINE(aps_msgq, sizeof(struct aps_req), 4, 4);

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

/*
 * Set when the host changes a frequency register; applied to the radio just
 * before the next radio command, never from the BLE callback.
 *
 * CMD_UPDATE_REG runs inline so it cannot be dropped behind a long listen. But
 * applying it inline retuned the radio DURING a transmit: a 201-frame wakeup
 * burst takes ~3.9 s, AndroidAPS writes three frequency registers inside that
 * window, and the burst ended up scattered across three frequencies. A sleeping
 * pump never hears a coherent burst on any of them, so it never wakes.
 *
 * Deferring the write also removes the SPI race that made me queue this command
 * in the first place: the BLE callback now touches no radio register at all.
 */
static bool freq_pending;
static uint8_t use_pkt_len;
static bool active;

static uint32_t loop_count;   /* drives the statistics updTime field */

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

static uint16_t decode(const uint8_t *src, uint8_t *dst, uint16_t len)
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

/* Called from the APS thread only, immediately before a radio command. */
static void apply_pending_freq(void)
{
	uint32_t hz;

	if (!freq_pending) {
		return;
	}
	freq_pending = false;

	hz = freq_from_regs();

	if (hz < APS_FREQ_916_MIN || hz > APS_FREQ_916_MAX) {
		/* Legacy logged and discarded, leaving the radio unchanged. Kept --
		 * but note this build only carries the 916 MHz radio, so a host
		 * asking for 433 or 868 gets silently ignored rather than retuned.
		 */
		LOG_WRN("frequency %u Hz outside the 916 MHz band, ignored", hz);
		return;
	}

	if (rf69_set_freq(hz) != 0) {
		LOG_ERR("rf69_set_freq failed");
		return;
	}

	/*
	 * Read the frequency back rather than trusting the write.
	 *
	 * A HackRF capture showed the radio transmitting on 916.5477 MHz -- the
	 * config-table default -- while this function was logging every frequency
	 * AndroidAPS asked for. The tune was being computed and logged but never
	 * taking effect on the chip, so every mmtune candidate behaved identically
	 * and the scan could never converge.
	 */
	{
		uint32_t actual = rf69_get_freq();
		int32_t err_hz = (int32_t)(actual - hz);

		if (err_hz > 5000 || err_hz < -5000) {
			LOG_ERR("tune FAILED: asked %u Hz, radio reads %u Hz (%+d)",
				hz, actual, err_hz);
		} else {
			LOG_INF("tuned to %u Hz (radio reads %u)", hz, actual);
		}
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
		/* Record only. Applied by the APS thread before the next radio
		 * command, so a change can never land mid-burst.
		 */
		freq_reg[addr - 0x09] = value;
		freq_pending = true;
		break;
	case 0x0C:
		/* Legacy switched to MINIMED_WWL (868 MHz) on 0x59. That radio is not
		 * fitted here, so the 916 configuration is reapplied instead -- and
		 * deferred, for the same reason as the frequency registers.
		 */
		if (value == 0x59) {
			LOG_INF("host requested Minimed mode; will reapply 916 config");
			freq_pending = true;
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
	sys_put_be16(subg_get_rx_count(), &buf[8]);
	sys_put_be16(subg_get_tx_count(), &buf[10]);
	sys_put_be16(0, &buf[12]);                /* crcFailCnt         (always 0) */
	sys_put_be16(0, &buf[14]);                /* spiSyncFailCnt     (always 0) */
	sys_put_be16(0, &buf[16]);                /* placeholder0 */
	sys_put_be16(0, &buf[18]);                /* placeholder1 */

	respond_data(buf, sizeof(buf));
}

/* ------------------------------------------------------------------------- *
 * Radio commands
 *
 * Parameters are parsed with sys_get_be16/32 rather than by overlaying a packed
 * struct and byte-swapping in place. The legacy code took the address of unaligned
 * packed members and cast them, which is undefined behaviour that GCC warns about
 * and may compile to an aligned load. See docs/aps-protocol-spec.md section 6.3.
 * ------------------------------------------------------------------------- */

/* RileyLink clients expect CC111x-style RSSI. Legacy formula, truncation included. */
static uint8_t rssi_to_cc111x(int16_t dbm)
{
	return (uint8_t)((dbm + 73) * 2);
}

/* Emit [0xDD][rssi][pktCnt][payload...] for a received packet. */
static void respond_rx_packet(const uint8_t *pkt, uint16_t len)
{
	uint8_t buf[2 + SUBG_MAX_PKT_LEN];

	if (len > SUBG_MAX_PKT_LEN) {
		len = SUBG_MAX_PKT_LEN;
	}

	buf[0] = rssi_to_cc111x(subg_get_last_rssi());
	buf[1] = (uint8_t)(subg_get_rx_count() & 0xFF);
	memcpy(&buf[2], pkt, len);

	respond_data(buf, len + 2);
}

static void respond_rx_status(enum subg_rx_status st, const uint8_t *pkt, uint8_t len)
{
	switch (st) {
	case SUBG_RX_OK:
		respond_rx_packet(pkt, len);
		break;
	case SUBG_RX_TIMEOUT:
		respond_code(APS_RESP_RX_TIMEOUT);
		break;
	case SUBG_RX_INTERRUPTED:
		respond_code(APS_RESP_CMD_INTERRUPTED);
		break;
	}
}

/* CMD_GET_PKT: [listenChan][listenTimeout BE32] */
static void cmd_get_pkt(const uint8_t *p, uint16_t len)
{
	apply_pending_freq();
	uint8_t raw[SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t dec[SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t raw_len = 0;
	uint16_t dec_len;
	uint32_t timeout;
	enum subg_rx_status st;

	if (len < 5) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	timeout = sys_get_be32(&p[1]);   /* p[0] is listenChan, accepted and unused */

	st = subg_get_pkt(raw, &raw_len, timeout);
	if (st != SUBG_RX_OK) {
		LOG_INF("send+listen: no reply (status %d)", st);
		respond_rx_status(st, NULL, 0);
		return;
	}

	dec_len = decode(raw, dec, raw_len);
	LOG_INF("send+listen: REPLY %u B raw -> %u B decoded", raw_len, dec_len);
	LOG_HEXDUMP_INF(dec, dec_len, "pump reply");
	respond_rx_packet(dec, dec_len);
}

/* Strip one trailing zero byte, as the legacy TX path did for Minimed. The radio
 * layer appends its own terminator, so a caller-supplied one is redundant.
 */
static uint16_t trim_trailing_zero(const uint8_t *pkt, uint16_t len)
{
	if (len > 0 && pkt[len - 1] == 0) {
		return len - 1;
	}
	return len;
}

/* CMD_SEND_PKT: [sendChan][repeatCnt][repeatIntvl BE16][preambleExtend BE16][payload...] */
static void cmd_send_pkt(const uint8_t *p, uint16_t len)
{
	apply_pending_freq();
	uint8_t enc[SUBG_MAX_PKT_LEN] = { 0 };
	uint16_t payload_len, enc_len;
	uint8_t repeat_cnt;
	uint16_t repeat_intvl;

	if (len < 6) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	repeat_cnt = p[1];
	repeat_intvl = sys_get_be16(&p[2]);
	payload_len = trim_trailing_zero(&p[6], len - 6);

	enc_len = encode(&p[6], enc, payload_len);
	if (enc_len == 0 || enc_len > sizeof(enc)) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	subg_send_pkt(enc, (uint8_t)enc_len, repeat_cnt, repeat_intvl);
	respond_code(APS_RESP_SUCCESS);
}

/*
 * CMD_SEND_AND_LISTEN:
 * [sendChan][repeatCnt][repeatIntvl BE16][listenChan][listenTimeout BE32]
 * [retryCnt][preambleExtend BE16][payload...]
 */
static void cmd_send_and_listen(const uint8_t *p, uint16_t len)
{
	apply_pending_freq();
	uint8_t enc[SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t raw[SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t dec[SUBG_MAX_PKT_LEN] = { 0 };
	uint8_t raw_len = 0;
	uint16_t payload_len, enc_len, dec_len;
	uint8_t repeat_cnt, retry_cnt;
	uint16_t repeat_intvl;
	uint32_t timeout;
	enum subg_rx_status st;

	if (len < 12) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	repeat_cnt = p[1];
	repeat_intvl = sys_get_be16(&p[2]);
	timeout = sys_get_be32(&p[5]);
	retry_cnt = p[9];
	payload_len = trim_trailing_zero(&p[12], len - 12);

	enc_len = encode(&p[12], enc, payload_len);
	if (enc_len == 0 || enc_len > sizeof(enc)) {
		respond_code(APS_RESP_PARAM_ERROR);
		return;
	}

	LOG_INF("send+listen: %u B payload -> %u B encoded, listen %u ms, retries %u",
		payload_len, enc_len, timeout, retry_cnt);

	subg_send_pkt(enc, (uint8_t)enc_len, repeat_cnt, repeat_intvl);
	st = subg_get_pkt(raw, &raw_len, timeout);

	/* Retry loop: resend with repeatCnt forced to 0, as in legacy. */
	while (st == SUBG_RX_TIMEOUT && retry_cnt > 0) {
		LOG_DBG("send-and-listen retry, %u left", retry_cnt);
		subg_send_pkt(enc, (uint8_t)enc_len, 0, repeat_intvl);
		st = subg_get_pkt(raw, &raw_len, timeout);
		retry_cnt--;
	}

	if (st != SUBG_RX_OK) {
		LOG_INF("send+listen: no reply (status %d)", st);
		respond_rx_status(st, NULL, 0);
		return;
	}

	dec_len = decode(raw, dec, raw_len);
	LOG_INF("send+listen: REPLY %u B raw -> %u B decoded", raw_len, dec_len);
	LOG_HEXDUMP_INF(dec, dec_len, "pump reply");
	respond_rx_packet(dec, dec_len);
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
		cmd_get_pkt(req->param, req->len);
		break;
	case CMD_SEND_PKT:
		cmd_send_pkt(req->param, req->len);
		break;
	case CMD_SEND_AND_LISTEN:
		cmd_send_and_listen(req->param, req->len);
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
		/* Legacy Subg_ClrIntFlg(): clear any stale preemption request once,
		 * here, before the command runs.
		 */
		subg_clear_abort();
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

	/*
	 * CMD_UPDATE_REG runs INLINE, exactly as the legacy firmware did.
	 *
	 * It never enters the queue, so it can never be dropped behind a long listen
	 * and can never preempt one. A frequency change is three of these; losing any
	 * of them applies a mixture of old and new register bytes and tunes the radio
	 * somewhere nobody asked for, which is what broke mmtune.
	 *
	 * The legacy version did this with no synchronisation at all, racing the
	 * command loop on the SPI bus. Safe here because rf69 serialises every
	 * operation on its own mutex, held per transaction rather than across a whole
	 * receive -- so this interleaves between the FIFO polls of a live listen
	 * instead of corrupting one.
	 */
	if (buf[1] == CMD_UPDATE_REG) {
		cmd_update_reg(&buf[2], param_len);
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
	/*
	 * Abort an in-flight receive so the queued command runs promptly -- the
	 * legacy Subg_SetIntFlg(), which I originally missed entirely. Without it a
	 * CMD_SEND_AND_LISTEN holds the APS thread for its whole listen window, up
	 * to 25 s, and everything behind it is dropped.
	 *
	 * But NOT for every command. In the legacy flow CMD_UPDATE_REG ran inline
	 * and returned before SetIntFlg was ever reached, so register writes never
	 * disturbed a receive. Aborting on them too is worse than the original: a
	 * frequency change is three writes, and each one would cut short the listen
	 * it is meant to be configuring -- every receive ending as INTERRUPTED
	 * before it had a chance to hear anything.
	 *
	 * So: preempt for commands that actually want the radio now; let
	 * configuration writes queue behind the current operation. Queue depth 4
	 * absorbs a three-write frequency burst without dropping any of it.
	 */
	/*
	 * DELIBERATELY no preemption here, and a deviation from legacy.
	 *
	 * Legacy called Subg_SetIntFlg() after a successful enqueue, aborting any
	 * in-flight receive. Reproducing that against AndroidAPS created a feedback
	 * loop: an aborted listen answers "no reply" in a few hundred milliseconds,
	 * AndroidAPS treats that as a failure and retries immediately, and the retry
	 * aborts the next listen. Measured 17 interrupted receives and zero replies
	 * across a full mmtune sweep -- no listen ever ran long enough to hear a
	 * pump that answers in 13-80 ms when it is given the chance.
	 *
	 * Queue the commands and let each listen run its window instead. A frequency
	 * sweep then costs a little more wall-clock time and actually completes.
	 *
	 * The abort path itself is kept for disconnect, where cutting a listen short
	 * is exactly right.
	 */
	if (k_msgq_put(&aps_msgq, &req, K_NO_WAIT) != 0) {
		LOG_DBG("busy, command 0x%02x dropped", req.cmd);
	}
}

void aps_set_active(bool on)
{
	if (!on) {
		/* Legacy aborted an in-flight receive when BLE dropped to advertising,
		 * which is what produced RESPONSE_CODE_CMD_INTERRUPTED. Preserved.
		 */
		subg_abort();
	}
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
