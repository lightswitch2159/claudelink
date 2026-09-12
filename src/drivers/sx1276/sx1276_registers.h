/*
 * Semtech SX1276 register map -- FSK/OOK mode only.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Only the registers this driver touches. The LoRa register bank is deliberately
 * absent: LoRa shares the address space but means different things, and this
 * driver never leaves FSK/OOK mode.
 *
 * Cross-checked against two sources, because a wrong constant here fails
 * silently on air rather than at build time:
 *   - modules/lib/loramac-node/src/radio/sx1276/sx1276Regs-Fsk.h (Semtech)
 *   - https://github.com/ecc1/gnarl  lib/radio/rfm95.h (MIT), which is a
 *     working SX1276 OOK driver for Medtronic pumps. See
 *     docs/sx1276-reference.md.
 */

#ifndef ORANGELINK_SX1276_REGISTERS_H_
#define ORANGELINK_SX1276_REGISTERS_H_

/* ---- addresses ---------------------------------------------------------- */
#define SX_REG_FIFO             0x00
#define SX_REG_OPMODE           0x01
#define SX_REG_BITRATEMSB       0x02
#define SX_REG_BITRATELSB       0x03
#define SX_REG_FRFMSB           0x06
#define SX_REG_FRFMID           0x07
#define SX_REG_FRFLSB           0x08
#define SX_REG_PACONFIG         0x09
#define SX_REG_PARAMP           0x0A
#define SX_REG_OCP              0x0B
#define SX_REG_LNA              0x0C
#define SX_REG_RXCONFIG         0x0D
#define SX_REG_RSSICONFIG       0x0E
#define SX_REG_RSSIVALUE        0x11
#define SX_REG_RXBW             0x12
#define SX_REG_AFCBW            0x13
#define SX_REG_OOKPEAK          0x14
#define SX_REG_OOKFIX           0x15
#define SX_REG_OOKAVG           0x16
#define SX_REG_PREAMBLEDETECT   0x1F
#define SX_REG_PREAMBLEMSB      0x25
#define SX_REG_PREAMBLELSB      0x26
#define SX_REG_SYNCCONFIG       0x27
#define SX_REG_SYNCVALUE1       0x28
#define SX_REG_SYNCVALUE2       0x29
#define SX_REG_SYNCVALUE3       0x2A
#define SX_REG_SYNCVALUE4       0x2B
#define SX_REG_PACKETCONFIG1    0x30
#define SX_REG_PACKETCONFIG2    0x31
#define SX_REG_PAYLOADLENGTH    0x32
#define SX_REG_FIFOTHRESH       0x35
#define SX_REG_SEQCONFIG1       0x36
#define SX_REG_IRQFLAGS1        0x3E
#define SX_REG_IRQFLAGS2        0x3F
#define SX_REG_DIOMAPPING1      0x40
#define SX_REG_DIOMAPPING2      0x41
#define SX_REG_VERSION          0x42
#define SX_REG_PADAC            0x4D

/* ---- RegOpMode ----------------------------------------------------------
 *
 * The modulation selection lives HERE, not in a separate register as on the
 * SX1231. Every mode write must therefore re-assert FSK_OOK | MODULATION_OOK or
 * the part silently reverts toward its LoRa default.
 */
#define SX_OPMODE_FSK_OOK       (0 << 7)   /* LongRangeMode = 0 */
#define SX_OPMODE_MODULATION_FSK (0 << 5)
#define SX_OPMODE_MODULATION_OOK (1 << 5)
#define SX_OPMODE_MASK          0x07
#define SX_MODE_SLEEP           0x00
#define SX_MODE_STANDBY         0x01
#define SX_MODE_FSTX            0x02
#define SX_MODE_TX              0x03
#define SX_MODE_FSRX            0x04
#define SX_MODE_RX              0x05

/* ---- RegLna ------------------------------------------------------------- */
#define SX_LNA_GAIN_MAX         (1 << 5)

/* ---- RegRxConfig -------------------------------------------------------- */
#define SX_RXCONFIG_AFC_AUTO_ON (1 << 4)
#define SX_RXCONFIG_AGC_AUTO_ON (1 << 3)
#define SX_RXCONFIG_TRIGGER_PREAMBLE 0x06
#define SX_RXCONFIG_TRIGGER_RSSI     0x01

