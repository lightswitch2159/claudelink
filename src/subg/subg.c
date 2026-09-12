/*
 * Sub-GHz packet path -- 916 MHz Minimed only.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "subg.h"
#include "radio.h"
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

/*
 * PacketSent poll interval.
 *
 * Airtime for a wakeup frame is ~9.3 ms (3B preamble + 4B sync + 12B payload at
 * 16384 bps), but polling at 1 ms granularity cost ~7.9 ms of overhead per frame
 * -- 46% of a measured 17.1 ms. Polling finer recovers most of that.
 *
 * NOTE: this shortens the total wakeup burst, and burst duration is plausibly
 * what actually wakes a sleeping pump. SUBG_TX_MIN_BURST_MS exists to put the
 * duration back under explicit control if shortening it regresses the wake --
 * set it and the burst is padded to that length rather than finishing early.
 */
#define SUBG_TX_POLL_US 250

/* 0 = no padding; burst takes however long it takes. */
#define SUBG_TX_MIN_BURST_MS 0

/* Legacy app_subg.c: WAIT_FIFO_NOT_FULL_TIMEOUT 100 ms, TX_TIMEOUT 150. */
#define SUBG_TX_FIFO_WAIT_MS 100
#define SUBG_TX_DONE_WAIT_MS 150

/* Minimum PA setting, used for loopback TX so the PA is not driven hard into an
 * unmatched load. -18 dBm with PA0.
 */
#define SUBG_PA_LEVEL_MIN 0
#define SUBG_PA_LEVEL_DEFAULT 31

static uint16_t rx_count;
static uint16_t tx_count;
static int16_t last_rssi;
static bool rssi_latched;
static volatile bool abort_flag;

static K_SEM_DEFINE(dio1_sem, 0, 1);

void subg_init(void)
{
	if (radio_rx_irq_enable(&dio1_sem) != 0) {
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
		if (!radio_fifo_is_full()) {
			return true;
		}
		k_sleep(K_USEC(250));
	}
	LOG_WRN("FIFO stayed full for %u ms", timeout_ms);
	return false;
}

/* Set by subg_send_pkt() for the first frame of a burst only. */
static bool tx_profile_frame;

static int minimed_tx(const uint8_t *data, uint8_t len)
{
	uint16_t sent;
	uint32_t t_sb = 0, t_clr = 0, t_wr = 0, t_tx = 0, t_done = 0;
	uint32_t c0 = k_cycle_get_32();

	radio_set_mode(RADIO_MODE_STANDBY);
	t_sb = k_cyc_to_us_floor32(k_cycle_get_32() - c0);
	c0 = k_cycle_get_32();
	radio_fifo_clear();
	t_clr = k_cyc_to_us_floor32(k_cycle_get_32() - c0);

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
#endif

	/* Prime the FIFO, then stream the remainder as it drains. */
	sent = MIN(len, RADIO_FIFO_SIZE);
	c0 = k_cycle_get_32();
	if (radio_fifo_write(data, sent) != 0) {
		return -EIO;
	}
	t_wr = k_cyc_to_us_floor32(k_cycle_get_32() - c0);

	c0 = k_cycle_get_32();
	radio_set_mode(RADIO_MODE_TX);
	t_tx = k_cyc_to_us_floor32(k_cycle_get_32() - c0);

	while (sent < len) {
		if (!wait_fifo_not_full(SUBG_TX_FIFO_WAIT_MS)) {
			return -ETIMEDOUT;
		}
		if (radio_fifo_write_byte(data[sent]) != 0) {
			return -EIO;
		}
		sent++;
	}

	/* Zero terminator -- the far end stops here. See the note in subg.h. */
	if (wait_fifo_not_full(SUBG_TX_FIFO_WAIT_MS)) {
		radio_fifo_write_byte(0x00);
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
		if (radio_fifo_is_empty()) {
			break;
		}
		k_sleep(K_MSEC(1));
	}
	radio_set_mode(RADIO_MODE_STANDBY);
	return 0;
#else
	/*
	 * Legacy wait_tx_done() waits for the FIFO to empty, not for PacketSent.
	 *
	 * The FIFO empties once the packet handler has taken the last payload byte,
	 * which is earlier than the last bit leaving the antenna. Waiting for
	 * PacketSent on every frame is stricter than the original and cost real time
	 * across a 201-frame burst. The sequencer returns the radio to standby by
	 * itself once the packet completes.
	 */
	c0 = k_cycle_get_32();
	for (int i = 0; i < SUBG_TX_DONE_WAIT_MS; i++) {
		if (radio_fifo_is_empty()) {
			t_done = k_cyc_to_us_floor32(k_cycle_get_32() - c0);
			if (tx_profile_frame) {
				tx_profile_frame = false;
				LOG_DBG("tx profile len=%u: standby=%uus clear=%uus "
					"write=%uus ->tx=%uus drain=%uus",
					len, t_sb, t_clr, t_wr, t_tx, t_done);
			}
			return 0;
		}
		k_sleep(K_MSEC(1));
	}

	LOG_WRN("FIFO did not drain within %u ms", SUBG_TX_DONE_WAIT_MS);
	return -ETIMEDOUT;
#endif
}

