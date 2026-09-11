# Carrier board: port map and parts

Port map is authoritative -- taken from `boards/xiao_ble.overlay` and
`src/battery/battery.c` as built. Parts are chosen for JLCPCB assembly and the
LCSC numbers were checked against JLC's parts library, but **confirm stock and
Basic/Extended status at order time**; Extended parts carry a per-reel setup fee
and stock moves.

Assumes the revised architecture: the cell lives on the carrier, the module's
`B+`/`B-` are unconnected and its USB-C unused.

## 1. XIAO nRF52840 port map

All 14 castellated pads. "Fixed" means the firmware already assigns it and the
board must match; "new" means it is being added for this carrier.

| Pad | nRF52840 | Analog | Net | Direction | Status |
|---|---|---|---|---|---|
| D0 | P0.02 | AIN0 | RFM69 `NSS` | out | fixed |
| D1 | P0.03 | **AIN1** | Battery sense divider tap | analog in | **new** |
| D2 | P0.28 | AIN4 | RFM69 `DIO1` | in, IRQ | fixed |
| D3 | P0.29 | AIN5 | Charger `STAT` | in | **new** |
| D4 | P0.04 | **AIN2** | Spare, to expansion header | -- | **free** |
| D5 | P0.05 | AIN3 | Buzzer drive | out, PWM | fixed |

| D6 | P1.11 | none | RFM69 `DIO0` (see note) | in | suggested |
| D7 | P1.12 | none | Spare, to expansion header | -- | free |
| D8 | P1.13 | none | RFM69 `SCK` | out | fixed |
| D9 | P1.14 | none | RFM69 `MISO` | in | fixed |
| D10 | P1.15 | none | RFM69 `MOSI` | out | fixed |
| 3V3 | -- | -- | **Carrier 3V3 rail (input to module)** | power in | **new** |
| GND | -- | -- | Ground | -- | -- |
| 5V | -- | -- | **NO CONNECT** | -- | **must stay unconnected** |

Notes:

* **D1 is the only sensible battery-sense pin.** Of the pins that were free, only
  D1 (AIN1) and D3 (AIN5) are analog capable at all -- D6 and D7 are on port 1,
  which has no ADC. Do not swap D1 and D6.
* **D4 is free** now the motor is out of scope, and it is `AIN2`, so it is the
  obvious home for a second analog input if one is ever wanted. Bring it to the
  expansion header rather than leaving it unrouted.
* **`5V` must be left unconnected.** Driving it energises the module's VBUS and
  wakes its BQ25101 charger with no cell attached. That is the single most
  important line in this table.
* **D6 / DIO0** is a suggestion, not a requirement: DIO0 carries `PacketSent`,
  which the firmware currently polls for. Routing it costs nothing and would let
  TX completion become interrupt-driven later. Leave it as a no-fit if you prefer.
* Not on the header, and now unused: P0.31 (AIN7, the module's own battery
  divider), P0.14 (its sense enable), P0.13 (its charge-current select). All three
  become dead once `B+` is unconnected.
* P0.26 / P0.30 / P0.06 drive the module's onboard RGB LED, used for battery
  status. No indicator LEDs needed on the carrier beyond charger status.

### SWD -- worth doing properly

The module brings SWDIO/SWCLK out only as **small test pads on its underside**,
which face the carrier when it is soldered down. Place matching pads on the
carrier under the module footprint and route them to a 4-pin header (SWDIO, SWCLK,
GND, 3V3). Confirm pad positions against Seeed's mechanical drawing.

This is not a nicety. Flashing over flying leads to those pads is how this project
bricked a board mid-session; a proper header would have avoided it.

## 2. RFM69HCW connections