/* ---- RegRxBw ------------------------------------------------------------
 *
 * BW = FXOSC / (mantissa * 2^(exponent + 2)).
 *
 * NOTE the exponent term: the SX1231 uses 2^(exp+3) in OOK, so its exponent for
 * the same bandwidth is one LOWER. Carrying our SX1231 value across would give
 * 100 kHz, not 200 kHz, and present as poor sensitivity rather than a bad
 * register. mantissa 20, exponent 1 -> 32 MHz / (20 * 8) = 200 kHz.
 */
#define SX_RXBW_MANT_SHIFT      3
#define SX_RXBW_MANT_16         (0 << SX_RXBW_MANT_SHIFT)
#define SX_RXBW_MANT_20         (1 << SX_RXBW_MANT_SHIFT)
#define SX_RXBW_MANT_24         (2 << SX_RXBW_MANT_SHIFT)

/* ---- RegOokPeak / RegOokAvg --------------------------------------------- */
#define SX_OOKPEAK_BITSYNC_ON       (1 << 5)
#define SX_OOKPEAK_THRESHTYPE_PEAK  (1 << 3)
#define SX_OOKAVG_OFFSET_0DB        (0 << 2)

/* ---- RegPreambleDetect --------------------------------------------------- */
#define SX_PREAMBLE_DETECT_ON   (1 << 7)
#define SX_PREAMBLE_DETECT_3B   (2 << 5)

/* ---- RegSyncConfig ------------------------------------------------------- */
#define SX_SYNC_ON              (1 << 4)
#define SX_SYNC_SIZE_4          0x03       /* field is size-1 */

/* ---- RegPacketConfig1/2 -------------------------------------------------- */
#define SX_PACKET1_FORMAT_FIXED (0 << 7)
#define SX_PACKET1_DCFREE_OFF   (0 << 5)
#define SX_PACKET1_CRC_OFF      (0 << 4)
#define SX_PACKET2_PACKET_MODE  (1 << 6)

/* ---- RegFifoThresh ------------------------------------------------------- */
#define SX_FIFOTHRESH_TXSTART_FIFOLEVEL (0 << 7)
#define SX_FIFOTHRESH_TXSTART_NOTEMPTY  (1 << 7)

/* ---- RegSeqConfig1 ------------------------------------------------------- */
#define SX_SEQ_START            (1 << 7)
#define SX_SEQ_STOP             (1 << 6)
#define SX_SEQ_IDLE_STANDBY     (0 << 5)
#define SX_SEQ_IDLE_SLEEP       (1 << 5)
#define SX_SEQ_FROM_START_TO_TX (2 << 3)

/* ---- RegIrqFlags1 -------------------------------------------------------- */
#define SX_IRQ1_MODEREADY       (1 << 7)
#define SX_IRQ1_RXREADY         (1 << 6)
#define SX_IRQ1_TXREADY         (1 << 5)
#define SX_IRQ1_SYNCMATCH       (1 << 0)

/* ---- RegIrqFlags2 -------------------------------------------------------- */
#define SX_IRQ2_FIFOFULL        (1 << 7)
#define SX_IRQ2_FIFOEMPTY       (1 << 6)
#define SX_IRQ2_FIFOLEVEL       (1 << 5)
#define SX_IRQ2_FIFOOVERRUN     (1 << 4)
#define SX_IRQ2_PACKETSENT      (1 << 3)
#define SX_IRQ2_PAYLOADREADY    (1 << 2)

/* ---- RegDioMapping1 ------------------------------------------------------ */
#define SX_DIO0_SHIFT           6
#define SX_DIO1_SHIFT           4
#define SX_DIO2_SHIFT           2
#define SX_DIO2_SYNCADDRESS     (3 << SX_DIO2_SHIFT)

/* ---- RegPaConfig / RegPaDac ---------------------------------------------- */
#define SX_PACONFIG_PABOOST     (1 << 7)
#define SX_PADAC_20DBM_ON       0x07
#define SX_PADAC_20DBM_OFF      0x04

/* SX1276 silicon revision reported in RegVersion. */
#define SX1276_VERSION          0x12

/* SX1276_FIFO_SIZE lives in sx1276.h -- it is API, not a register. */

#endif /* ORANGELINK_SX1276_REGISTERS_H_ */
