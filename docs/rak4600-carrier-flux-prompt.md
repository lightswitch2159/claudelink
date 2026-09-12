# Flux AI prompt — RAK4600 carrier board

Paste the block below into Flux. Notes on what to check yourself follow it.

Design targets a **bench/test** sub-GHz↔BLE bridge. Not a medical device and not
for clinical or worn use.

---

## The prompt

```
Design a single-board carrier PCB for a RAKwireless RAK4600 module
(nRF52832 + SX1276), powered by one 1800 mAh 1S LiPo cell.

The RAK4600 is a stamp module: its SPI, DIO0-DIO4 and RF-switch connections to
the SX1276 are internal and must NOT be re-created. Only connect to the pins the
module exposes.

=== POWER ===

1. USB-C receptacle, POWER INPUT ONLY. No USB data: the nRF52832 has no USB
   peripheral. Fit 5.1 k CC1 and CC2 pulldowns to present a 5 V sink, and add
   USB ESD protection on VBUS.

2. Single-cell LiPo charger from VBUS, with power-path so the board runs from USB
   while charging and starts with a deeply discharged cell.
   Preferred: TI BQ24074 or BQ24075. Acceptable simpler alternative: MCP73831.
   Set charge current to 500 mA via the programming resistor. The cell is
   1800 mAh, so 500 mA is about 0.28 C -- conservative and safe. Do not design
   for 100 mA: that would take over 20 hours from empty.
   Bring the charger STATUS / CHG open-drain output to module pin P0.14 with a
   pull-up to the 3.3 V rail, so firmware can tell when it is charging.

3. Single-cell LiPo protection (BMS): over-charge, over-discharge, over-current
   and short-circuit, between the cell and the rest of the board.
   Use a TI BQ29700 (or equivalent single-cell protector plus dual N-FET, e.g.
   DW01A + FS8205). Place it directly at the cell connector.

4. 3.3 V regulator from the cell to power the module. The RAK4600 supply range is
   2.0-3.6 V, so it MUST NOT be fed 4.2 V directly. Use a low-dropout linear
   regulator rated at least 500 mA continuous -- TI TLV75733 or Diodes AP2112K-3.3.
   The SX1276 draws large current spikes on transmit, so place at least 10 uF of
   bulk capacitance plus a 100 nF decoupling capacitor as close to the module
   supply pin as possible.

5. JST-PH 2.0 mm 2-pin battery connector.

=== FUEL GAUGE ===

6. Maxim MAX17048 I2C fuel gauge at address 0x36.
   - Its supply/sense must be on the CELL side (before the regulator), because it
     derives state of charge from its own supply voltage.
   - I2C to module pins P0.12 = SCL and P0.13 = SDA, with 4.7 k pull-ups to the
     3.3 V rail -- not to the cell -- so the nRF52832 inputs are never driven
     above 3.3 V.
   - Bring the ALRT output to module pin P0.10 with a pull-up, as a low-battery
     interrupt.
   An ON Semiconductor LC709203F is an acceptable alternative.

=== INDICATOR ===

7. One RGB LED, common anode to the 3.3 V rail, so each colour is driven by the
   module sinking current (active low).
   - Red   -> P0.17
   - Green -> P0.18
   - Blue  -> P0.19
   Size the series resistors for about 2 mA per colour, accounting for the
   different forward voltages: roughly 650 ohm for red and 150 ohm for green and
   blue. The LED is used for brief 30 ms flashes, so low current is fine.

=== RF ===

8. A U.FL / IPEX coaxial connector for an external 916 MHz antenna, connected to
   the module's RF pin by a 50 ohm controlled-impedance trace kept as short as
   possible, with a solid ground plane beneath it and no other traces crossing it.

=== DEBUG AND CONTROL ===

9. SWD programming header: SWDIO, SWCLK, GND, 3.3 V and RESET, on a proper
   1.27 mm pitch pin header -- NOT bare test pads.
10. Momentary reset push-button to module pin P0.03, which is the configured MCU
    reset, plus a 100 nF debounce capacitor to ground.
11. UART debug header exposing P0.22 (TX) and P0.23 (RX) with GND.
12. Test points for BAT+, the 3.3 V rail, and GND.

=== DO NOT INCLUDE ===

- No vibration motor and no buzzer.
- No display.
- No USB data lines, and no USB-based firmware update path.
- No second radio and no external SPI flash.
- Do not add an analog battery-voltage divider. The fuel gauge replaces it, and
  the only analog-capable exposed pin (P0.03 / AIN1) is needed for reset.

=== PINS THAT MUST BE LEFT UNCONNECTED ===

P0.04, P0.05, P0.06, P0.07, P0.15, P0.16, P0.27, P0.28, P0.29, P0.30 and P0.31
are used internally by the module for the SX1276 SPI bus, the RF switch and the
DIO lines. Leave them alone.

Remaining free pins for expansion: P0.09 and P0.23 if UART is not fitted.

Prioritise a compact two-layer board with a continuous ground plane on the
bottom layer.
```

---

## Check these yourself — do not take the AI's word

**Confirm the MCU variant first.** The whole design assumes nRF52832 **QFAA**
(512 KB flash / 64 KB RAM). On **QFAB** (256 KB / 32 KB) the firmware does not
fit at all -- measured RAM use is 50.4 KB, or 40.7 KB trimmed. Neither datasheet
states which is fitted. This is binary and worth settling before laying out a
board. See MIGRATION_NOTES 19.6.

**RF is not an AI job.** The antenna trace, its impedance and the ground plane
under it decide whether the thing reaches a pump. Review that by hand.

**Fuel gauge level shifting.** The MAX17048 is powered from the cell (up to 4.2 V)
while the nRF52832 I/O is 3.3 V. Pull-ups to 3.3 V should satisfy its input
thresholds, but verify VIH/VIL against the datasheet rather than assuming.

**The protection IC may be redundant.** Most 1800 mAh pouch cells ship with a
protection module already fitted. A second one on the board is harmless, but
check before paying for it.

**RESET wiring is an open question.** The RAK4600 datasheet shows SPI, DIO0-DIO4
and the RF switch, but no SX1276 RESET line to the MCU. GNARL hardware-resets the
SX1276 at init, so if that line is genuinely absent the driver has to work from
power-on reset alone. See docs/sx1276-reference.md.

## Firmware consequences

Adopting this board changes three things in the tree:

* `src/battery/` drops the ADC path -- divider arithmetic, the `lipo_curve` table
  and the calibration logging -- and reads the gauge instead. Measured cost of the
  fuel gauge was +3540 B flash and +128 B RAM, partly offset by deleting the above.
* The charge-current select GPIO disappears. Charge current is set by a resistor
  at the charger, not by firmware, so `CONFIG_ORANGELINK_BATTERY_FAST_CHARGE` and
  the `chg_current` node both go away.
* A new charger-status input on P0.14 lets the LED distinguish "charging" from a
  charge level, which the XIAO could not do.
