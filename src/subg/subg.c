/*
 * Sub-GHz packet path -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "subg.h"
#include "rf69.h"
#include "rf69_registers.h"
#include "4b6b.h"
#include "manchester.h"

LOG_MODULE_REGISTER(subg, CONFIG_ORANGELINK_LOG_LEVEL);

/*
 * Receive is driven by the DIO1 interrupt, mapped to FifoNotEmpty.
 *
 * DIO1 rather than DIO0: this framing is variable length, terminated by 0x00,
 * under a fixed PayloadLength of 107, so DIO0's PayloadReady never asserts for a
 * normal short packet. FifoNotEmpty asserts the moment a byte lands whatever the
 * eventual length turns out to be, which is exactly what this loop needs.
 *
 * FifoNotEmpty is a LEVEL, not a pulse, so the edge only arrives on the
 * empty->non-empty transition. If bytes keep arriving while we drain, the line
 * stays high and no further edge is generated. Waiting on the semaphore alone
 * would therefore deadlock against an edge that has already passed.
 *
 * So: block on the semaphore with a short bounded fallback and re-check the FIFO
 * every pass. The interrupt supplies the latency in the common case; the fallback
 * makes the loop immune to that race. Either way the thread sleeps, so the
 * Bluetooth threads keep running -- the legacy loop busy-waited with
 * Kit_DelayMs(1) and never yielded, which is what starved everything else.
 */
#define SUBG_RX_WAIT_MS 2

#define SUBG_TX_FIFO_WAIT_MS 100
#define SUBG_TX_DONE_WAIT_MS 200

/* Minimum PA setting, used for loopback TX so the PA is not driven hard into an
 * unmatched load. -18 dBm with PA0.
 */
#define SUBG_PA_LEVEL_MIN 0
#define SUBG_PA_LEVEL_DEFAULT 31

static uint16_t rx_count;
static uint16_t tx_count;
static int16_t last_rssi;
static volatile bool abort_flag;

static K_SEM_DEFINE(dio1_sem, 0, 1);

void subg_init(void)
{
	if (rf69_dio1_irq_enable(&dio1_sem) != 0) {
		LOG_WRN("DIO1 interrupt unavailable; receive falls back to polling");
	}
	LOG_INF("sub-GHz ready (916 MHz Minimed, max %u B)", SUBG_MAX_PKT_LEN);
}

void subg_abort(void)
{
	abort_flag = true;
}

void subg_clear_abort(void)
{
	abort_flag = false;
}

uint16_t subg_get_rx_count(void) { return rx_count; }
uint16_t subg_get_tx_count(void) { return tx_count; }
int16_t subg_get_last_rssi(void) { return last_rssi; }

/* ------------------------------------------------------------------------- *
 * Transmit
 * ------------------------------------------------------------------------- */

static bool wait_fifo_not_full(uint32_t timeout_ms)
{
	for (uint32_t i = 0; i < timeout_ms * 4; i++) {
		if (!rf69_fifo_is_full()) {
			return true;
		}
		k_sleep(K_USEC(250));
	}
	LOG_WRN("FIFO stayed full for %u ms", timeout_ms);
	return false;
}