| Module pin | Goes to | Note |
|---|---|---|
| `VCC` | 3V3 rail | 100 nF + 10 uF local; see current note below |
| `GND` | Ground | multiple vias, solid pour |
| `SCK` | D8 / P1.13 | |
| `MISO` | D9 / P1.14 | |
| `MOSI` | D10 / P1.15 | |
| `NSS` | D0 / P0.02 | |
| `DIO0` | D6 / P1.11 | optional, `PacketSent` |
| `DIO1` | D2 / P0.28 | **required**, `FifoNotEmpty` |
| `DIO2..5` | no connect | |
| `RESET` | 10k pull-down to GND | firmware never asserts it |
| `ANT` | u.FL centre pin | 50 ohm, see the antenna section in the brief |

**Supply current:** the HCW transmits at up to +20 dBm and this firmware sends
wake bursts of roughly 200 frames, so it draws on the order of 130 mA for
**several seconds continuously**. That is not a short pulse the decoupling can
absorb -- size the 3V3 rail for it.

## 3. Bill of materials

Verified present in JLC's library. Prices and Basic/Extended status change, so
re-check before ordering.

| Function | Part | LCSC | Notes |
|---|---|---|---|
| Radio | RFM69HCW-915S2R | **C5189430** | HopeRF, JLC-assemblable. **Check whether your variant has a bare `ANT` pad or its own IPEX** -- if it has IPEX, skip the carrier u.FL |
| Charger + power path | BQ24075RGTR | **C15464** | 1.5 A, DPPM power path, VQFN-16 |
| Protection IC | DW01A | **C351410** | SOT-23-6 |
| Protection FETs | FS8205A | **C16052** | SOT-23-6, pairs with DW01A |
| 3V3 LDO | AP2112K-3.3TRG1 | **C51118** | 600 mA, 250 mV dropout at full load, SOT-23-5 |
| USB-C receptacle | SHOU HAN TYPE-C 6P(073) | **C668623** | 6-pin, power only: VBUS, GND, CC1, CC2. No data lines |
| VBUS TVS | *optional*, any ~6 V unidirectional SOD-123 | -- | See the ESD note below; not required |
| u.FL / IPEX MHF1 | I-PEX 20279-001E-03 | **C3173448** | 50 ohm, DC-9 GHz |
| Buzzer FET | AO3400A | **C20917** | logic-level N-ch, JLC Basic. Only needed for a magnetic buzzer; a piezo can be driven from D5 directly |

Still to pick, deliberately not asserted here because I did not verify them:
a 2-pin JST-PH 2.0 battery connector, the buzzer itself, and 0603 passives. All
are trivially available; take them from JLC's Basic library so they cost nothing
extra.

## 4. Key component values

### Battery sense divider
* **1 Mohm high side, 1 Mohm low side**, from the **cell** (battery side of the
  protection FETs, so it reads the cell and not the charger output) to D1.
* 4.2 V cell gives 2.1 V at the tap. Firmware samples at gain 1/4 against the
  0.6 V internal reference -- a 2.4 V full scale -- so that uses most of the range
  with headroom.
* **100 nF from the tap to ground is required.** The nRF52840 SAADC is a
  switched-capacitor input and will not settle from a 500 kohm source without it.
* Standing drain about 2.1 uA. Do not be tempted by 100k/100k: 21 uA would be a
  large fraction of a board idle budget now in the tens of microamps.

### BQ24075 programming
Work these out against the datasheet rather than trusting the arithmetic here --
I am recalling the constants, not reading them:

* `ISET` sets fast-charge current as `I = K_ISET / R_ISET` with K_ISET around
  890 A-ohm. For 0.5C on an 1800 mAh cell (900 mA) that is roughly **1 kohm**.
* `ILIM` sets the input current limit similarly, K_ILIM around 1550 A-ohm.
* **`TS` is the classic way to get a board that refuses to charge.** The thermistor
  input must sit in a valid window or charging is inhibited. Either fit a 10k NTC
  against the cell, or bias `TS` with fixed resistors per the datasheet's
  disable procedure. Check this before you send the board.
