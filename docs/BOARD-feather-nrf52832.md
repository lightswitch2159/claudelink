# Adafruit Feather nRF52832 (3406) + RFM69HCW FeatherWing

Branch: `feather-nrf52832`. Keeps the **stock Adafruit bootloader** — no MCUboot.

## Why this board

The FeatherWing carries the same **SX1231** as the XIAO build, so
`src/drivers/rf69/` and the whole OOK datapath port unchanged. That is the reason
it was chosen over an SX1262 part (T-Echo, RAK4630), which cannot do OOK at all
and therefore cannot talk to a Medtronic pump — `sx126x.h` offers only
`PACKET_TYPE_GFSK` and `PACKET_TYPE_LORA`.

## Wiring

The FeatherWing's CS / IRQ / RST pads are solder-jumpers, so these are choices,
not defaults:

| RFM69 | Feather pin | nRF52832 | note |
|---|---|---|---|
| SCK | SCK | P0.12 | hardware SPI, no jumper |
| MOSI | MOSI | P0.13 | " |
| MISO | MISO | P0.14 | " |
| CS (NSS) | P0.11 | P0.11 | **jumper CS here** |
| DIO1 | P0.07 | P0.07 | **solder a wire** |
| RST | — | — | unused; the driver never asserts it |

**DIO1 is not optional.** The receive path waits on FifoNotEmpty via DIO1. The
FeatherWing only brings DIO0 out to IRQ, and DIO0's PayloadReady is useless under
our variable-length framing — without DIO1 the driver falls back to SPI polling,
which is what made the legacy firmware block for seconds at a time.

Pins avoided, and why: P0.17/P0.19 onboard LEDs, P0.20 DFU button, P0.06/P0.08
UART, P0.25/P0.26 I2C, P0.31 VBAT divider.

## Known differences from the XIAO build

- **Only two LEDs** (red P0.17, blue P0.19). There is no green, so the battery
  indicator's "green" is **blue** here and "yellow" is red+blue. The three states
  stay distinguishable; the colours just do not match the names.
- **No VBAT-enable and no charge-current pin.** Both are XIAO-specific;
  `src/battery/battery.c` now treats them as optional devicetree nodes.
- **Battery divider is 100k/100k** (VBAT/2), so ADC gain is 1/6 for a 3.6 V scale
  — a 4.2 V cell presents 2.1 V and would clip at gain 1/4. **Uncalibrated**:
  measure the cell and correct `FULL_OHMS` against the startup log.
- **No mcumgr/SMP DFU**, because there are no MCUboot slots to write into.

## Not UF2

The nRF52832 **has no USB peripheral** — `nrf52840.dtsi` has `usbd@40027000`,
`nrf52832.dtsi` has nothing — so the board reaches USB through a CP2104 bridge and
cannot enumerate as mass storage. Adafruit's UF2 bootloaders are nRF52840-only,
and Zephyr has no UF2 family ID for this part (asking for one fails the build with
`argument -f/--family: expected one argument`).

Update paths are **serial DFU** over the USB-serial bridge and **Adafruit BLE
OTA**. Neither needs a probe, and both survive a bad application image — which is
the property that motivated keeping the stock bootloader.

## Build

```bash
west build -b nrf52_adafruit_feather -d build-feather . -- \
  -DEXTRA_DTC_OVERLAY_FILE="$PWD/dts/feather-stock-bootloader.dtsi"
```

Measured: **FLASH 164064 B of 304 KB (52.7%)**, **RAM 37512 B of 64 KB (57.2%)**,
linking at `0x26000` (`CONFIG_FLASH_LOAD_OFFSET=0x26000`, first LOAD vaddr
`0x00026000`) — which is where the stock bootloader jumps.

For comparison, the same app with MCUboot dual-slot on this part needs 90.5% of a
slot and 77.5% of RAM. Dropping MCUboot and mcumgr is what buys the headroom.

## Flash

`adafruit-nrfutil` is **not currently installed**. Install it, package the hex,
and send it over the serial bridge:

```bash
pip install adafruit-nrfutil
adafruit-nrfutil dfu genpkg --dev-type 0x0052 \
  --application build-feather/orangelink-ncs/zephyr/zephyr.hex fw.zip
adafruit-nrfutil dfu serial --package fw.zip -p /dev/ttyUSB0 -b 115200 --singlebank
```

Double-tap reset first to enter the bootloader.

## VERIFY BEFORE FIRST FLASH

1. **The app start address.** `0x26000` is correct for a bootloader built against
   SoftDevice S132 6.x. Older Adafruit bootloaders differ. A wrong offset flashes
   cleanly and then does nothing. Check the bootloader version the board reports.
2. **That the stock bootloader is still present.** If this Feather has had Zephyr
   or MCUboot flashed onto it before, the bootloader may already be gone — and
   restoring it needs one SWD flash of Adafruit's release.

## Battery divider

Read off the **Rev G schematic**, LIPO MONITORING block:

```
VBAT --[ R4 806k ]--+--[ R6 2M ]-- GND
                    |
                  A7 / P0.31
```

```
CONFIG_ORANGELINK_BATTERY_OUTPUT_OHMS=2000000
CONFIG_ORANGELINK_BATTERY_FULL_OHMS=2806000      # 806k + 2M
```

Ratio 2M/2.806M = **0.7128**, so cell = pin x 1.4030. Adafruit annotate the same
drawing with *"Maximum voltage: 4.2 V \* (2 M/(0.8 M+2 M)) = 3 V"*, which agrees.

**This is not 100k/100k.** The board's text specs say the divider is 100k/100k with
a compensation factor of 2.0; the schematic says otherwise, and the hardware agrees
with the schematic. Assuming 0.5 made every reading **1.43x high** — 5542 mV against
a cell measured at 3860 mV — and the ratio error (0.7128/0.5 = 1.426) matches the
observed error (1.436) almost exactly.

Residual with the correct nominal values: **+0.72%**, which is resistor tolerance
(R4 is a 1% part). Trimming `FULL_OHMS` to 2786000 matches this particular unit
exactly, but that is a per-board refinement, not a fix for anything.

ADC gain 1/6 gives a 3600 mV full scale; a 4.2 V cell presents 2994 mV, so 83% of
range with no clipping.

**Lesson worth keeping:** a 43% error is never resistor tolerance. It was treated as
an unexplained scale factor and papered over with an empirical constant for longer
than it should have been — reading the schematic settled it in one look.

### Reading battery state without RTT

RTT on this board reproducibly dies at `[00:00:01.119` -- exactly when the first
battery sample runs (`BATTERY_FIRST_SAMPLE_DELAY_MS = 1100`), also unexplained.
The values can be read straight out of RAM over SWD instead, which is reliable:

```bash
E=build/orangelink-ncs/zephyr/zephyr.elf
MV=$(nm $E | awk '/ last_mv$/{print $1}')
PC=$(nm $E | awk '/ last_percent$/{print $1}')
pyocd commander -t nrf52832 -c "read16 0x$MV" -c "read8 0x$PC"
```

### SWD needs USB power

SWD fails completely on this board when it runs on battery alone -- `Unexpected
ACK '0'` at every clock rate and every connect mode -- and works on the first
attempt with USB connected. Same lesson as the XIAO, where battery-only power broke
long SWD transfers. **Plug in USB before debugging.**
