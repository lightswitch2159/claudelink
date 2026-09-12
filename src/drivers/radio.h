/*
 * Sub-GHz radio selection -- compile-time dispatch, no vtable.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * src/subg talks to the radio through these wrappers so the packet layer does not
 * name a specific part. They are static inline and resolve to a direct call to
 * the selected backend, so the generated code is the same as calling it directly
 * -- selecting the RFM69 must produce a byte-identical image to before this
 * header existed.
 *
 * This is deliberately thin. It abstracts register-level operations, NOT the
 * transmit and receive strategies, which genuinely differ: the RFM69 path starts
 * transmission by writing the mode and keys receive on DIO1/FifoNotEmpty, while
 * the SX1276 can drive transmission from its sequencer and keys receive on
 * DIO2/SyncAddressMatch (it has no FifoNotEmpty mapping at all). Pretending those
 * are the same would mean forcing the SX1276 into a shape its own working drivers
 * do not use. Where the strategies diverge, src/subg branches explicitly.
 */

#ifndef ORANGELINK_RADIO_H_
#define ORANGELINK_RADIO_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#if defined(CONFIG_ORANGELINK_SX1276)
#include "sx1276.h"
#else
#include "rf69.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum radio_mode {
	RADIO_MODE_SLEEP = 0,
	RADIO_MODE_STANDBY,
	RADIO_MODE_RX,
	RADIO_MODE_TX,
};

#if defined(CONFIG_ORANGELINK_SX1276)

#define RADIO_NAME "SX1276"
#define RADIO_FIFO_SIZE SX1276_FIFO_SIZE

static inline int radio_set_mode(enum radio_mode m)
{
	switch (m) {
	case RADIO_MODE_SLEEP:   return sx1276_set_mode(SX1276_MODE_SLEEP);
	case RADIO_MODE_STANDBY: return sx1276_set_mode(SX1276_MODE_STANDBY);
	case RADIO_MODE_RX:      return sx1276_set_mode(SX1276_MODE_RX);
	case RADIO_MODE_TX:      return sx1276_set_mode(SX1276_MODE_TX);
	default:                 return -EINVAL;
	}
}

static inline int  radio_config_916(void)            { return sx1276_config_916(); }
static inline int  radio_set_freq(uint32_t hz)       { return sx1276_set_freq(hz); }
static inline uint32_t radio_get_freq(void)          { return sx1276_get_freq(); }
static inline int  radio_set_power_level(uint8_t l)  { return sx1276_set_power_level(l); }
static inline int  radio_set_payload_len(uint8_t n)  { return sx1276_set_payload_len(n); }
static inline int  radio_fifo_clear(void)            { return sx1276_fifo_clear(); }
static inline int  radio_fifo_write(const uint8_t *d, uint16_t n)
						     { return sx1276_fifo_write(d, n); }
static inline int  radio_fifo_write_byte(uint8_t b)  { return sx1276_fifo_write_byte(b); }
static inline int  radio_fifo_read_byte(uint8_t *b)  { return sx1276_fifo_read_byte(b); }
static inline bool radio_fifo_is_empty(void)         { return sx1276_fifo_is_empty(); }
static inline bool radio_fifo_is_full(void)          { return sx1276_fifo_is_full(); }
static inline bool radio_packet_sent(void)           { return sx1276_packet_sent(); }
static inline int  radio_rx_irq_enable(struct k_sem *s) { return sx1276_rx_irq_enable(s); }
static inline int  radio_rx_irq_disable(void)        { return sx1276_rx_irq_disable(); }

/* Sample RSSI at the moment the carrier is known present, and return it. */
static inline int16_t radio_sample_rssi(void)
{
	sx1276_latch_rssi();
	return sx1276_read_rssi();
}

static inline void radio_dump_regs(void) { sx1276_dump_regs(); }
static inline int  radio_rssi_survey(int16_t *lo, int16_t *hi)
					 { return sx1276_rssi_survey(lo, hi); }

#else /* RFM69 / SX1231 -- the default */

#define RADIO_NAME "RFM69"
#define RADIO_FIFO_SIZE RF69_FIFO_SIZE

static inline int radio_set_mode(enum radio_mode m)
{
	switch (m) {
	case RADIO_MODE_SLEEP:   return rf69_set_mode(RF69_MODE_SLEEP);
	case RADIO_MODE_STANDBY: return rf69_set_mode(RF69_MODE_STANDBY);
	case RADIO_MODE_RX:      return rf69_set_mode(RF69_MODE_RX);
	case RADIO_MODE_TX:      return rf69_set_mode(RF69_MODE_TX);
	default:                 return -EINVAL;
	}
}

static inline int  radio_config_916(void)            { return rf69_config_916(); }
static inline int  radio_set_freq(uint32_t hz)       { return rf69_set_freq(hz); }
static inline uint32_t radio_get_freq(void)          { return rf69_get_freq(); }
static inline int  radio_set_power_level(uint8_t l)  { return rf69_set_power_level(l); }
static inline int  radio_set_payload_len(uint8_t n)  { return rf69_set_payload_len(n); }
static inline int  radio_fifo_clear(void)            { return rf69_fifo_clear(); }
static inline int  radio_fifo_write(const uint8_t *d, uint16_t n)
						     { return rf69_fifo_write(d, n); }
static inline int  radio_fifo_write_byte(uint8_t b)  { return rf69_fifo_write_byte(b); }
static inline int  radio_fifo_read_byte(uint8_t *b)  { return rf69_fifo_read_byte(b); }
static inline bool radio_fifo_is_empty(void)         { return rf69_fifo_is_empty(); }
static inline bool radio_fifo_is_full(void)          { return rf69_fifo_is_full(); }
static inline bool radio_packet_sent(void)           { return rf69_packet_sent(); }
static inline int  radio_rx_irq_enable(struct k_sem *s) { return rf69_dio1_irq_enable(s); }
static inline int  radio_rx_irq_disable(void)        { return rf69_dio1_irq_disable(); }

/* rf69_read_rssi(false) reads RegRssiValue live rather than re-triggering a
 * measurement, which is what the receive path wants: sample the carrier that is
 * being received, not a fresh one after it has gone.
 */
static inline int16_t radio_sample_rssi(void)        { return rf69_read_rssi(false); }

static inline void radio_dump_regs(void) { rf69_dump_regs(); }
static inline int  radio_rssi_survey(int16_t *lo, int16_t *hi)
					 { return rf69_rssi_survey(lo, hi); }

#endif

#ifdef __cplusplus
}
#endif

#endif /* ORANGELINK_RADIO_H_ */
