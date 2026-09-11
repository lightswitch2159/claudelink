# Carrier board brief (prompt for Flux)

Paste the block below into Flux. Everything in it is derived from the firmware as
built: pin assignments come from `boards/xiao_ble.overlay`, the charge-current and
battery-sense behaviour from `src/battery/battery.c`.

## Read this first: the charger conflict

**The XIAO nRF52840 already has a USB-C connector and a BQ25101 charger on it.**
Adding a second charger to the same cell is the main hazard in this design: two
chargers driving one LiPo is not a configuration either one is designed for.

The resolution below avoids it without any firmware change, and the reasoning
matters enough that it is stated as a hard constraint in the prompt:

* The cell connects to the carrier's protection + charger **and** to the XIAO's
  `B+`/`B-` pads on the same net.
* The XIAO's BQ25101 only charges when *its own* VBUS is powered. With the XIAO's
  USB-C left unused, it is inert, and the carrier's charger has the cell to itself.
* Because the cell is still on `B+`, the XIAO's onboard divider on P0.31 keeps
  working, so `battery.c` needs no changes at all.

**The one rule this creates: never plug into both USB-C ports at once.** Worth
physically blocking the XIAO's connector in any enclosure.

Why bother with a second charger at all: the XIAO's charger tops out at 100 mA, and
the cell is 1800 mAh. That is roughly **23 hours** from empty. At 900 mA (0.5C) it
is about three. That is the whole motivation, and it is stated in the prompt so the
tool does not "helpfully" spec a 100 mA part.

Note also that `CONFIG_ORANGELINK_BATTERY_FAST_CHARGE` (P0.13) selects current on
the XIAO's *inert* charger once this board exists. Harmless, and left in place.

Terminology: for a single cell, "BMS" means a protection IC (over-charge,
over-discharge, over-current, short-circuit). Cell balancing does not apply.

---

## The prompt

> Design a carrier board for a Seeed Studio XIAO nRF52840 module. This is a
> sub-GHz to Bluetooth bridge: the module talks to an RFM69HCW radio at 916 MHz
> over SPI, and is powered by a 1S LiPo cell.
>
> ### Module mounting
> The XIAO nRF52840 mounts on the carrier via its 2x7 castellated pads (2.54 mm
> pitch, 7 per side). Provide both castellated-compatible pads and through-hole
> positions so it can be soldered down or socketed on headers. Break out the
> module's underside `B+` and `B-` battery pads to the carrier's battery net.
>
> ### Power and charging — the core requirement
> * USB-C receptacle, sink only, for charging and 5 V input. Include **5.1 kΩ
>   pull-downs on both CC1 and CC2** so a USB-C source will actually supply
>   current. Add ESD protection on the data/CC lines and VBUS.
> * A 1S LiPo charger IC with **programmable charge current set by resistor,
>   defaulted to approximately 900 mA** (0.5C for the 1800 mAh cell). Please use a
>   part with **integrated power path** so the board runs from USB while the cell
>   charges, and with thermal regulation. Texas Instruments BQ24075 or BQ25185 are
>   good candidates; MCP73831 is acceptable but its 500 mA ceiling and lack of
>   power path are a downgrade. Do not spec a 100 mA charger.
> * A **1S protection IC** (over-charge, over-discharge, over-current,
>   short-circuit) between the cell and the rest of the board, with its dual
>   N-channel MOSFET. Place it on the cell side of the charger so it protects in
>   both directions.
> * Expose charger `STAT`/`CHG` and `PGOOD` outputs on test pads or a header.
> * A JST-PH 2.0 mm connector for the cell, polarity marked in silkscreen.
>
> **Hard constraint — do not create a second charge path.** The XIAO module has
> its own USB-C and BQ25101 charger wired to the same `B+` net. Do not connect the
> carrier's VBUS, 5 V rail, or charger output to the module's `5V` pin, and do not
> add any path that could energise the module's VBUS from this board. The module's
> charger must remain unpowered and inert so only the carrier charger drives the
> cell. Note this restriction in the silkscreen and in the design notes.
>
> ### Sub-GHz radio
> RFM69HCW module (SMD, 915/916 MHz band), connected to the XIAO as follows.
> These assignments are fixed by firmware and must not be reassigned:
>
> | RFM69 | XIAO pin | nRF52840 |
> |---|---|---|
> | SCK | D8 | P1.13 |
> | MISO | D9 | P1.14 |
> | MOSI | D10 | P1.15 |
> | NSS (chip select) | D0 | P0.02 |
> | DIO1 | D2 | P0.28 |
>
> * Antenna: a u.FL/IPEX connector **and** an alternative SMA footprint, with a
>   50 ohm controlled-impedance trace, kept short and away from the switching
>   regulator and USB. Solid ground pour and stitching vias under the RF section.
> * The RFM69HCW transmits at up to +20 dBm and draws roughly 130 mA in bursts
>   lasting several seconds. Size the power path and decoupling for that: bulk
>   capacitance local to the radio plus the usual 100 nF per supply pin.
> * RESET may be tied to its inactive state; the firmware never asserts it.
>
> ### Other I/O
> * **Vibration motor** on D4 (P0.04): the XIAO GPIO cannot drive a motor
>   directly, so include a low-side N-channel MOSFET driver with a flyback diode
>   and a gate pull-down, plus a 2-pin connector.
> * **Buzzer** on D5 (P0.05): magnetic buzzer with a driver transistor, or a
>   piezo driven directly if that keeps it simpler.
> * Leave D1 (P0.03), D3 (P0.29), D6 (P1.11) and D7 (P1.12) unassigned, brought
>   out to a 0.1 inch expansion header with 3V3 and GND.
> * Bring SWDIO, SWCLK, RESET, 3V3 and GND to a standard debug header. The module
>   only exposes these as tiny underside test pads, which are painful to solder
>   by hand and have already cost this project a bricked board.
> * The module has its own RGB LED used for battery status, so no indicator LEDs
>   are needed on the carrier beyond a charge-status LED.
>
> ### General
> * Two-layer board is fine if the RF section can be kept clean; use four layers
>   if that gives a better ground plane under the radio.
> * All passives 0603 or larger for hand assembly.
> * Mounting holes: 4x M2, one near each corner.
> * Provide the schematic, a suggested placement, a BOM with manufacturer part
>   numbers, and design notes explaining the power path and the single-charger
>   constraint.

---

## What to check in whatever Flux returns

Automated tools get these wrong often enough to be worth a checklist:

1. **CC pull-downs present on both CC1 and CC2.** Without them a USB-C source
   supplies nothing and the board simply will not charge.
2. **No path from carrier VBUS to the module's `5V` pin.** This is the whole
   point of the constraint above; verify it in the netlist, not the prose.
3. **Protection IC on the cell side of the charger**, not between charger and load.
4. **Charge current resistor** actually computes to ~900 mA for the chosen part --
   check it against that part's datasheet formula rather than trusting the value.
5. **Antenna trace impedance** and that the RF section is not routed under or
   beside the switcher.
6. **Flyback diode across the motor**, and the MOSFET gate pulled down so the
   motor cannot twitch during reset.