static int minimed_tx(const uint8_t *data, uint8_t len)
{
	uint16_t sent;

	rf69_set_mode(RF69_MODE_STANDBY);
	rf69_fifo_clear();

	/*
	 * Set PayloadLength to the frame we are actually sending: len data bytes
	 * plus the 0x00 terminator.
	 *
	 * This matters. The 916 config uses PACKET1_FORMAT_FIXED with
	 * PAYLOADLENGTH 0xFF, so without this the radio transmits a full 255-byte
	 * frame -- about 125 ms of airtime at 16384 bps -- padding the tail with
	 * whatever the underrunning FIFO produces, and PacketSent only fires at the
	 * end of all 255 bytes. Measured at 135 ms before this was added.
	 *
	 * The legacy driver never set PayloadLength for TX. It got away with it
	 * because wait_tx_done() only waited for the FIFO to drain and rf_stop()
	 * then forced SLEEP, truncating the transmission mid-packet. That works
	 * against a receiver that stops at the zero terminator, but it wastes
	 * airtime, radiates padding, and makes the send-and-listen turnaround far
	 * slower than it needs to be.
	 *
	 * DEVIATION worth validating against the 722: the pump has only ever seen
	 * the truncated-255 behaviour. If it turns out to depend on that tail,
	 * revert to draining-plus-STANDBY instead.
	 */
#if defined(CONFIG_ORANGELINK_TX_LEGACY_TRUNCATE)
	/* Leave PayloadLength at 0xFF and truncate by forcing STANDBY once the FIFO
	 * has drained, exactly as the legacy firmware did.
	 */
#else
	rf69_set_payload_len(len + 1);
#endif

	/* Prime the FIFO, then stream the remainder as it drains. */
	sent = MIN(len, RF69_FIFO_SIZE);
	if (rf69_fifo_write(data, sent) != 0) {
		return -EIO;
	}

	rf69_set_mode(RF69_MODE_TX);

	while (sent < len) {
		if (!wait_fifo_not_full(SUBG_TX_FIFO_WAIT_MS)) {
			return -ETIMEDOUT;
		}
		if (rf69_fifo_write_byte(data[sent]) != 0) {
			return -EIO;
		}
		sent++;
	}

	/* Zero terminator -- the far end stops here. See the note in subg.h. */
	if (wait_fifo_not_full(SUBG_TX_FIFO_WAIT_MS)) {
		rf69_fifo_write_byte(0x00);
	}

	/*
	 * Rely on the sequencer to leave TX once PacketSent fires.
	 *
	 * Polled, not interrupt-driven: DIO1 carries FifoNotEmpty, not PacketSent,
	 * so it cannot signal completion. DIO0 could, and is still free on the
	 * module if TX latency ever matters -- but TX is short and bounded, so
	 * polling costs little.
	 */
#if defined(CONFIG_ORANGELINK_TX_LEGACY_TRUNCATE)
	/* Legacy: wait for the FIFO to drain, then cut the transmission short. */
	for (int i = 0; i < SUBG_TX_DONE_WAIT_MS; i++) {
		if (rf69_fifo_is_empty()) {
			break;
		}
		k_sleep(K_MSEC(1));
	}
	rf69_set_mode(RF69_MODE_STANDBY);
	return 0;
#else
	for (int i = 0; i < SUBG_TX_DONE_WAIT_MS; i++) {
		if (rf69_packet_sent()) {
			return 0;
		}
		k_sleep(K_MSEC(1));
	}

	LOG_WRN("PacketSent did not assert within %u ms", SUBG_TX_DONE_WAIT_MS);
	return -ETIMEDOUT;
#endif
}

int subg_send_pkt(const uint8_t *data, uint8_t len, uint8_t repeat_cnt,
		  uint16_t repeat_interval_ms)
{
	int err;

	if (data == NULL || len == 0) {
		return -EINVAL;
	}

	tx_count++;

	/*
	 * A repeat burst must not be abandoned on a single failed frame.
	 *
	 * This loop used to return on the first error from minimed_tx(), so one
	 * missed PacketSent inside a 201-frame wakeup burst cut the whole burst
	 * short. Waking a sleeping Medtronic pump depends on it hearing a sustained
	 * sequence, so a truncated burst is the difference between waking and not --
	 * and the failure is silent, because the command still returns a sensible
	 * status afterwards.
	 *
	 * Errors are counted and reported instead. The legacy driver had no error
	 * path here at all and simply kept transmitting.
	 */
	int64_t t0 = k_uptime_get();
	unsigned int sent = 0, failed = 0;

	err = minimed_tx(data, len);
	if (err) {
		failed++;
	} else {
		sent++;
	}

	for (uint8_t i = 0; i < repeat_cnt; i++) {
		if (repeat_interval_ms) {
			k_sleep(K_MSEC(repeat_interval_ms));
		}
		if (minimed_tx(data, len) != 0) {
			failed++;
		} else {
			sent++;
		}
	}

	if (repeat_cnt > 0 || failed > 0) {
		LOG_INF("tx burst: %u bytes x%u -> %u sent, %u failed, %lld ms",
			len, repeat_cnt + 1, sent, failed, k_uptime_get() - t0);
	} else {
		LOG_DBG("tx: %u bytes", len);
	}

	/* Succeed if anything at all went out; a partial burst can still wake. */
	return (sent > 0) ? 0 : -EIO;
}

/* ------------------------------------------------------------------------- *
 * Receive
 * ------------------------------------------------------------------------- */

enum subg_rx_status subg_get_pkt(uint8_t *buf, uint8_t *len, uint32_t timeout_ms)
{
	uint8_t count = 0;
	uint8_t b;
	int64_t start;

	if (buf == NULL || len == NULL) {
		return SUBG_RX_TIMEOUT;
	}

