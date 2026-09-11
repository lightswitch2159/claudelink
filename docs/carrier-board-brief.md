# Carrier board brief (prompt for Flux)

Paste the block below into Flux. Everything in it is derived from the firmware as
built: pin assignments come from `boards/xiao_ble.overlay`, the charge-current and
battery-sense behaviour from `src/battery/battery.c`.

## Read this first: the carrier owns the power path

The cell connects to the **carrier only**. The XIAO's `B+`/`B-` pads are left
unconnected and its USB-C is unused, so its onboard BQ25101 charger never has
either a supply or a battery and stays out of the design entirely. That removes the
two-chargers-on-one-cell hazard by construction rather than by a usage rule.

The consequence is that **both of the XIAO's battery facilities stop working**, and
the carrier has to replace one of them:

* P0.31 / AIN7 reads the cell through the module's own divider off `B+`. With `B+`
  unconnected it reads nothing, so **the carrier must provide its own divider** to
  a free analog pin.
* P0.13 selects 50/100 mA on the module's charger. Irrelevant once that charger is
  unused; charge current is set by a resistor on the carrier instead.

Power architecture this implies:

```
USB-C ──► charger (power path) ──► SYS ──► LDO 3V3 ──► XIAO 3V3 pin + RFM69HCW
                └──► BAT ──► 1S protection ──► cell ──► divider ──► XIAO AIN1
```

Two choices in there worth knowing about, both argued in the prompt:

1. **The XIAO is fed on its `3V3` pin, not its `5V` pin.** Feeding `5V` would
   energise the module's VBUS and wake its charger with no battery attached.
   Feeding `3V3` back-drives the module's LDO output, which is accepted practice
   for these modules provided USB is never connected at the same time.
2. **An LDO rather than a buck-boost.** A LiPo spans 4.2 V down to about 3.0 V and
   the RFM69HCW is only rated to 3.6 V, so regulation is required -- running
   straight off the cell is not an option. An LDO loses regulation near 3.5 V and
   so gives up roughly the last 10% of the cell, but it is quiet next to a 916 MHz
   receiver and far simpler. With idle current now in the tens of microamps and an
   1800 mAh cell, runtime is nowhere near the binding constraint, so the RF
   cleanliness is worth more than the capacity. Buck-boost is offered as the
   alternative if that judgement changes.

Why a charger on the carrier at all: the module's is capped at 100 mA, which is
about **23 hours** for an 1800 mAh cell against roughly **three** at 900 mA (0.5C).
That is stated in the prompt so the tool does not spec a small part.

Terminology: for a single cell, "BMS" means a protection IC (over-charge,
over-discharge, over-current, short-circuit). Cell balancing does not apply.

## Firmware changes this requires

The board below does not work with the firmware as it stands. Tracked here so the
two stay in step:

* `boards/xiao_ble.overlay` -- point `zephyr,user` `io-channels` at `&adc 1`
  (AIN1 = P0.03 = D1) instead of `&adc 7`; delete the `vbat_enable` and
  `chg_current` nodes.
* `src/battery/battery.c` -- drop `vbatt_enable` and `chg_current` entirely.
* `Kconfig` -- `ORANGELINK_BATTERY_OUTPUT_OHMS` / `_FULL_OHMS` become the carrier's
  divider values, and the calibration must be redone against a meter. Remove
  `ORANGELINK_BATTERY_FAST_CHARGE`.
* Optionally consume the charger `STAT` output as a GPIO input so the status LED
  can distinguish charging from discharging.
