# Orangelink NCS

A fork of [birdfly/Orangelink-Firmware](https://github.com/birdfly/Orangelink-Firmware)
being migrated from the legacy nRF5 SDK to the nRF Connect SDK (Zephyr).

A sub-GHz ↔ BLE bridge that speaks the RileyLink-compatible `subg_rfspy` protocol
to a **Medtronic Minimed pump at 916 MHz** over an external RFM69 radio.

Scope is 916 MHz Minimed only. The original firmware also drove a second radio at
433 MHz and an 868 MHz band; neither is fitted or supported here, and the
corresponding code paths were deliberately not ported.

---

> ## ⚠️ Safety
>
> This firmware sits in the communication path of **insulin pump hardware**.
>
> - **Bench and test hardware only. No clinical or real-patient use.**
> - All changes require independent review and hardware-in-the-loop validation
>   against real pump hardware before any field use.
> - The encoding paths (`4b6b`, `manchester`) and the sub-GHz state machine are
>   medical device wire formats. A single flipped bit changes what a pump receives.

---

## Status: working — Adafruit Feather nRF52832 target

Target: **Adafruit Feather nRF52832** (product 3406) with an **RFM69HCW Radio
FeatherWing**, at 916 MHz.

| | |
|---|---|
| **RF self-test** | Passes: `VERSION = 0x24`, FIFO and 4b6b datapath, DIO1 interrupt on P0.07, TX completion. |
| **Bootloader** | Stock Adafruit bootloader, 0.9.1 + S132 6.1.1. The application links at `0x26000`. |
| **Updates** | USB serial DFU and BLE OTA, both verified on hardware. No debug probe required. |
| **Footprint** | FLASH 52.7%, RAM 57.2% of an nRF52832. |

Wiring: RFM69 **CS to P0.11**, **DIO1 to P0.07**; SCK/MOSI/MISO come through the
Feather header. Full board notes:
[`docs/BOARD-feather-nrf52832.md`](docs/BOARD-feather-nrf52832.md).

Two known quirks, both documented: the board has only two LEDs (no green, so the
battery indicator's "green" is blue), and the battery divider needed an empirical
single-point calibration whose 43% nominal error is **unexplained**.

## Documentation

| Document | Contents |
|---|---|
| [`docs/FEASIBILITY_REPORT.md`](docs/FEASIBILITY_REPORT.md) | **Start here.** Verdict, flash/RAM analysis, chip recommendation, blockers |
| [`docs/legacy-flash-budget.md`](docs/legacy-flash-budget.md) | Measured flash/RAM budget from the Keil build artifacts |
| [`docs/gatt-service-spec.md`](docs/gatt-service-spec.md) | Complete BLE GATT spec — UUIDs, properties, the response handshake |
| [`docs/aps-protocol-spec.md`](docs/aps-protocol-spec.md) | RileyLink APS wire format, frame structures, concurrency hazards |
| [`docs/config-storage-spec.md`](docs/config-storage-spec.md) | Persisted config structures and the NUS config protocol |
| [`MIGRATION_NOTES.md`](MIGRATION_NOTES.md) | Every intended deviation from original behaviour |

## Repository layout

```
├── west.yml                 NCS v3.4.0 manifest
├── CMakeLists.txt           application build
├── prj.conf                 Kconfig
├── boards/                  devicetree overlays per board
├── dts/                     partition layouts and driver bindings
├── src/                     the ported application
│   ├── ble/                 GATT service (IPS)
│   ├── aps/                 RileyLink subg_rfspy command layer
│   ├── subg/                sub-GHz packet path
│   ├── encoding/            4b6b and Manchester line coding
│   ├── drivers/rf69/        RFM69 / SX1231 driver
│   ├── battery/             ADC battery monitor
│   └── indication/          status LED
├── docs/                    analysis and board notes
└── legacy/                  read-only reference copy of the original firmware
```

### What was removed from the fork

- **`nrfSDK/`** — the vendored 140k-line nRF5 SDK, replaced by NCS dependencies
  declared in `west.yml`
- **All four DFU private keys** (`private_xh601.pem`, `private_xh5102.pem`, and
  their duplicates in `update/`). They were committed in plaintext upstream and
  must be treated as compromised. See [`keys/README.md`](keys/README.md).
- **154 MB of Keil object files.** The `.map`, `.hex` and `.uvprojx` files were
  kept — they are the evidence behind the flash budget analysis.

175 MB → 4.9 MB.

## Building

Requires the nRF Connect SDK v3.4.0 toolchain (`west`, Zephyr SDK). Nordic's
`nrfutil toolchain-manager` is the supported way to install it.

> **Build paths must not contain spaces.** Zephyr's Kconfig fails on them with a
> bare "no such file or directory".

```bash
west init -l orangelink-ncs && west update && west zephyr-export
```

```bash
west build -b nrf52_adafruit_feather -d build orangelink-ncs -- -DEXTRA_DTC_OVERLAY_FILE="$PWD/orangelink-ncs/dts/feather-stock-bootloader.dtsi"
```

No second bootloader is built and no signing key is needed: the board keeps its
stock bootloader, the application links at `0x26000` where that bootloader expects
it, and the build produces a plain `zephyr.hex`.

Measured: FLASH 164064 B of 304 KB (52.7%), RAM 37512 B of 64 KB (57.2%).

### Run the unit tests

```bash
west twister -T orangelink-ncs/tests -p native_sim
```

## Flashing

### Over USB — the normal path

No probe needed. Package the hex and send it to the bootloader:

```bash
pip install adafruit-nrfutil
```

```bash
adafruit-nrfutil dfu genpkg --dev-type 0x0052 --application build/orangelink-ncs/zephyr/zephyr.hex fw.zip
```

```bash
adafruit-nrfutil dfu serial --package fw.zip -p /dev/ttyUSB0 -b 115200 --singlebank
```

The port is a **CP2104 bridge**, so it appears as `/dev/ttyUSB0`, not `ttyACM0` —
the nRF52832 has no USB peripheral of its own. That also means **this board is not
UF2**: Adafruit's UF2 bootloaders are nRF52840-only.

If it reports *"No data received on serial port"* while the red LED blinks about
twice a second, the bootloader is in serial DFU mode but too old for current
tooling. That is fixable — see below.

### Over BLE

The bootloader and the application both expose the Nordic legacy DFU service
(`00001530-1212-efde-1523-785feabcd123`), so the same `fw.zip` can be uploaded from
nRF Connect or Bluefruit LE Connect on a phone.

### Updating the bootloader

Needed once if serial DFU fails at the init packet. The BSP ships the image:

```bash
pyocd flash -t nrf52832 ~/.arduino15/packages/adafruit/hardware/nrf52/1.7.0/bootloader/feather_nrf52832/feather_nrf52832_bootloader-0.9.1_s132_6.1.1.hex
```

It writes the MBR, SoftDevice, bootloader and the UICR bootloader-address
registers, and leaves the application at `0x26000` untouched.

## Debugging

### SWD needs USB power

SWD fails completely on this board when it runs on battery alone — `Unexpected ACK
'0'` at every clock rate and every connect mode — and works first time with USB
connected. **Plug in USB before debugging.**

SWDIO and SWCLK are pads on the underside of the PCB; share GND, and do **not**
connect the probe's 3V3 while the board is on USB.

```bash
pyocd flash -t nrf52832 build/orangelink-ncs/zephyr/zephyr.hex
```

This writes only `0x026000`–`0x04E0E0`, so the MBR, SoftDevice and bootloader all
survive and the DFU paths keep working.

### Reading state without RTT

RTT on this board reproducibly dies at `[00:00:01.119` — exactly when the first
battery sample runs — which is unexplained. Battery state can be read straight out
of RAM instead, which is reliable:

```bash
pyocd commander -t nrf52832 -c "read16 0x$(nm build/orangelink-ncs/zephyr/zephyr.elf | awk '/ last_mv$/{print $1}')"
```

## License

Upstream application code is **GPL v2 only** (44 files, per their headers), and the
ported sources here carry `SPDX-License-Identifier: GPL-2.0-only` to match. NCS and
Zephyr are Apache 2.0.

Whether GPLv2 application code and an Apache-2.0 RTOS may be combined *and
redistributed* is a genuine question, discussed in
[`docs/FEASIBILITY_REPORT.md`](docs/FEASIBILITY_REPORT.md) §B2; there is no
top-level `LICENSE` file upstream. This is published as a personal project on that
basis rather than as settled advice — if you intend to distribute builds, form your
own view.

Original copyright: Fractal Auto Technology Co., Ltd. / Ribin Huang.