	/*
	 * Deliberately does NOT clear abort_flag.
	 *
	 * It used to, and that silently swallowed preemption: a send-and-listen
	 * transmits first, so an abort arriving during the transmit was wiped the
	 * moment the listen began, and the 25 s window ran to completion anyway.
	 * Against AndroidAPS that showed up as zero interrupted receives while
	 * dozens of commands were being refused behind a listen that should have
	 * been cut short. The dispatcher clears it instead -- legacy
	 * Subg_ClrIntFlg(), called once before running a command.
	 */
	rf69_set_mode(RF69_MODE_STANDBY);
	rf69_set_payload_len(SUBG_MAX_PKT_LEN);

	/*
	 * Drain the FIFO before listening.
	 *
	 * Without this, residue from the transmit that just finished is still
	 * sitting there, so the first read returns stale bytes -- and a 0x00 among
	 * them trips the terminator check and aborts the receive at zero length,
	 * immediately. Against AndroidAPS that looked like every 4000 ms
	 * send-and-listen completing instantly with nothing heard.
	 */
	rf69_fifo_clear();
	while (!rf69_fifo_is_empty()) {
		uint8_t stale;

		if (rf69_fifo_read_byte(&stale) != 0) {
			break;
		}
	}

	/* Clear any edge left over from a previous receive. */
	k_sem_reset(&dio1_sem);
	rf69_set_mode(RF69_MODE_RX);

	start = k_uptime_get();

	while (true) {
		if (!rf69_fifo_is_empty()) {
			if (rf69_fifo_read_byte(&b) != 0) {
				break;
			}

			/* Zero byte terminates the packet. */
			if (b == 0) {
				break;
			}

			buf[count++] = b;

			if (count >= SUBG_MAX_PKT_LEN) {
				break;
			}
			continue;   /* drain fast while bytes are available */
		}

		if (abort_flag) {
			rf69_set_mode(RF69_MODE_STANDBY);
			return SUBG_RX_INTERRUPTED;
		}

		if (timeout_ms > 0 && (k_uptime_get() - start) > (int64_t)timeout_ms) {
			rf69_set_mode(RF69_MODE_STANDBY);
			return SUBG_RX_TIMEOUT;
		}

		/* Wait for DIO1 (FifoNotEmpty). The bounded timeout is the safety net
		 * for the level-vs-edge race described at the top of this file.
		 */
		k_sem_take(&dio1_sem, K_MSEC(SUBG_RX_WAIT_MS));
	}

	/*
	 * Trim the end-of-packet glitch: a trailing byte of just one or two high
	 * bits is an artefact of OOK demodulation, not data. Legacy behaviour.
	 */
	if (count > 0 && (buf[count - 1] == 0x80 || buf[count - 1] == 0xC0)) {
		LOG_DBG("trimmed end-of-packet glitch 0x%02x", buf[count - 1]);
		count--;
	}

	/*
	 * Sample RSSI BEFORE leaving RX.
	 *
	 * RegRssiValue is only meaningful while the receiver is running; read after
	 * a switch to standby it returns a stale value. This used to sit after the
	 * mode change, so every reply carried a nonsense RSSI -- and mmtune ranks
	 * candidate frequencies purely by the RSSI of the reply. The scan could
	 * never pick a winner, no lastGoodFrequency was ever recorded, and
	 * AndroidAPS re-tuned on every single connection.
	 *
	 * The legacy driver never left RX inside minimed_rx at all, so it read a
	 * live value by construction. Leaving RX here is still right -- we should
	 * not keep the receiver running -- but the read has to come first.
	 */
	if (count > 0) {
		last_rssi = rf69_read_rssi(false);
	}

	rf69_set_mode(RF69_MODE_STANDBY);

	/*
	 * Legacy returned SUBG_RX_OK even when count was 0, leaving *pRxLen
	 * untouched -- the caller then transmitted whatever was on its stack.
	 * A zero-length receive is reported as a timeout here instead.
	 */
	if (count == 0) {
		LOG_DBG("rx: nothing after %lld ms", k_uptime_get() - start);
		return SUBG_RX_TIMEOUT;
	}

	LOG_INF("rx: %u bytes after %lld ms, rssi %d dBm",
		count, k_uptime_get() - start, last_rssi);
	rx_count++;
	*len = count;
	return SUBG_RX_OK;
}

/* ------------------------------------------------------------------------- *
 * Loopback verification (no RF link required)
 * ------------------------------------------------------------------------- */

/* Push a payload through the hardware FIFO and read it straight back. Verifies
 * the SPI burst path and the FIFO itself, with the radio in standby so nothing
 * is transmitted.
 */