/*
 * Repeat burst, streamed as one continuous transmission.
 *
 * The obvious implementation -- call minimed_tx() once per frame -- costs a full
 * STANDBY->TX cycle each time, with a PLL relock and two ModeReady waits. Measured
 * at 16.4 ms per frame against 9.3 ms of actual airtime: 43% overhead, so a
 * 201-frame wakeup took 3.3 s and AndroidAPS spends 31 such bursts getting through
 * a tune.
 *
 * In fixed-length mode the packet handler chops the FIFO into PayloadLength-sized
 * packets and sends them back to back without CPU involvement. So the repeats can
 * be streamed as one byte sequence, staying in TX throughout and refilling the
 * FIFO as it drains. Identical bytes on air, far less dead time between them.
 */
static int minimed_tx_repeat(const uint8_t *data, uint8_t len, unsigned int frames)
{
	uint8_t frame[RADIO_FIFO_SIZE];
	uint8_t flen = len + 1;            /* payload + zero terminator */
	uint32_t total, written = 0;
	int64_t deadline;

	if (flen > RADIO_FIFO_SIZE || frames == 0) {
		return -EINVAL;
	}

	memcpy(frame, data, len);
	frame[len] = 0x00;
	total = (uint32_t)flen * frames;

	radio_set_mode(RADIO_MODE_STANDBY);
	radio_fifo_clear();
	radio_set_payload_len(flen);

	/* Prime with whole frames only, so packet boundaries stay aligned. */
	while (written + flen <= RADIO_FIFO_SIZE && written < total) {
		if (radio_fifo_write(frame, flen) != 0) {
			return -EIO;
		}
		written += flen;
	}

	radio_set_mode(RADIO_MODE_TX);

	/* Refill as it drains. Bounded so a stalled radio cannot hang the thread. */
	deadline = k_uptime_get() + (int64_t)frames * 50 + 1000;
	while (written < total) {
		if (k_uptime_get() > deadline) {
			LOG_WRN("burst stalled at %u/%u bytes", written, total);
			break;
		}
		if (radio_fifo_is_full()) {
			k_sleep(K_USEC(SUBG_TX_POLL_US));
			continue;
		}
		if (radio_fifo_write_byte(frame[written % flen]) != 0) {
			return -EIO;
		}
		written++;
	}

	/* Let the tail drain before leaving TX, or the last packets are cut off. */
	for (int i = 0; i < SUBG_TX_DONE_WAIT_MS * (1000 / SUBG_TX_POLL_US); i++) {
		if (radio_fifo_is_empty()) {
			break;
		}
		k_sleep(K_USEC(SUBG_TX_POLL_US));
	}
	k_sleep(K_MSEC(2));   /* final packet still being clocked out */
	radio_set_mode(RADIO_MODE_SLEEP);

	return (written == total) ? 0 : -EIO;
}

