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

## Battery calibration

Calibrated against a cell measured at **3.86 V**:

```
CONFIG_ORANGELINK_BATTERY_OUTPUT_OHMS=100000
CONFIG_ORANGELINK_BATTERY_FULL_OHMS=139300     # nominal would be 200000
```

Result: `last_mv = 3857 mV`, `last_percent = 56%` — within 0.1% of the meter.

**The 43% error this corrects is unexplained**, and that matters more than the fix.
The nominal ratio of 2.0 is right per the board spec, and the configuration was
verified in the generated devicetree:

```
reg = 0x7, zephyr,input-positive = 0x7   -> AIN7 / P0.31
zephyr,gain = ADC_GAIN_1_6, ref INTERNAL -> 3600 mV full scale
zephyr,resolution = 0xc                  -> 12-bit
```

A 3.86 V cell should give 1930 mV at the pin and a raw count near 2195 of 4095.
Instead the firmware derived 5542 mV, implying ~3152 raw — high, but **not
clipping**, so saturation is ruled out. Raising the acquisition time to 40 us for
the ~50 kOhm source impedance (Nordic's guidance for that range) changed nothing:
5526 mV before, 5542 mV after.

43% is far too large for resistor tolerance, so a systematic factor is at work that
has not been identified. Consequences to keep in mind:

* This is a **single-point** calibration. Linearity has not been checked, so the
  reading may drift at other voltages. Verify against a meter at a second point --
  ideally near 3.5 V and near 4.1 V -- before trusting the percentage.
* If the cause is later found (a different reference, an unexpected gain, or a
  divider that is not what the spec says), `FULL_OHMS` should go back to nominal
  and the real fault be corrected instead.

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