static bool fifo_roundtrip(const uint8_t *src, uint16_t len, uint16_t *matched)
{
	uint8_t back;
	uint16_t ok = 0;

	rf69_set_mode(RF69_MODE_STANDBY);
	rf69_fifo_clear();

	if (rf69_fifo_write(src, len) != 0) {
		return false;
	}

	for (uint16_t i = 0; i < len; i++) {
		if (rf69_fifo_read_byte(&back) != 0) {
			break;
		}
		if (back == src[i]) {
			ok++;
		}
	}

	if (matched) {
		*matched = ok;
	}
	return ok == len;
}

/* Full transform chain: encode -> hardware FIFO -> read back -> decode. */
static bool datapath_roundtrip(int encoding)
{
	static const uint8_t payload[] = {
		0xA7, 0x12, 0x34, 0x56, 0x00 + 1, 0x5D, 0x0F, 0xAA, 0x55, 0x99,
	};
	uint8_t enc[64] = { 0 };
	uint8_t back[64] = { 0 };
	uint8_t dec[64] = { 0 };
	uint16_t enc_len = 0, dec_len;

	switch (encoding) {
	case 0:
		memcpy(enc, payload, sizeof(payload));
		enc_len = sizeof(payload);
		break;
	case 1:
		if (!encode_manchester(payload, enc, sizeof(payload))) {
			return false;
		}
		enc_len = sizeof(payload) * 2;
		break;
	case 2:
		enc_len = encode_4b6b(payload, enc, sizeof(payload));
		break;
	default:
		return false;
	}

	if (enc_len == 0 || enc_len > sizeof(enc)) {
		return false;
	}

	rf69_set_mode(RF69_MODE_STANDBY);
	rf69_fifo_clear();
	if (rf69_fifo_write(enc, enc_len) != 0) {
		return false;
	}
	for (uint16_t i = 0; i < enc_len; i++) {
		if (rf69_fifo_read_byte(&back[i]) != 0) {
			return false;
		}
	}
	if (memcmp(enc, back, enc_len) != 0) {
		return false;
	}

	switch (encoding) {
	case 0:
		memcpy(dec, back, enc_len);
		dec_len = enc_len;
		break;
	case 1:
		dec_len = decode_manchester(back, dec, enc_len);
		break;
	default:
		dec_len = decode_4b6b(back, dec, enc_len);
		break;
	}

	return dec_len == sizeof(payload) &&
	       memcmp(dec, payload, sizeof(payload)) == 0;
}

int subg_loopback_run(struct subg_loopback *out)
{
	struct subg_loopback r = { 0 };
	static const uint8_t burst[] = {
		0x00 + 1, 0x02, 0x03, 0xFF, 0xA5, 0x5A, 0x7E, 0x81,
		0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
	};
	uint8_t b;

	/* Stage 1: single byte through the FIFO. */
	rf69_set_mode(RF69_MODE_STANDBY);
	rf69_fifo_clear();
	r.fifo_byte_wrote = 0x5C;
	if (rf69_fifo_write_byte(r.fifo_byte_wrote) == 0 &&
	    rf69_fifo_read_byte(&r.fifo_byte_read) == 0) {
		r.fifo_byte = (r.fifo_byte_read == r.fifo_byte_wrote);
	}

	/* Stage 2: burst write, byte-wise read back. */
	r.burst_len = sizeof(burst);
	r.fifo_burst = fifo_roundtrip(burst, sizeof(burst), &r.burst_matched);

	/* Stage 3: do the status flags actually track FIFO content? */
	rf69_fifo_clear();
	bool empty_when_cleared = rf69_fifo_is_empty();
	rf69_fifo_write_byte(0x42);
	bool not_empty_after_write = !rf69_fifo_is_empty();
	rf69_fifo_read_byte(&b);
	bool empty_after_drain = rf69_fifo_is_empty();
	r.flags_track = empty_when_cleared && not_empty_after_write && empty_after_drain;

	/* Stage 4: the real encode/decode chain through hardware. */
	r.datapath_none = datapath_roundtrip(0);
	r.datapath_manchester = datapath_roundtrip(1);
	r.datapath_4b6b = datapath_roundtrip(2);

	/*
	 * Stage 5: does the DIO1 interrupt actually fire?
	 *
	 * FifoNotEmpty is fully under our control, so this needs no RF at all: arm
	 * the interrupt with the FIFO empty, push one byte, and the empty->non-empty
	 * edge must wake the semaphore. This is what caught DIO1 being unconnected.
	 */
	rf69_set_mode(RF69_MODE_STANDBY);
	rf69_fifo_clear();
	while (!rf69_fifo_is_empty()) {
		if (rf69_fifo_read_byte(&b) != 0) {
			break;
		}
	}
	k_sem_reset(&dio1_sem);
	rf69_fifo_write_byte(0x5A);
	r.dio1_irq_fired = (k_sem_take(&dio1_sem, K_MSEC(50)) == 0);
	rf69_fifo_clear();

	/*
	 * Stage 6: TX completes, exercising the real subg_send_pkt() path.
	 *
	 * Deliberately calls the production function rather than hand-rolling a
	 * transmit here. An earlier version of this stage did its own FIFO write and
	 * mode change, which meant it silently skipped the PayloadLength that
	 * minimed_tx() sets -- so it kept measuring 136 ms of 255-byte fixed-length
	 * airtime and reported PASS while telling us nothing about the real path.
	 *
	 * Transmits at MINIMUM PA level. Driving the PA hard into an unmatched load
	 * risks damaging it, and this runs with whatever antenna happens to be
	 * fitted. -18 dBm also keeps radiated output negligible.
	 */
	rf69_set_power_level(SUBG_PA_LEVEL_MIN);
	{
		static const uint8_t probe[] = { 0xA7, 0x01, 0x02, 0x03 };
		int64_t t0 = k_uptime_get();

		r.tx_packet_sent = (subg_send_pkt(probe, sizeof(probe), 0, 0) == 0);
		r.tx_wait_us = (uint32_t)((k_uptime_get() - t0) * 1000);
	}
	rf69_set_power_level(SUBG_PA_LEVEL_DEFAULT);

	/* Restore a known-good state for normal operation. */
	rf69_config_916();
	rf69_set_mode(RF69_MODE_STANDBY);

	r.all_passed = r.fifo_byte && r.fifo_burst && r.flags_track &&
		       r.datapath_none && r.datapath_manchester &&
		       r.datapath_4b6b && r.dio1_irq_fired && r.tx_packet_sent;

	if (out) {
		*out = r;
	}
	return r.all_passed ? 0 : -EIO;
}

