# SX1276 OOK: prior art and the deltas from our SX1231 driver

## The reference

[**GNARL** — "GNARL is Not A RileyLink"](https://github.com/ecc1/gnarl) by ecc1.
ESP32 (ESP-IDF, C) driving an **RFM95 / SX1276 in OOK** to talk to Medtronic pumps
at 916 MHz — the same problem this project solves with an SX1231/RFM69. Radio
driver is `lib/radio/rfm95.c` + `rfm95.h`, about 385 lines.

**Licence: MIT**, so it can be used as a reference or adapted with attribution into
this GPL-2.0 project.

Related: `lib/medtronic/` in the same repo carries an independent implementation of
4b6b, CRC and the pump command set — useful for cross-checking
`src/encoding/4b6b.c` and `tools/minimed.py`.

## Why this matters more than a datasheet read

Six things in GNARL's working configuration differ from what a straight
transcription of our SX1231 table would have produced. Each is a silent failure.

**1. Sleep twice before selecting FSK/OOK.** GNARL's own comment: *"Must be in Sleep
mode first before the second call can change to FSK/OOK mode."* `LongRangeMode`
only changes in Sleep. The SX1231 has no such rule.

**2. The modulation bits live in `RegOpMode`.** Every mode write is
`FSK_OOK_MODE | MODULATION_OOK | mode`, so modulation is re-asserted on each mode
change. On the SX1231 it is a separate `RegDataModul` written once at init.

**3. RxBw exponent is not the same number.** 200 kHz is mantissa 20, **exp 1** on
SX1276; our SX1231 table uses **exp 0**. The formulas differ -- SX1231 OOK uses
`2^(exp+3)`, SX127x uses `2^(exp+2)`. Copying the SX1231 value gives half the
intended bandwidth, which would look like poor sensitivity rather than a
misconfiguration.

**4. Preamble is 24 bytes (`0x18`), not 16.** GNARL: *"Make sure enough preamble
bytes are sent."*

**5. Unlimited-length packet mode.** `PACKET_FORMAT_FIXED` with
`PAYLOAD_LENGTH = 0` (datasheet 4.2.13.2), rather than our fixed length set per
frame. Worth understanding before choosing; this port's payload-length handling was
itself the subject of a 136 ms -> 13 ms fix (MIGRATION_NOTES 9.4).

**6. TX runs off the sequencer.** `SEQUENCER_START | IDLE_MODE_STANDBY |
FROM_START_TO_TX` auto-enters TX on the FifoLevel condition, and "transmit done" is
detected by polling for the mode falling back to STANDBY -- not by `PacketSent`.

## Independent confirmation of two of our own findings

* **RSSI is read immediately after sync match**, before the FIFO is drained. That is
  exactly the bug fixed in MIGRATION_NOTES 13.3, where reading RSSI after draining
  measured the noise floor on ~43% of replies.
* **The end-of-packet glitch trim is identical** -- a trailing `0x80` or `0xC0` is
  discarded as an OOK demodulation artefact. We inherited that from the legacy
  firmware; GNARL arrived at it separately.

Both agree that a zero byte terminates the packet, and the FRF encoding is the same
(FXOSC 32 MHz, `f = freq << 19 / FXOSC`).

## New open question for RAK4600

GNARL performs a **hardware reset** of the SX1276 at init (`rfm95_reset()`: drive
RST low ~100 us, release, wait 5 ms). The RAK4600 datasheet lists SPI, DIO0-DIO4
and the RF switch lines, but **does not show a RESET connection** to the nRF52832.
If reset is not wired, init has to work from power-on reset alone. Worth confirming
before committing to that module.
