# Orangelink NCS

A fork of [birdfly/Orangelink-Firmware](https://github.com/birdfly/Orangelink-Firmware)
being migrated from the legacy nRF5 SDK to the nRF Connect SDK (Zephyr).

Orangelink is a sub-GHz ↔ BLE bridge that speaks the RileyLink-compatible
`subg_rfspy` protocol to insulin pumps (Medtronic Minimed 722, Omnipod) over an
external RFM69 radio.

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

## Status: Phase 0 complete. No code has been ported. Nothing builds yet.

Phase 0 was a feasibility gate, and **it did not pass as specified**. Read
[`docs/FEASIBILITY_REPORT.md`](docs/FEASIBILITY_REPORT.md) before doing anything
else.

Headline findings:

| | |
|---|---|
| **nRF52810 cannot host this port** | The NCS platform baseline (~133 KB) exceeds the *entire* current shipping image (131.4 KB). Adding MCUboot overflows 192 KB. |
| **The scoping document's flash map was wrong by 72 KB** | S112 is **96 KB**, not ~25 KB. The application region is **52 KB**, not ~124 KB. |
| **The bare-metal option does not exist for nRF52** | NCS Bare Metal is a separate SDK supporting **nRF54L Series only**. |
| **Three remotely reachable buffer overflows in the shipping firmware** | Unauthenticated, no pairing required. Affects deployed devices today. |
| **Recommended chip** | nRF52832 — but compare against nRF54L first, since hardware is changing anyway. |

Three blockers gate Phase 1: a **hardware decision**, a **legal review** of the
GPL v2 / Apache 2.0 conflict, and **security disclosure** to the upstream
maintainer.

## Documentation

| Document | Contents |
|---|---|
| [`docs/FEASIBILITY_REPORT.md`](docs/FEASIBILITY_REPORT.md) | **Start here.** Verdict, flash/RAM analysis, chip recommendation, blockers |
| [`docs/legacy-flash-budget.md`](docs/legacy-flash-budget.md) | Measured flash/RAM budget from the Keil build artifacts |
| [`docs/gatt-service-spec.md`](docs/gatt-service-spec.md) | Complete BLE GATT spec — UUIDs, properties, the response handshake |
| [`docs/aps-protocol-spec.md`](docs/aps-protocol-spec.md) | RileyLink APS wire format, frame structures, concurrency hazards |
| [`docs/config-storage-spec.md`](docs/config-storage-spec.md) | Persisted config structures and the NUS config protocol |
| [`MIGRATION_NOTES.md`](MIGRATION_NOTES.md) | Every intended deviation from original behaviour |
| [`keys/README.md`](keys/README.md) | MCUboot signing keys and why the legacy ones are unusable |

## Repository layout

```
├── west.yml                 NCS v3.4.0 manifest          (untested)
├── CMakeLists.txt           application scaffolding      (src/ is empty)
├── prj.conf                 Kconfig starting point       (unverified)
├── boards/                  devicetree overlays          (Phase 1)
├── src/                     ported application           (Phase 3-4, empty)
├── keys/                    MCUboot signing keys         (public keys only in git)
├── docs/                    Phase 0 deliverables
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

**Nothing builds yet.** `src/` is empty by design; Phase 0 produced analysis, not
code.

Once the chip decision (blocker B1) is made, the first Phase 1 task is to stand up
the workspace and produce a real proof-of-fit build:

```bash
west init -l /path/to/orangelink-ncs
```

```bash
west update && west zephyr-export
```

```bash
west build -b <board> --sysbuild
```

Requires the nRF Connect SDK toolchain (`west`, Zephyr SDK / ARM GCC). Nordic's
`nrfutil toolchain-manager` is the supported way to install it. Note that the
toolchain was **not** installed during Phase 0, so `west.yml` and `prj.conf` have
never been exercised — expect to fix them.

## Flashing

Legacy tooling (`nrfjprog` + `mergehex` + `nrfutil pkg`, driven by the `.bat`
scripts in `legacy/update/`) does **not** apply to an NCS build. MCUboot images are
produced and signed by `imgtool` via the build system, and flashed with
`west flash`. Do not mix the two toolchains.

The legacy scripts also set `UICR APPROTECT` (`nrfjprog --memwr 0x10001208 --val
0xFFFFFF00`). Readback protection needs an equivalent decision in the NCS build
before any production flashing.

## License

Upstream application code is **GPL v2 only** (44 files, per their headers).
NCS and Zephyr are Apache 2.0. Whether these may be combined and distributed is
**an open legal question and a blocker on publishing this fork** —
[`docs/FEASIBILITY_REPORT.md`](docs/FEASIBILITY_REPORT.md) §B2. There is no
top-level `LICENSE` file upstream.

Original copyright: Fractal Auto Technology Co., Ltd. / Ribin Huang.

---

## Toolchain setup (verified working)

Workspace lives **outside** this repo at `/home/user/ai/orangelink-ncs-ws`.
Zephyr's build breaks on paths containing spaces, which is why it is not under
the original `OL SDK Update/` directory.

| Component | Version |
|---|---|
| nRF Connect SDK (`sdk-nrf`) | v3.4.0 |
| Zephyr (`sdk-zephyr`) | ncs-v3.4.0 / 4.4.0 |
| Zephyr SDK | 1.0.1 (`arm-zephyr-eabi-gcc` 14.3.0) |
| Board | `xiao_ble` (upstream Zephyr, `seeed/xiao_ble`) |

```bash
. /home/user/ai/orangelink-ncs-ws/env.sh
```

### Build: plain application (USB-flashable, no MCUboot)

Keeps the board's UF2 partition layout, so it can be flashed by drag-and-drop
over USB. This is the bring-up path.

```bash
west build -b xiao_ble -d build orangelink-ncs
```

### Build: with MCUboot dual-slot, signed

Replaces the UF2 bootloader, so **this variant requires SWD to flash**.

```bash
west build -b xiao_ble -d build-mcuboot --sysbuild orangelink-ncs -- -DSB_CONFIG_BOOTLOADER_MCUBOOT=y -DSB_CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y -DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$PWD/orangelink-ncs/keys/mcuboot-xiao-priv.pem\" -DEXTRA_DTC_OVERLAY_FILE="$PWD/orangelink-ncs/dts/orangelink-partitions.dtsi;$PWD/orangelink-ncs/dts/orangelink-app-slot0.overlay" -Dmcuboot_EXTRA_DTC_OVERLAY_FILE=$PWD/orangelink-ncs/dts/orangelink-partitions.dtsi
```

`orangelink-partitions.dtsi` goes to **both** images; `orangelink-app-slot0.overlay`
to the **application only**. Sending the latter to MCUboot gives the bootloader
`FLASH_LOAD_OFFSET=0xc000` and produces a clean build that does not boot.
Always check link addresses after changing partitions.

### Run the unit tests

```bash
west build -b native_sim -d build-tests orangelink-ncs/tests/encoding && ./build-tests/encoding/zephyr/zephyr.exe
```

## Debug probe: Raspberry Pi Pico as CMSIS-DAP

The XIAO has no onboard debug probe. A Pico 1 (RP2040) flashed with Raspberry Pi
`debugprobe` v2.3.1 (`debugprobe_on_pico.uf2`) works as a CMSIS-DAP probe and is
driven by pyOCD, which has a builtin `nrf52840` target.

Flash the Pico by holding BOOTSEL while plugging in, then copying the UF2 to the
`RPI-RP2` volume. It returns as USB `2e8a:000c`.

### One-time udev rule

Without this, pyOCD reports "No available debug probes are connected" because
`/dev/bus/usb/*` is root-only:

```bash
sudo install -m 644 /home/user/ai/orangelink-ncs-ws/orangelink-ncs/tools-udev-60-cmsis-dap.rules /etc/udev/rules.d/60-cmsis-dap.rules && sudo udevadm control --reload-rules && sudo udevadm trigger
```

### Wiring, Pico -> XIAO nRF52840

Pico pins are fixed by the stock firmware (`board_pico_config.h`): SWCLK and
SWDIO must be consecutive, `SWCLK = PROBE_PIN_OFFSET + 0`, `SWDIO = +1`.
Changing them requires rebuilding debugprobe from source against the Pico SDK.

| Signal | Pico GPIO | Pico physical pin |
|---|---|---|
| SWCLK | GP2 | 4 |
| SWDIO | GP3 | 5 |
| GND | — | 3 (or any GND) |
| target RESET (optional) | GP1 | 2 |
| UART TX -> target RX | GP4 | 6 |
| UART RX <- target TX | GP5 | 7 |

On the XIAO, SWDIO/SWCLK are **small test pads on the underside**. Published test
point numbering is inconsistent between sources, so do not trust a TP map --
including any in this file. Practical approach:

- Take **GND from the castellated header pin**, which is clearly labelled. No need
  to find a GND test pad.
- Only **two** pads actually need soldering: SWDIO and SWCLK.
- Swapping SWDIO and SWCLK cannot damage anything -- pyOCD simply fails to
  connect. Try one orientation, swap if it fails.
- **Do not connect the probe's 3V3** while the XIAO is powered over USB-C.
  Dual-powering it has been reported to corrupt bootloaders. Power the XIAO from
  USB-C (this works even with a charge-only cable) and connect only
  SWDIO / SWCLK / GND.

### Reading the log over RTT

```bash
pyocd reset -t nrf52840                       # probe must be free for this
pyocd rtt   -t nrf52840 -M attach             # -M attach is load-bearing
```

Two traps, both of which look like "RTT is broken" when they are not:

* **`-M attach` matters.** pyOCD's default connect mode *halts the core*, so the
  board stops running and emits nothing. The session attaches cleanly, reports
  "3 up channels ... Reading from up channel 0", and then sits silent forever.
* **Only one pyOCD can hold the probe.** Running `pyocd reset` while `pyocd rtt`
  is attached silently does nothing, so there is no boot output to see. Reset
  first, then attach -- `CONFIG_LOG_PROCESS_THREAD_STARTUP_DELAY_MS=4000` holds
  the boot burst long enough to catch it.

Attaching to an already-running, healthy board correctly shows *nothing*: at idle
the only periodic message is the battery sample every 3 minutes, and that is
`LOG_DBG` unless the charge colour changes. Silence is not a fault.

### Flash over SWD

```bash
pyocd flash --target nrf52840 build/orangelink-ncs/zephyr/zephyr.hex
```

```bash
pyocd gdbserver --target nrf52840
```