void subg_loopback_report(const struct subg_loopback *r)
{
	if (r == NULL) {
		return;
	}

	LOG_INF("---- sub-GHz loopback (no RF link) ----");
	LOG_INF("[%s] fifo byte      wrote 0x%02x read 0x%02x",
		r->fifo_byte ? "PASS" : "FAIL", r->fifo_byte_wrote, r->fifo_byte_read);
	LOG_INF("[%s] fifo burst     %u/%u bytes matched",
		r->fifo_burst ? "PASS" : "FAIL", r->burst_matched, r->burst_len);
	LOG_INF("[%s] fifo flags     empty/not-empty track content",
		r->flags_track ? "PASS" : "FAIL");
	LOG_INF("[%s] datapath NONE  encode -> fifo -> decode",
		r->datapath_none ? "PASS" : "FAIL");
	LOG_INF("[%s] datapath MANCH encode -> fifo -> decode",
		r->datapath_manchester ? "PASS" : "FAIL");
	LOG_INF("[%s] datapath 4B6B  encode -> fifo -> decode",
		r->datapath_4b6b ? "PASS" : "FAIL");
	LOG_INF("[%s] dio1 interrupt fires on FifoNotEmpty edge",
		r->dio1_irq_fired ? "PASS" : "FAIL");
	if (!r->dio1_irq_fired) {
		LOG_ERR("       no edge seen -- DIO1 not wired to D2/P0.28, or the");
		LOG_ERR("       radio has it mapped to something other than FifoNotEmpty.");
	}
	LOG_INF("[%s] tx complete    PacketSent asserted after %u us",
		r->tx_packet_sent ? "PASS" : "FAIL", r->tx_wait_us);
	LOG_INF("---- %s ----", r->all_passed ? "ALL PASSED" : "FAILURES PRESENT");
	LOG_INF("note: loopback proves FIFO and encoding, NOT receiver sensitivity");
	LOG_INF("      or interoperability -- those need the Minimed 722.");

	rf69_dump_regs();

	{
		int16_t lo = 0, hi = 0;
		int spread = rf69_rssi_survey(&lo, &hi);

		LOG_INF("---- RSSI survey (24 samples in RX) ----");
		LOG_INF("  min=%d dBm  max=%d dBm  spread=%d dB", lo, hi, spread);
		if (spread == 0) {
			LOG_WRN("  RSSI never moved. A receiver with an antenna shows some");
			LOG_WRN("  thermal variation; a flat reading suggests no antenna is");
			LOG_WRN("  fitted, or the front end is not running.");
		} else {
			LOG_INF("  RSSI varies, so the receive front end is live.");
		}
	}
}