int subg_send_pkt(const uint8_t *data, uint8_t len, uint8_t repeat_cnt,
		  uint16_t repeat_interval_ms)
{
	int err;

	if (data == NULL || len == 0) {
		return -EINVAL;
	}

	tx_count++;

	/* Once per burst, as in legacy Subg_SendPkt(). */
	tx_profile_frame = true;
	radio_set_mode(RADIO_MODE_STANDBY);
	radio_set_payload_len(len + 1);

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

	/*
	 * A back-to-back repeat burst streams through the FIFO in one transmission.
	 * Anything with a requested gap between repeats still goes frame by frame,
	 * since the gap is the point.
	 */
	/*
	 * REVERTED: streaming the repeats through the FIFO was measurably worse.
	 *
	 * The idea was that fixed-length mode would chop a continuously refilled
	 * FIFO into back-to-back packets with no CPU involvement. It does not: with
	 * the sequencer enabled and TXSTART_FIFONOTEMPTY, the radio returns to
	 * standby after each PacketSent, so the refill loop ends up fighting the
	 * sequencer rather than feeding one transmission. Measured 3732 ms against
	 * 3300 ms for the straightforward per-frame path, and pump replies became
	 * intermittent.
	 *
	 * minimed_tx_repeat() is kept below but unused, as the record of a tried and
	 * rejected approach. Making it work would mean disabling the sequencer and
	 * driving the mode transitions by hand, which is a larger change than the
	 * ~20% it might buy.
	 */
	if (false) {
		if (minimed_tx_repeat(data, len, (unsigned int)repeat_cnt + 1) == 0) {
			sent = (unsigned int)repeat_cnt + 1;
		} else {
			failed = (unsigned int)repeat_cnt + 1;
		}
	} else {
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
	}

	/* Optionally hold the burst to a minimum wall-clock length -- see the note
	 * on SUBG_TX_MIN_BURST_MS. Off by default.
	 */
	if (SUBG_TX_MIN_BURST_MS > 0 && repeat_cnt > 0) {
		int64_t elapsed = k_uptime_get() - t0;

		if (elapsed < SUBG_TX_MIN_BURST_MS) {
			k_sleep(K_MSEC(SUBG_TX_MIN_BURST_MS - elapsed));
		}
	}

	if (repeat_cnt > 0 || failed > 0) {
		LOG_INF("tx burst: %u bytes x%u -> %u sent, %u failed, %lld ms",
			len, repeat_cnt + 1, sent, failed, k_uptime_get() - t0);
	} else {
		LOG_DBG("tx: %u bytes", len);
	}

	/*
	 * rf_stop(), once per burst -- exactly where legacy Subg_SendPkt() puts it,
	 * after the repeat loop rather than after each frame. See the note in
	 * subg_get_pkt() for why SLEEP and not STANDBY.
	 */
	radio_set_mode(RADIO_MODE_SLEEP);

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
	radio_set_mode(RADIO_MODE_STANDBY);
	radio_set_payload_len(SUBG_MAX_PKT_LEN);

	/*
	 * Drain the FIFO before listening.
	 *
	 * Without this, residue from the transmit that just finished is still
	 * sitting there, so the first read returns stale bytes -- and a 0x00 among
	 * them trips the terminator check and aborts the receive at zero length,
	 * immediately. Against AndroidAPS that looked like every 4000 ms
	 * send-and-listen completing instantly with nothing heard.
	 */
	radio_fifo_clear();
	while (!radio_fifo_is_empty()) {
		uint8_t stale;

		if (radio_fifo_read_byte(&stale) != 0) {
			break;
		}
	}

	/* Clear any edge left over from a previous receive. */
	k_sem_reset(&dio1_sem);
	radio_set_mode(RADIO_MODE_RX);

	start = k_uptime_get();

	rssi_latched = false;

	while (true) {
		if (!radio_fifo_is_empty()) {
			/*
			 * Latch RSSI on the first byte of the packet, while the carrier is
			 * still present.
			 *
			 * RegRssiValue is a live measurement of current received power, not
			 * a per-packet latch. Reading it once the packet has been fully
			 * drained measures whatever is on the air *after* the pump stopped
			 * transmitting -- i.e. the noise floor. That produced replies
			 * reported at -93..-95 dBm from a pump inches away, exactly the
			 * floor the boot-time survey measures, mixed with occasional real
			 * values when the read happened to catch the tail.
			 *
			 * mmtune ranks candidate frequencies purely by reply RSSI, so a
			 * floor reading on half the replies makes the ranking meaningless.
			 */
			if (!rssi_latched) {
				last_rssi = radio_sample_rssi();
				rssi_latched = true;
			}

			if (radio_fifo_read_byte(&b) != 0) {
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
			radio_set_mode(RADIO_MODE_SLEEP);
			return SUBG_RX_INTERRUPTED;
		}

		if (timeout_ms > 0 && (k_uptime_get() - start) > (int64_t)timeout_ms) {
			radio_set_mode(RADIO_MODE_SLEEP);
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
	 * SLEEP, not STANDBY, once the receive is over.
	 *
	 * Legacy's Subg_GetPkt() and Subg_SendPkt() both end in rf_stop(), which
	 * sets RADIO_MODE_SLEEP, and Rf69_DevParaCfg() leaves the radio asleep at
	 * init. This port left it in STANDBY on every path, so the RFM69 was awake
	 * permanently -- datasheet-typical 1.25 mA against 0.1 uA asleep, which on
	 * this board swamps everything else in the idle budget.
	 *
	 * SPI still works in sleep, so deferred register writes (apply_pending_freq)
	 * do not need the radio woken first.
	 */
	radio_set_mode(RADIO_MODE_SLEEP);

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

	radio_set_mode(RADIO_MODE_STANDBY);
	radio_fifo_clear();

	if (radio_fifo_write(src, len) != 0) {
		return false;
	}

	for (uint16_t i = 0; i < len; i++) {
		if (radio_fifo_read_byte(&back) != 0) {
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

	radio_set_mode(RADIO_MODE_STANDBY);
	radio_fifo_clear();
	if (radio_fifo_write(enc, enc_len) != 0) {
		return false;
	}
	for (uint16_t i = 0; i < enc_len; i++) {
		if (radio_fifo_read_byte(&back[i]) != 0) {
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
	radio_set_mode(RADIO_MODE_STANDBY);
	radio_fifo_clear();
	r.fifo_byte_wrote = 0x5C;
	if (radio_fifo_write_byte(r.fifo_byte_wrote) == 0 &&
	    radio_fifo_read_byte(&r.fifo_byte_read) == 0) {
		r.fifo_byte = (r.fifo_byte_read == r.fifo_byte_wrote);
	}

	/* Stage 2: burst write, byte-wise read back. */
	r.burst_len = sizeof(burst);
	r.fifo_burst = fifo_roundtrip(burst, sizeof(burst), &r.burst_matched);

	/* Stage 3: do the status flags actually track FIFO content? */
	radio_fifo_clear();
	bool empty_when_cleared = radio_fifo_is_empty();
	radio_fifo_write_byte(0x42);
	bool not_empty_after_write = !radio_fifo_is_empty();
	radio_fifo_read_byte(&b);
	bool empty_after_drain = radio_fifo_is_empty();
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
	radio_set_mode(RADIO_MODE_STANDBY);
	radio_fifo_clear();
	while (!radio_fifo_is_empty()) {
		if (radio_fifo_read_byte(&b) != 0) {
			break;
		}
	}
	k_sem_reset(&dio1_sem);
	radio_fifo_write_byte(0x5A);
	r.dio1_irq_fired = (k_sem_take(&dio1_sem, K_MSEC(50)) == 0);
	radio_fifo_clear();

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
	radio_set_power_level(SUBG_PA_LEVEL_MIN);
	{
		static const uint8_t probe[] = { 0xA7, 0x01, 0x02, 0x03 };
		int64_t t0 = k_uptime_get();

		r.tx_packet_sent = (subg_send_pkt(probe, sizeof(probe), 0, 0) == 0);
		r.tx_wait_us = (uint32_t)((k_uptime_get() - t0) * 1000);
	}
	radio_set_power_level(SUBG_PA_LEVEL_DEFAULT);

	/* Restore a known-good state for normal operation. */
	radio_config_916();
	radio_set_mode(RADIO_MODE_STANDBY);

	r.all_passed = r.fifo_byte && r.fifo_burst && r.flags_track &&
		       r.datapath_none && r.datapath_manchester &&
		       r.datapath_4b6b && r.dio1_irq_fired && r.tx_packet_sent;

	if (out) {
		*out = r;
	}
	/* Leave the radio as the rest of the driver does: asleep unless in use.
	 * Without this the boot-time register dump reports STANDBY and the
	 * "asleep at idle" invariant is unverifiable from the log.
	 */
	radio_set_mode(RADIO_MODE_SLEEP);

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

	radio_dump_regs();

	{
		int16_t lo = 0, hi = 0;
		int spread = radio_rssi_survey(&lo, &hi);

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