* Drop `&pwm1` and the `pwm1_default`/`pwm1_sleep` pinctrl entries from
  `boards/xiao_ble.overlay` -- that was the motor, now out of scope, and removing
  it releases D4/P0.04. There is no motor code in `src/` to delete; it only ever
  existed in devicetree.

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
> * USB-C receptacle, sink only, for charging and 5 V input. A **6-pin power-only
>   part is sufficient** (VBUS, GND, CC1, CC2) -- no USB data is needed anywhere on
>   this board. Include **5.1 kΩ pull-downs on both CC1 and CC2**, one per line, so
>   a USB-C source will actually supply current. A dedicated USB ESD array is not
>   needed with no data lines and CC terminating only in resistors; an optional
>   TVS on VBUS is reasonable insurance.
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
> * **System rail and regulation.** Take the charger's power-path output as a
>   `SYS` rail, and regulate it to 3.3 V with a low-dropout LDO sized for at least
>   250 mA continuous. This rail supplies the XIAO and the RFM69HCW. Prefer an LDO
>   over a switching regulator here: the RFM69HCW is a 916 MHz receiver and
>   switching noise is the greater risk, while runtime is not a constraint with an
>   1800 mAh cell. If you believe a buck-boost is justified to recover the bottom
>   of the cell's range, say so and explain the trade-off rather than silently
>   substituting one.
> * **Feed the XIAO on its `3V3` pin**, not its `5V` pin. Do not connect the
>   carrier's VBUS, `SYS` rail or charger output to the module's `5V` pin, and do
>   not add any path that could energise the module's VBUS from this board.
> * **Leave the module's `B+` and `B-` pads unconnected.** The cell belongs to the
>   carrier alone. The module's onboard charger must never see a supply or a
>   battery.
> * Note both restrictions in the silkscreen and in the design notes.
>
> ### Battery sense — the carrier must provide this
> With the cell off the module, the module's own battery divider is dead and the
> firmware needs a replacement analog input:
>
> * Resistive divider from the **cell** (battery side of the protection IC, so it
>   reads the cell rather than the charger output) to **D1 / P0.03 / AIN1**.
> * Use **1 Mohm / 1 Mohm**, giving 2.1 V at a full 4.2 V cell. The firmware will
>   sample it at ADC gain 1/4 against the 0.6 V internal reference, a 2.4 V full
>   scale, so that ratio uses most of the range with headroom to spare. High values
>   deliberately: the standing drain is about 2 uA, against a board idle budget
>   measured in tens of microamps. Do not use 100k/100k -- 21 uA would be a
>   significant fraction of total idle current.
> * Place a **100 nF capacitor from the divider tap to ground.** The nRF52840 SAADC
>   is a switched-capacitor input and cannot settle from a 500 kohm source without
>   it.
> * D1/P0.03 is chosen because it is one of only two free pins that are analog
>   capable: of D1 (P0.03 = AIN1), D3 (P0.29 = AIN5), D6 (P1.11) and D7 (P1.12),
>   the P1 pins have no ADC. Do not reassign this to a P1 pin.
> * Bring the charger's `STAT` output to **D3 / P0.29** as a GPIO input so firmware
>   can tell charging from discharging.
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
> ### Antenna
> * Fit a **u.FL / IPEX MHF1 surface-mount connector** as the antenna interface.
>   This is the primary connector and must be populated. Place it at a board edge
>   so a pigtail can exit cleanly, oriented so the cable does not run back across
>   the radio or the USB connector.
> * Honour the connector's keep-out: no copper, silkscreen or components inside
>   the footprint's exclusion area on any layer, per the manufacturer's datasheet.
> * Route from the RFM69HCW module's antenna pad to the u.FL centre pin as a
>   **50 ohm controlled-impedance transmission line**, as short and straight as
>   possible, with no stubs, no vias if it can be avoided, and a continuous
>   unbroken ground reference directly beneath it for the whole run. Flood ground
>   either side with stitching vias along the length (coplanar waveguide), and
>   keep the trace away from the regulator, the charger and the USB lines.
> * Optionally also provide an **unpopulated edge-mount SMA footprint** on the
>   same net as a build-time alternative, but design the trace for the u.FL path;
>   do not route a T-junction feeding both, as the unused branch becomes a stub.
> * If the chosen RFM69HCW variant already carries its own u.FL connector rather
>   than a bare antenna pad, say so and adjust: in that case the carrier's u.FL
>   is redundant and the module's own connector should be used instead.

**Impedance note for the stackup:** a 50 ohm microstrip on 1.6 mm two-layer FR4
works out around 2.9 mm wide, which is not practical next to a u.FL pad. Use a
four-layer stackup with a thin prepreg to layer 2 ground (roughly 0.9 mm trace for
50 ohm), or a coplanar waveguide with tight ground clearance on two layers. State
the stackup, dielectric and the calculated trace width in the design notes.
> * The RFM69HCW transmits at up to +20 dBm and draws roughly 130 mA in bursts
>   lasting several seconds. Size the power path and decoupling for that: bulk
>   capacitance local to the radio plus the usual 100 nF per supply pin.
> * RESET may be tied to its inactive state; the firmware never asserts it.
>
> ### Other I/O
> * **Piezo buzzer** on D5 (P0.05), driven straight from the GPIO. A piezo is
>   capacitive and needs no driver transistor, flyback diode or gate pull-down.
>   Do not substitute a magnetic buzzer, which would need all three.
> * D1 (P0.03) and D3 (P0.29) are taken by battery sense and charger status above.
>   Leave D4 (P0.04), D6 (P1.11) and D7 (P1.12) unassigned, brought out to a
>   0.1 inch expansion header with 3V3 and GND. D4 is analog capable (AIN2), so
>   keep it free rather than using it for a digital-only function.
> * Bring SWDIO, SWCLK, RESET, 3V3 and GND to a standard debug header. The module
>   only exposes these as tiny underside test pads, which are painful to solder
>   by hand and have already cost this project a bricked board.
> * The module has its own RGB LED used for battery status, so no indicator LEDs
>   are needed on the carrier beyond a charge-status LED.
>
> ### General
> * **Prefer a four-layer stackup**, primarily so the antenna trace has a solid
>   ground plane on layer 2 at a spacing that makes a 50 ohm line a sensible
>   width -- see the impedance note under Antenna. Two layers is acceptable only
>   with a coplanar-waveguide feed and the stackup stated explicitly.
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
2. **No path from carrier VBUS or SYS to the module's `5V` pin, and `B+`/`B-`
   genuinely unconnected.** Verify both in the netlist, not the prose -- this is
   what keeps the module's charger out of the design.
3. **Battery divider taps the cell, not the charger output**, lands on D1/P0.03,
   and has its 100 nF to ground. A divider on the wrong side of the protection IC
   reads the charger during charging and looks plausible while being wrong.
4. **Protection IC on the cell side of the charger**, not between charger and load.
5. **Charge current resistor** actually computes to ~900 mA for the chosen part --
   check it against that part's datasheet formula rather than trusting the value.
6. **Antenna trace**: impedance actually calculated for the stated stackup rather
   than asserted, continuous ground directly under the whole run, no stub branch
   to an unpopulated SMA, u.FL keep-out respected on every layer, and the RF
   section not routed under or beside the switcher.
7. **Piezo, not magnetic, on D5** -- a magnetic buzzer silently exceeds what the
   GPIO can source and would need a FET the BOM no longer carries.