* `EN1`/`EN2` select the input-limit mode; set them per the datasheet table for
  resistor-programmed `ILIM`, not left floating.
* `SYSOFF` tied low for normal operation.

### USB-C
Using the 6-pin **C668623** rather than a 16-pin part. It carries VBUS, GND, CC1
and CC2 only -- no D+/D-, no SBU.

* **5.1 kohm from CC1 to GND and 5.1 kohm from CC2 to GND**, separately -- not one
  resistor shared between them. Without these a USB-C source supplies nothing.
  This does not change with the 6-pin connector.
* **Check the mechanical retention.** A 6-pin part has fewer anchors than a 16-pin
  one, and this is a charging port that gets plugged and unplugged repeatedly.
  Confirm the 073 series' board-lock tabs are in your footprint and that the pads
  are generous. LCSC USB-C footprints are a known source of trouble -- worth
  checking against a community footprint library rather than trusting the
  datasheet drawing alone.

### ESD: why the USBLC6 came out
Dropping the dedicated ESD array is justified here, but not because the connector
protects anything. Two separate reasons:

1. **The lines it existed to protect are gone.** USBLC6-2SC6 is a two-channel
   array for D+/D- plus a VBUS clamp. With a power-only connector, two of its
   three jobs do not exist.
2. **CC1/CC2 terminate in nothing but resistors.** They go to a 5.1 kohm pull-down
   each and nowhere else -- there is no CC decoder, no IC pin on that net. A strike
   there has nothing to damage. *This is the part that would change* if a CC
   decoder were ever added for higher-current negotiation: CC would then reach a
   silicon input and would want protection.

What remains exposed is **VBUS**, and that goes somewhere sensitive. It is covered
reasonably well already: the BQ24075 is rated to 28 V on `IN` with overvoltage
protection, and the input bulk capacitance absorbs the low-energy part of a strike.

A ~6 V unidirectional TVS on VBUS to ground is therefore **optional** -- a few cents
of insurance on the one line that matters, not a correctness requirement. Fit the
footprint and decide at assembly if you want the option.
* Note that nothing here reads the CC lines, so the board cannot legitimately
  negotiate above the default. The BQ24075's DPPM handles this gracefully: if the
  source cannot deliver, VIN droops and it reduces charge current rather than
  collapsing. Add a CC decoder only if you want guaranteed high-current charging.

### Buzzer
A piezo element can be driven straight from D5. A magnetic buzzer needs current the
GPIO cannot supply, so use the AO3400A low-side with a ~100 ohm gate resistor, a
**10 kohm gate-to-source pull-down** so it cannot sound while the nRF is in reset,
and a flyback diode across the coil. Either way it runs from 3V3; the load is small
enough that there is no reason to tap `SYS`.

## 5. Power architecture

```
USB-C ──► BQ24075 ──► SYS ──► AP2112K-3.3 ──► 3V3 ──┬──► XIAO 3V3 pad
   │         │                                       ├──► RFM69HCW
  CC 5k1    ISET/ILIM                                └──► buzzer
                     │
                     └──► BAT ──► DW01A + FS8205A ──► cell ──► 1M/1M ──► XIAO D1
```

The LDO choice trades the bottom of the cell's range for RF quietness: it drops
out near 3.5 V, giving up roughly the last 10%, but it puts no switching noise
next to a 916 MHz receiver. With an 1800 mAh cell and microamp idle, runtime is
not the binding constraint. Swap to a buck-boost only if that changes.

## 6. Order of things to get right

Roughly by how expensive the mistake is:

1. `5V` pad unconnected, `B+`/`B-` unconnected.
2. CC resistors, both of them, 5.1k to ground, one per line.
3. `TS` biased so the charger will actually charge.
4. Divider on the cell side of the protection FETs, with its 100 nF.
5. u.FL keep-out honoured, 50 ohm feed, ground plane continuous beneath it.
