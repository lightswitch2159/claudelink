# Migration Notes

Every intended or discovered deviation from the original firmware's behaviour,
and the reasoning behind each one.

**Status: complete and verified on hardware.** This branch targets the Adafruit
Feather nRF52832 (product 3406) with an RFM69HCW Radio FeatherWing, alongside the
XIAO nRF52840 build on `xiao-nrf52840-sense`.

Differences from the XIAO branch, all deliberate and documented in
`docs/BOARD-feather-nrf52832.md`:

* Keeps the **stock Adafruit bootloader** instead of installing MCUboot, so updates
  go over USB serial DFU or BLE OTA -- both verified working -- and a bad image
  never costs you the recovery path. No MCUboot means no SMP/mcumgr DFU.
* Only two LEDs (red, blue -- no green), so the battery indicator's "green" is blue
  and "yellow" is red+blue.
* Battery divider calibrated empirically at 3.86 V. The 43% error this corrects is
  **unexplained** and the calibration is single-point, so linearity is unverified.

The RF self-test passes (`VERSION = 0x24`, FIFO and 4b6b datapath, DIO1 interrupt
on P0.07, TX completion) and the radio sleeps between operations.

Sections 1-2 record what was found *before* any code was written. Everything from
section 3 onward is implemented and tested, and several sections exist specifically
to record where an earlier conclusion in this document turned out to be wrong.


> **A note on `tools/` and the pump serial.** This repository is published without
> the bench test scripts (`tools/*.py`) that the notes below refer to — they drove a
> real insulin pump over BLE and are not useful without that hardware. The pump's
> serial number has also been redacted to `REDACTED` throughout. References to
> `tools/…` are kept because the findings they record — protocol bugs, encoding
> mistakes, and what each test did or did not prove — are the point of these notes,
> and several of those lessons were about the tooling being wrong rather than the
> firmware.


---

## 1. Discrepancies between the brief and the source

Found during Phase 0 analysis. Each one would have produced a wrong port if
followed literally.

### 1.1 Flash map — off by 72 KB

The hardware specification states SoftDevice S112 ≈ 25 KB and an application
region of ≈ 124 KB. **Measured: S112 is 96 KB (`0x1000`–`0x19000`) and the
application region is 52 KB (`0x19000`–`0x26000`).**

Corroborated three ways: the SoftDevice hex spans `0x000000`–`0x018C10`; the
application's Keil IROM1 base is `0x19000`; the DFU scripts declare
`--sd-req 0xB8` (S112 6.1.1). Detail in
[`docs/legacy-flash-budget.md`](docs/legacy-flash-budget.md).

**Consequence:** this single error is why nRF52810 looked merely tight rather than
impossible. Any plan derived from the original map is invalid.

### 1.2 2M PHY — the brief asks for a behaviour change, not a port

Brief task 13 says "Request 2M PHY on connection." The legacy firmware **never
initiates a PHY update.** It only responds to peer-initiated requests, with
`BLE_GAP_PHY_AUTO` for both directions.

**Decision: do not request 2M.** Enable support (`CONFIG_BT_CTLR_PHY_2M=y`) and
let the central drive it, matching current behaviour. If 2M is genuinely wanted,
raise it as a separate change with its own interoperability testing.

### 1.3 The custom heap allocator is dead code

Brief task 23 says to replace `userKit/heap/`. It declares `static uint8_t
heap[10240]` plus a 640-entry table, but `grep -c kit_heap app.map` returns **0** —
it is not linked. The C library heap is discarded too (`Removing
arm_startup_nrf52810.o(HEAP), (2048 bytes)`), and total ZI is 4,432 B, far below
what the array alone would need.

**Decision: delete, do not port.** The application performs no dynamic allocation;
`CONFIG_HEAP_MEM_POOL_SIZE` stays `0`. Do not budget 10 KB of RAM for it — the
`HEAP_SIZE` figure in the hardware spec is misleading.

### 1.4 NUS is load-bearing and the service list omits it

The brief's Phase 2 tasks cover IPS and the Battery Service. They do not mention
that **Nordic UART Service carries the entire configuration protocol and the
factory-test protocol** (`Ble_NusSendData` in `app_config.c` and
`app_factory.c`), including the buzzer trigger. Without NUS, all of it stops
working. See [`docs/config-storage-spec.md`](docs/config-storage-spec.md) §3.

### 1.5 Advertising interval — the source comment is wrong

`BLE_ADV_INTERVAL 480` with a comment claiming "187.5 ms". `480 × 0.625 ms =
300 ms`. The brief's 300 ms is correct; the code comment is stale. Trust the
constant.

### 1.6 Bare-metal option does not exist for nRF52

The scoping document treats `CONFIG_KERNEL=n` as an nRF Connect SDK (NCS) Kconfig that might reduce
footprint. NCS Bare Metal is a **separate SDK** (`ncs-bm`, v2.0.99) supporting
**nRF54L Series only**. Remove this branch from the plan; Zephyr is the only NCS
architecture available for nRF52.

### 1.7 FDS → Settings needs no migration tooling

The scoping risk matrix lists "FDS → Settings migration loses user config" with a
mitigation of "write a migration tool." Since the flash layout change forces
erase-and-reflash (and a chip change forces new hardware), **there is no scenario
where NCS firmware must read a legacy FDS record.**

**Decision: accept the config reset.** The user-visible loss is a custom device
name and two indication toggles. Document it in release notes; skip the tool.

---

## 2. Deliberate deviations from original behaviour

Decided in advance, before porting began. All of these are now implemented; where
one later proved wrong or needed revisiting, a later section says so.

### 2.1 Bounds checking added to all three command parsers — REQUIRED

Three unchecked `memcpy` calls in the shipping firmware are remotely reachable
stack buffer overflows over an unauthenticated BLE link:

| Function | Destination | Max write | Overflow |
|---|---|---|---|
| `Aps_PutCmd()` | 123 B | 148 B | ~25 B |
| `Cfg_PutReq()` | 2 B | 242 B | ~234 B |
| `Fct_PutReq()` | 20 B | 242 B | ~222 B |

The port will bound-check every deserialisation boundary, including the `offset`
parameter that Zephyr GATT write callbacks expose. Malformed frames will be
rejected rather than silently dropped where the protocol has a parameter-error
code available.

**This is a behaviour change** — frames that previously corrupted the stack will
now be rejected. That is the point. Details:
[`docs/aps-protocol-spec.md`](docs/aps-protocol-spec.md) §7,
[`docs/config-storage-spec.md`](docs/config-storage-spec.md) §4.

### 2.2 Buttonless DFU service removed — COMPANION APP AFFECTED

`ble_dfu_buttonless` disappears; SMP-over-BLE replaces it. **Any companion-app
code that triggers DFU by writing to the Buttonless DFU service will break.** This
must be raised with whoever owns the mobile app before Phase 5.

### 2.3 Sub-GHz path restructured into a dedicated thread

`Subg_GetPkt()` currently blocks for the entire client-supplied `listenTimeout`
(× `retryCnt + 1`) using unbounded FIFO polling and `Kit_DelayMs`. Ported as-is
into a Zephyr `k_timer` handler or the system workqueue, it would **starve the
Bluetooth host thread** — the link stays up while the device stops answering ATT.

Planned: a dedicated cooperative thread below the BT RX thread; RFM69 DIO lines as
GPIO interrupts with `k_sem` handoff instead of FIFO polling; `k_sleep` vs
`k_busy_wait` chosen per call site.

**Observable timing will differ.** Requires logic-analyser comparison against the
legacy build and validation against real pump hardware.

### 2.4 RFM69 access serialised behind a mutex

`CMD_UPDATE_REG` currently executes synchronously in the BLE callback and can call
`Subg_SetMode()` / `Subg_CfgRf()` while the APS loop is mid-SPI-transaction on the
same device. No lock exists; today only the legacy interrupt-priority arrangement
hides the race. Under Zephyr these are concurrent threads.

Planned: one `k_mutex` owning all RFM69 access, with `CMD_UPDATE_REG` routed
through the same queue as every other command. **This changes `CMD_UPDATE_REG`
from immediate to queued execution** — latency increases by up to one 10 ms loop
tick. Verify no client depends on the synchronous behaviour.

### 2.5 Packed-struct pointer casts replaced

Six sites take the address of an unaligned packed-struct member and cast it, e.g.
`Kit_ReverseFourBytes((uint32_t *)&p->listenTimeout)`. This is undefined
behaviour; GCC warns (`-Waddress-of-packed-member`) and may emit an aligned load
where ARMCC5 did not.

Planned: explicit byte extraction via `sys_get_be32()` / `sys_get_be16()` from
`<zephyr/sys/byteorder.h>`. Wire format is unchanged. **All six sites must be
converted** or the port will misparse commands at runtime only.

### 2.6 Config write debounce timing normalised

`Cfg_SetAdvName()` passes a raw `20` to `app_timer_start()` where
`motion_data_set()` correctly uses `APP_TIMER_TICKS(20)`, so the name path fires
after 20 ticks (<1 ms) rather than 20 ms. Both become
`k_work_schedule(&w, K_MSEC(20))`; the inconsistency disappears. No functional
impact expected.

`Cfg_SetAdvName()` also has `advNameInitFlg = CFG_INIT_FLG` commented out, working
only because the flag was already set at load. The port will set it explicitly.

### 2.7 Flash partitions become a single source of truth

The legacy scatter file gives the application `0x19000`–`0x28000` while FDS claims
`0x26000`–`0x28000` — **8 KB is double-booked.** Latent today (the image ends
~21 KB short) but an application that grew past `0x26000` would link over its own
configuration storage.

Zephyr's partition manager makes overlaps a build error. No action needed beyond
not reproducing the layout.

---

## 3. Behaviour that must be preserved exactly

Non-obvious things that look like bugs or accidents but are load-bearing for
companion-app compatibility. Do not "clean these up."

| Behaviour | Why it matters |
|---|---|
| Data value set **before** the Response Count notification | The app reads Data on notification. Reordering breaks every response. |
| Response Count wraps `0x00`–`0xFE`, never `0xFF` | `if (count >= 0xFF) count = 0` |
| Timer Tick uses the same wrap, 60 s interval, notify only when subscribed | |
| Custom Name write → persist, then **disconnect** with HCI reason `0x3B` | The app almost certainly special-cases this. |
| Device name **not** exposed as a readable GATT attribute | `SEC_MODE_SET_NO_ACCESS`. Zephyr exposes it by default. |
| `CMD_RESET` (`0x07`) has no handler — **no response at all** | Client times out. Changing this is a protocol change. |
| `CMD_LED` (`0x08`), `CMD_SET_MODE_REG` (`0x0A`), LED Mode writes | Accepted, ignored, return success. |
| `CMD_READ_REG` returns constant `0x5A` for all addresses except `0x09`–`0x0B` | A stub, not a real register read. |
| Trailing single zero byte stripped for Minimed | Get it wrong and packets corrupt silently. |
| RSSI reported as `(dBm + 73) × 2`, `uint8_t` truncation included | CC111x emulation for RileyLink clients. |
| Out-of-band frequency logged and **discarded**, radio keeps previous setting | |
| Response Count incremented **only** when the value write succeeded | Zephyr's async `bt_gatt_notify()` needs `bt_gatt_notify_cb()` to match. |
| Sub-GHz RX aborts when BLE drops to advertising → `CMD_INTERRUPTED` (`0xBB`) | Cross-layer coupling; must be preserved. |
| Two characteristics share 16-bit alias `0x7910`, differ only in the 128-bit UUID | Keying off 16-bit aliases will break. |
| All six User Description strings, verbatim | `"Data"`, `"Response Count"`, `"Timer Tick"`, `"Custom Name"`, `"Version"`, `"LED Mode"` |
| `Version` reads `"ble_rfspy 2.0"`; `CMD_GET_VER` returns `"subg_rfspy 2.2"` | Two different strings. Not a typo. |
| No pairing, no bonding, no encryption on any characteristic | Compatibility requirement — hence §2.1. |
| `4b6b.c` / `manchester.c` ported **verbatim** | Medical device wire formats. Cover with round-trip and known-vector `ztest`. |

---

## 4. Deferred decisions

| Decision | Owner | Gate |
|---|---|---|
| Target chip: nRF52810 (infeasible) / nRF52832 / nRF54L | Hardware + product | **Blocks Phase 1** |
| GPL v2 vs Apache 2.0 combined distribution | Legal counsel | Blocks publication |
| Security disclosure timing to upstream maintainer | Product + security | Blocks publication |
| Whether to patch the three overflows on the existing nRF5 SDK build | Product | Independent of migration — available now |
| One MCUboot signing key or one per board | Product | Phase 5 |
| Single-slot direct-XIP vs dual-slot OTA | Follows chip decision | Phase 5 |
| Key rotation / revocation plan | Security | Before production |
| `UICR APPROTECT` readback protection in the NCS build | Security | Before production flashing |
| In-field OTA migration — recommend dropping if chip changes | Product | Phase 5 |

---

## 5. Phase 1/2 build results (XIAO nRF52840, NCS v3.4.0)

First real builds. Toolchain: Zephyr SDK 1.0.1, `arm-zephyr-eabi-gcc 14.3.0`,
Zephyr 4.4.0, sdk-nrf v3.4.0.

### Measured footprint -- the proof-of-fit Phase 0 could not produce

| Image | Load offset | Partition | Used | % |
|---|---|---|---|---|
| MCUboot | `0x00000` | 48 KB | 39,664 B | 80.7% |
| Application (signed) | `0x0C000` | 442,218 B usable | 178,348 B | 40.3% |
| Application RAM | -- | 256 KB | 39,468 B | 15.1% |

Contents: BLE peripheral + full IPS GATT service + Battery Service + SPI + ADC +
PWM + watchdog + Settings/NVS + UART logging + MCUboot dual-slot signed with
ECDSA P-256. Not yet included: APS handler, sub-GHz state machine, RFM69 driver,
NUS.

**This retroactively confirms the nRF52810 verdict with first-party numbers.**
178 KB of application would not fit its 52 KB slot, and 39.5 KB of RAM exceeds
the part's entire 24 KB.

**MCUboot is at 80.7% of its 48 KB partition** -- only 8.3 KB spare. 64 KB is
unallocated at the top of flash and can be moved into `boot_partition` if
MCUboot needs to grow (serial recovery, for instance, would not fit today).

### Verified encoding port

`4b6b.c`, `4b6b.h` and `manchester.c` were copied **byte-identical** from the
legacy tree; `manchester.h` gained only `#include <stdbool.h>` (it used `bool`
while relying on SDK headers). A `ztest` suite in `tests/encoding/` runs on
`native_sim`: **9/9 pass, zero warnings**, covering round-trip identity for all
256 byte values and lengths to 71, plus the documented output-length formulas.

That establishes the libraries behave identically under GCC 14.3 as under the
original ARMCC5. Known-answer vectors from validated pump captures still need
adding -- round-trip identity would not catch a codebook that is
self-consistently wrong.

### New deviations from original behaviour

#### 5.1 Device name moved to the scan response

Legacy asked for `BLE_ADVDATA_FULL_NAME` in **both** advdata and srdata. A
128-bit UUID (18 B) plus flags (3 B) leaves 10 B of the 31 B payload, i.e. 8
characters -- so `"Orange"` (6) fitted the advertising packet but `"OrangePro"`
(9) could not, and the two boards behaved differently. The companion app
therefore cannot depend on the name being in the advertising payload.

The port puts flags + IPS UUID in the advertising packet and the complete name in
the scan response. Always fits; standard central APIs merge the two. **Verify
against the app.**

#### 5.2 Chip select handed to Zephyr

`cs-gpios` on `&spi2` replaces the legacy manual NSS toggling and per-transfer
bus de-init. Better design, but a behavioural change -- validate RFM69 CS timing
with a logic analyser before trusting it, and fall back to manual GPIO CS if the
radios prove fussy.

#### 5.3 `i2c1` disabled

The board enables `i2c1` on P0.04/P0.05, which are D4/D5 -- the pins the motor
and buzzer PWM need. Disabled in the overlay. I2C is not otherwise used.

#### 5.4 Motor and buzzer moved to pwm1/pwm2

`&pwm0` is already bound by the board to `pwm_led0` on P0.17, which is not even a
header pin. Motor uses `&pwm1` (D4), buzzer `&pwm2` (D5).

#### 5.5 Battery ADC channel and scaling both change

Legacy read P0.04 / AIN2 through an external divider. The XIAO reads AIN7 /
P0.31 through its own onboard divider, gated by P0.14.

**P0.14 must be held LOW while sampling** -- Seeed documents that with it HIGH the
sense path is disabled and P0.31 may reach 3.6 V, risking damage to the pin. It is
declared `GPIO_ACTIVE_LOW` in the overlay so `gpio_pin_set(...,1)` enables sensing.

The mV conversion in `app_battery.c` must be **recalculated, not carried over**,
and the divider ratio bench-calibrated against a measured cell voltage.

#### 5.6 UF2 bootloader and its partition layout replaced

The board pulls in `nrf52840_partition_uf2_sdv7.dtsi`: a 156 KB SoftDevice
reservation we do not need (Zephyr links its own SoftDevice Controller into the
application) plus a 48 KB Adafruit UF2 bootloader that performs **no signature
verification**. Both are deleted and replaced by the MCUboot layout in
`dts/orangelink-partitions.dtsi`.

**Consequence: USB drag-and-drop flashing no longer works for MCUboot builds; an
SWD probe is required.** Builds without MCUboot keep the board's UF2 layout and
stay USB-flashable, which is the practical path for bring-up.

#### 5.7 MCUboot has no console

The board's chosen console is the USB CDC ACM device, and MCUboot does not enable
the USB stack -- `uart_console.c` then links against a device that was never
instantiated. `sysbuild/mcuboot.conf` disables console, serial and logging in the
bootloader, which is correct for a bootloader regardless and saves flash.

#### 5.8 `CONFIG_BT_USER_PHY_UPDATE` enabled

Only to expose the `le_phy_updated` callback for observability. Zephyr responds to
peer PHY requests with or without it, and the port still does **not** initiate a
PHY update -- matching legacy behaviour (see section 1.2).

### Two build-system traps worth recording

1. **Paths containing spaces break the Zephyr build.** The original working
   directory was `.../OL SDK Update/`, and Kconfig failed with a bare
   "no such file or directory" from `cmake -E env`. The workspace lives at
   `/home/user/ai/orangelink-ncs-ws` for this reason.

2. **A shared partition `.dtsi` must not set `zephyr,code-partition`.** Doing so
   leaked into the MCUboot image and gave the bootloader
   `CONFIG_FLASH_LOAD_OFFSET=0xc000`, splitting its image across `0x0` and slot0 --
   a build that passed cleanly and would not have booted. The chosen now lives in
   an app-image-only overlay. **Check link addresses, not just build success.**

---

## 6. Hardware validation on XIAO nRF52840

First run on real hardware. Probe: Raspberry Pi Pico (RP2040) with `debugprobe`
v2.3.1, driven by pyOCD 0.45.1.

Target identified over SWD as **nRF52840 QIAA D0**, `INFO.PART = 0x00052840`,
`DEVICEID[0] = 0x7ddb79df`.

### The board arrived APPROTECT-locked

`pyocd` reported `NRF52840 APPROTECT enabled: not automatically unlocking`. D0 is a
revision with **hardware APPROTECT, enabled from the factory**, so a CTRL-AP mass
erase is required before any debug access:

```bash
pyocd erase --mass -t nrf52840 -O auto_unlock=1
```

This very likely explains why the board never enumerated over USB: with a locked
and effectively empty part there was no application to bring USB up, and no
bootloader to answer a double-tap either.

The mass erase also removed the Adafruit UF2 bootloader, so **the plain
non-MCUboot build can no longer boot** -- it links at `0x27000` and depended on
that bootloader to jump there. MCUboot builds are self-contained from reset and
are now the only bootable variant unless the UF2 bootloader is restored over SWD.

### Verified working

| Step | Result |
|---|---|
| MCUboot + signed app flashed over SWD | 40,960 B + 180,224 B programmed |
| MCUboot verified the ECDSA P-256 signature and booted the app | yes |
| BLE advertising | `E6:B5:4D:8C:C1:B9` as `OrangePro` |
| IPS GATT service vs docs/gatt-service-spec.md | **41/41 checks pass** |
| Encoding round-trip tests on native_sim | 9/9 pass |

`tools/verify_gatt.py` performs the GATT check against live hardware: all six
128-bit UUIDs, properties, declaration order, User Description strings, CCCD
presence, `"ble_rfspy 2.0"`, and that an over-length Data write is **rejected**
(the guard closing the legacy overflow). Re-run it after any change to
`src/ble/ips.c`.

Footprint with RTT enabled: application **179,628 B of 442,218 B (40.6%)**,
RAM **41,772 B of 256 KB (15.9%)**; MCUboot **39,664 B of 48 KB (80.7%)**.

### 6.1 Bug found on hardware: advertising did not restart after disconnect

`bt_le_adv_start()` was being called directly from the `disconnected` connection
callback. The device advertised correctly at boot, accepted one connection, and
then went silent permanently -- the callback runs while the connection object is
still being torn down, so the restart fails.

Fixed by deferring to the system workqueue (`k_work_submit`), and `-EALREADY` is
now treated as success. Confirmed in the RTT log:

```
<inf> main: disconnected (reason 0x13)
<inf> main: advertising as "OrangePro" at 300 ms
```

This class of bug is invisible to a build and to a single-connection test. Any
future change to connection handling should be checked with a
connect / disconnect / rescan cycle, which `tools/verify_gatt.py` now exercises.

### 6.2 RTT logging over SWD

The XIAO has no debug header and the UART would need two more flying leads to
tiny pads, so RTT is the practical console: it rides the SWD link already in
place. `CONFIG_USE_SEGGER_RTT` + `CONFIG_LOG_BACKEND_RTT`, ~1.3 KB flash and
~2.3 KB RAM.

pyOCD's `rtt` subcommand requires a TTY and dies with
`Inappropriate ioctl for device` when run without one, including with stdin
redirected. Run it under a pty:

```bash
script -qec "pyocd rtt -t nrf52840" /dev/null
```

### 6.3 Observation: the USB stack is being built in

The RTT log shows `udc_nrf: Preinit` / `Initialized` / `SUSPEND state detected`.
The board devicetree includes `cdc_acm_serial.dtsi`, so the USB device controller
is initialised even though nothing in the application uses it. `SUSPEND` means no
host is enumerating it, consistent with a charge-only USB-C cable.

Two follow-ups: this costs flash and RAM for nothing today and could be disabled;
or, if USB CDC is adopted as the console, it frees D6/D7 and gives a second DFU
path. Decide in Phase 5.

---

## 7. Scope narrowed to 916 MHz, single radio

Project scope is **916 MHz Minimed only**. The legacy design fitted **two** RFM69
modules -- one at 433 MHz and `RF69_DEV_FREQ916N868` (Minimed) --
each with its own chip select. Only one is fitted now.

Not ported, deliberately:

- the 433 MHz (`freq433CfgTbl`) and 868 MHz (`freq868CfgTbl`) register tables
- the second radio instance, its chip select and its DIO0 line
- the 433 MHz radio's receive path and 80-byte packet mode in `app_subg.c`
- the 433 MHz and 868 MHz sub-GHz modes

Pin usage drops to **7 of 11** header pins; D1 and D3 are now free.

`4b6b.c` and `manchester.c` are both **kept**. Encoding is selected at runtime by
the host via `CMD_SET_SW_ENCODING` and is independent of band, so dropping either
would break commands the host may legitimately send.

### 916 MHz configuration

Transcribed unchanged from `freq916CfgTbl`: OOK modulation, 16384 bps,
`FRF = 0xE52312`, fixed-length packets, CRC off, 4-byte sync word `FF 00 FF 00`,
RSSI threshold 228 (-114 dBm), DIO0 mapped to the packet interrupt.

`FSTEP` is computed with integer math -- `(frf * 32 MHz) >> 19` -- rather than the
legacy `float RF69_FSTEP = 61.03515625`. Exact, and no FPU dependency in what
becomes an interrupt-adjacent path. Note the RFM69's 32 MHz crystal is a different
thing from the 24 MHz `RILEY_LINK_FXOSC` the APS layer uses to decode CC111x-style
register values; conflating them would put the radio ~25% off frequency.

### Layered self-test

`rf69_selftest_run()` / `rf69_selftest_report()` verify the module in stages, so a
failure points at a specific fault rather than just "not working":

| Stage | Checks | A failure means |
|---|---|---|
| 1 | `REG_VERSION == 0x24` | `0x00` unpowered or MISO not connected; `0xFF` MISO floating or MOSI/SCLK/NSS not arriving; other value = wrong device or MOSI/MISO swapped |
| 2 | write/read-back of a sync-word byte | reads work, writes do not -- check MOSI |
| 3 | apply 916 table, read FRF back | multi-register writes not landing |
| 4 | `ModeReady` asserts | SPI fine but the 32 MHz crystal is not oscillating |
| 5 | RSSI reads | see the caveat below |
| 6 | DIO0 GPIO readable | line not wired |

Run at boot and re-run every 5 s **for as long as it fails**, so the module can be
connected with the board already powered and the result watched live in the log.
Retries stop on the first pass.

The legacy driver's unbounded `while ((REG_IRQFLAGS1 & MODEREADY) == 0);` spin is
now bounded -- an unpopulated or dead radio hung the original firmware.

> **Caveat on stage 5.** An idle band reads the noise floor (-127 dBm observed),
> which the plausibility check accepts. It proves the RSSI register responds; it
> does **not** prove the receiver has sensitivity. Real validation needs a known
> transmitter and belongs in Phase 6 against pump hardware.

### First hardware result

```
[PASS] present        REG_VERSION=0x24 (RFM69/SX1231)
[PASS] spi read/write  wrote 0xa5, read 0xa5
[PASS] 916 MHz config applied 24 registers
[PASS] frequency      916547973 Hz (expected 916548000)
       FRF read back 0xe52312
       FRF written   0xe52312
[PASS] radio alive    ModeReady after 0 us
[PASS] rssi           -127 dBm
[PASS] dio0           level=0
---- ALL PASSED ----
```

The 27 Hz difference is integer truncation in the FSTEP conversion.

### Trap: deferred logging silently drops oversized messages

An intermediate version printed the frequency and the raw FRF bytes in one
`LOG_INF` with nine arguments. Under `CONFIG_LOG_MODE_DEFERRED` that message was
**mangled rather than dropped** -- it reported a plausible-looking but wrong
`915000000 Hz`, which briefly looked like a real frequency bug. Splitting it into
three shorter lines showed the frequency had been correct all along.

Keep log calls to a handful of arguments, and treat a suspicious value in a
long-format log line as a logging problem before believing it.

---

## 8. APS command layer ported and verified over BLE

`src/aps/aps.c` replaces `legacy/project/app/src/app_aps.c`. Wire format is
unchanged; `tools/verify_aps.py` drives it over BLE and checks every response
against docs/aps-protocol-spec.md. **25/25 checks pass on hardware.**

### Verified end to end

```
CMD_GET_VER          -> b'\xddsubg_rfspy 2.2'
CMD_GET_STATE        -> b'\xddOK'
CMD_GET_STATISTICS   -> [0xDD] + 20 bytes, big-endian decode correct
CMD_READ_REG(0x09)   -> b'\xdd\x12'   (default frequency register)
CMD_READ_REG(0x42)   -> b'\xddZ'      (the 0x5A stub, unchanged)
CMD_SET_SW_ENCODING  -> 0xDD for NONE/MANCHESTER/4B6B, 0x11 for invalid
CMD_LED, CMD_SET_MODE_REG, CMD_RESET_RADIO_CFG -> 0xDD
Response Count       -> increments exactly one per response, notifies
```

Legacy quirks confirmed preserved, all three verified as *silent*:
`CMD_RESET` (0x07, no dispatch case), unknown opcodes, and `CMD_UPDATE_REG` with a
short frame. Each leaves the client to time out, exactly as before.

### The host -> firmware -> radio frequency path works

The host sets frequency through CC111x-style registers where
`freq = reg * 24 MHz / 2^16`. Writing `0x09/0x0A/0x0B = 26 30 00` produced:

```
<inf> aps: tuning to 916500000 Hz
```

and the RFM69 was retuned. Writing the legacy default `12 14 83` produced:

```
<wrn> aps: frequency 433922973 Hz outside the 916 MHz band, ignored
```

That 433,922,973 Hz is a useful cross-check: it matches the documented 433.92 MHz
legacy default, confirming the register-to-frequency arithmetic, and the 916-only
guard correctly refuses it rather than mistuning the fitted radio.

### Deviations

#### 8.1 Dedicated thread instead of a 10 ms polling timer

Legacy drained a 1-deep FIFO from an `app_timer` callback every 10 ms.
`CMD_GET_PKT` and `CMD_SEND_AND_LISTEN` block for a client-supplied timeout, so
this work cannot live on the system workqueue without starving everything else on
it. APS now has its own thread at priority 7 -- below the Bluetooth RX thread, so
the link keeps servicing ATT while the radio listens. Queue depth stays at 1, so
commands arriving while one is in flight are still dropped as before.

#### 8.2 `CMD_UPDATE_REG` is now queued, not immediate

Legacy executed it synchronously inside the BLE callback, where it could drive the
RFM69 over SPI while the command loop was mid-transaction on the same bus, with no
lock. It is queued like every other command now, so all radio access is serialised
onto the APS thread. Costs up to one queue hop of latency. Verify no client
depends on the old synchronous timing.

#### 8.3 Bounds check added (the overflow fix)

`aps_put_cmd()` rejects any frame whose parameter length exceeds
`APS_MAX_PARAM_LEN` (123) and answers `PARAM_ERROR`. The legacy
`memcpy(req.pkt, pBuf + 2, len - 2)` had no such check and the Data
characteristic accepts up to 150 bytes -- a 25-byte stack overflow reachable over
an unauthenticated link. The IPS write callback rejects over-length writes as a
second, independent layer; `verify_aps.py` confirms a 200-byte write is refused
at ATT.

#### 8.4 Radio commands answer RX_TIMEOUT while the packet path is unported

`CMD_GET_PKT`, `CMD_SEND_PKT` and `CMD_SEND_AND_LISTEN` return `0xAA` and log
plainly that the sub-GHz path is missing. The wire format is already correct so a
host sees a well-formed "nothing received" rather than hanging -- but this must not
be mistaken for working radio traffic. **These are the next thing to implement.**

#### 8.5 Band handling is narrowed

Legacy accepted three bands and switched radio mode accordingly. Only 914-918 MHz
is accepted now; anything else is logged and discarded, leaving the fitted radio
untouched. `CMD_UPDATE_REG` address `0x0C` value `0x59` reapplies the 916 config
instead of switching to the 868 MHz Minimed WWL table.

---

## 9. Sub-GHz packet path, DIO1, and a 10x TX airtime fix

`src/subg/subg.c` ports the Minimed TX/RX paths and wires the three radio APS
commands to them. Verified by an RF-free loopback that runs at boot once the
RFM69 self-test passes. **All stages pass on hardware.**

### 9.1 The interrupt line is DIO1, not DIO0 -- and that is better

The overlay originally specified DIO0. The board is wired to **DIO1**, and DIO1 is
the correct choice for this design:

| Line | Packet-mode mappings |
|---|---|
| DIO0 | RX: `CrcOk` / `PayloadReady` / `SyncAddress` / `Rssi`; TX: `PacketSent` |
| DIO1 | `FifoLevel` / `FifoFull` / **`FifoNotEmpty`** |

Minimed framing is variable length, terminated by `0x00`, under a fixed
`PayloadLength`. So DIO0's `PayloadReady` **never asserts for a normal short
packet** -- which is exactly why the first version of the RX loop had to poll
`FifoNotEmpty` over SPI every 250 us. `FifoNotEmpty` on DIO1 asserts the moment a
byte lands, whatever the eventual length, so receive is now genuinely
interrupt-driven. That is the mitigation for the top risk identified in Phase 0.

`RegDioMapping1` now writes `DIO0_00 | DIO1_10` -- a **deviation from the legacy
table**, which wrote `DIO0_00` only and left DIO1 at its `0b00` default of
`FifoLevel`.

TX completion goes back to polling `PacketSent`, since DIO1 cannot signal it.
DIO0 is still unconnected on the module and D1/D3 are free on the XIAO if TX
latency ever justifies its own line.

### 9.2 `FifoNotEmpty` is a level, not a pulse

The edge only arrives on the empty-to-non-empty transition. If bytes keep
arriving while the loop drains, the line stays high and no further edge is
generated -- so waiting on the semaphore alone would block on an edge that has
already passed. The loop therefore waits on the semaphore with a bounded 2 ms
fallback and re-checks the FIFO each pass: interrupt latency in the common case,
immune to the race. Either way the thread sleeps, so the Bluetooth threads keep
running.

### 9.3 A weak self-test check hid an unconnected pin

The first DIO check just read the GPIO level and reported
`[PASS] dio0 level=0`. **A floating input reads something**, so it passed on a pin
that was not connected to anything. What actually caught it was the loopback's
interrupt stage, and only because that was reported as a separate line rather than
folded into the overall pass.

Replaced with a deterministic test, made possible by DIO1 carrying a signal we
control completely: drain the FIFO and the line must read low; push one byte and it
must read high. Each step is cross-checked against `REG_IRQFLAGS2` over SPI, so
"the radio's flag never moved" (inconclusive) is distinguishable from "the flag
moved but the pin did not" (not wired), as are stuck-high and stuck-low. Result on
hardware: `[PASS] dio1 wiring follows FifoNotEmpty (low=0 high=1)`.

**Lesson: a self-test that samples a passive state proves almost nothing. Drive
the thing to a known state and check it followed.**

### 9.4 TX was sending 255-byte frames: 136 ms -> 13 ms

The loopback reported `PacketSent asserted after 136000 us` for a 4-byte payload.
At 16384 bps that should be about 6 ms. 136 ms is almost exactly 255 bytes of
airtime.

Cause: the 916 config uses `PACKET1_FORMAT_FIXED` with `PAYLOADLENGTH = 0xFF`, and
this port initially never set `PayloadLength` for TX. So every transmission sent a
full 255-byte frame, padding the tail with whatever the underrunning FIFO produced.

`minimed_tx()` now sets `PayloadLength` to the real frame size (data + terminator).
Measured **136 ms -> 13 ms**.

> **CORRECTION.** This section previously asserted that the legacy driver "never set
> `PayloadLength` for TX", and filed the fix as a DEVIATION to validate against the
> pump. Both claims were wrong. `Subg_SendPkt()` in
> `legacy/project/app/src/app_subg.c` does exactly this, once per burst in its
> per-mode setup:
>
> ```c
> case SUBG_MODE_MINIMED_NAS:
>         Rf69_SetMode(RF69_DEV_FREQ916N868, RF69_MODE_STANDBY);
>         Rf69_SetOokBw200khz(RF69_DEV_FREQ916N868);
>         Rf69_SetPayloadLen(RF69_DEV_FREQ916N868, len + 1);
> ```
>
> So this is not a deviation -- it restores legacy behaviour. The only real
> difference was *where* the call sits: per frame here, once per burst there. It is
> now hoisted into `subg_send_pkt()` to match. The original claim was written
> without checking the legacy source.

### 9.5 The loopback was testing a parallel reimplementation

The TX stage originally did its own FIFO write and mode change instead of calling
`subg_send_pkt()`. So it silently skipped the `PayloadLength` fix and kept
reporting `PASS` at 136 ms -- a green test that said nothing about the code that
actually runs. It now calls the production function.

**Lesson: a hardware test that reimplements the path it is testing will pass while
the real path is broken.**

### 9.6 Zero-length receive no longer reports success

Legacy `minimed_rx()` returned `SUBG_RX_OK` even when zero bytes were received,
leaving `*pRxLen` untouched -- so the caller transmitted whatever was on its stack.
Reported as `SUBG_RX_TIMEOUT` here instead.

### 9.7 Other preserved behaviour

- Zero-terminated framing: TX appends `0x00`, RX stops at the first `0x00`
- End-of-packet glitch trim: a trailing `0x80` or `0xC0` is an OOK demodulation
  artefact and is dropped
- Trailing zero stripped from the caller's payload before encoding, since the radio
  layer appends its own terminator
- `CMD_SEND_AND_LISTEN` retry loop resends with `repeatCnt` forced to 0
- Aborting an in-flight receive when BLE drops produces `CMD_INTERRUPTED` (0xBB)
- CC111x RSSI encoding `(dBm + 73) * 2`, truncation included

Radio command parameters are parsed with `sys_get_be16/32`, not by overlaying a
packed struct and byte-swapping in place -- see section 2.5.

### First hardware result

```
[PASS] fifo byte      wrote 0x5c read 0x5c
[PASS] fifo burst     16/16 bytes matched
[PASS] fifo flags     empty/not-empty track content
[PASS] datapath NONE  encode -> fifo -> decode
[PASS] datapath MANCH encode -> fifo -> decode
[PASS] datapath 4B6B  encode -> fifo -> decode
[PASS] dio1 interrupt fires on FifoNotEmpty edge
[PASS] tx complete    PacketSent asserted after 13000 us
---- ALL PASSED ----
```

The loopback proves the FIFO, the encode/decode chain and the DIO1 interrupt. It
does **not** prove receiver sensitivity or interoperability -- both need the
Minimed 722.

---

## 10. First Minimed 722 interop attempt: what is eliminated, what is not

Pump: 722, serial REDACTED, bench unit with no insulin, not worn. Read-only opcodes
only (wakeup 0x5D, model read 0x8D). No reply yet. Recording the eliminations so
the next session does not repeat them.

### One real bug found: RegPaLevel on a high-power module

The register dump showed `PALEVEL = 0x9f` -- PA0 on, PA1/PA2 off, power 31. That is
the RFM69 reset default, and the legacy 916 table **never writes PALEVEL**, so it
was simply inherited. It is correct only for a plain RFM69W, where PA0 is bonded to
the antenna.

The fitted module is an **RFM69HC**, where PA0 is *not* bonded -- PA1+PA2 are. So
the firmware was radiating nothing at all. `rf69_config_916()` now writes PALEVEL
explicitly from a Kconfig choice, because the variant is **not detectable in
software**: RFM69W and RFM69HW/HCW both report `REG_VERSION 0x24`.

Power is set to 27, giving +13 dBm on PA1+PA2 -- deliberately matching the +13 dBm
the legacy RFM69W produced at PA0 power=31, rather than the +17 dBm maximum. Same
level the pump has always seen, and it avoids desensing the pump's receiver at
bench range.

Fixing this did **not** produce a reply, so it was necessary but not sufficient.

### Eliminated by measurement

| Hypothesis | How it was ruled out |
|---|---|
| Frequency wrong | Swept 916.30-916.80 MHz in 50 kHz steps, silent at every point |
| Receive chain dead | RSSI survey in RX reads -90 to -87 dBm and **varies** -- a live front end on a connected antenna |
| SPI / FIFO / registers | Self-test and loopback all pass; register dump reads back correctly |
| DIO1 not wired | Deterministic FifoNotEmpty test passes; the interrupt fires |
| Payload-length change broke it | **Tested both ways.** `CONFIG_ORANGELINK_TX_LEGACY_TRUNCATE=y` reproduces the legacy fixed-255 truncated transmit exactly, and the pump is silent under both. This was the top predicted suspect and it is now cleared. |
| Host tooling wrong | CRC8 checked against decocare's table and test vector; 4b6b cross-checked byte-for-byte against the firmware's own C implementation; frequency register math round-trips exactly |

### Not yet eliminated, in order of probability

1. **The Medtronic protocol implementation in `tools/minimed.py` is wrong
   somewhere.** This is host-side guesswork, not firmware. Specifically unverified:
   whether the wakeup is really a bare `A7 <sn> 5D <crc>` frame (decocare's
   PowerControl 0x5D carries parameters, so a bare one may simply be rejected);
   whether 4b6b covers the CRC byte; and whether the pump needs a different
   preamble length or sync word than the legacy table configures.
2. **Nothing is actually leaving the antenna**, despite correct PA registers.
   Cannot be confirmed with a single transceiver -- needs an SDR or a second
   receiver.
3. **The pump is not in a receptive state.**

### The measurement that would cut through this

Everything above is inference. Two things would settle it directly:

- **Passive capture of pump RF** (`tools/sniff_722.py`, transmits nothing). If it
  captures even one frame, the receive chain is proven end to end against real RF
  and the problem is isolated to transmit. Needs the pump to use its radio.
- **AndroidAPS or Loop.** Our GATT service is byte-identical to the original
  (41/41 verified), so a real app should discover the device as an
  Orangelink/RileyLink and drive it with a correct, battle-tested Medtronic
  implementation -- removing the host-side guesswork entirely. This is the
  strongest available interop test.

**Honest status: the firmware's radio stack is verified as far as bench
instrumentation allows. Interoperability with the pump is unproven, and the
remaining uncertainty sits mostly in the host-side protocol implementation rather
than in the ported firmware.**

---

## 11. AndroidAPS reads the pump: what was actually wrong

Against a real Minimed 722 (serial REDACTED) driven by AndroidAPS, the device now
completes pump communication:

```
PumpModel: [raw=722, resolved=Medtronic_722]
Medtronic 523/723 (Revel) REDACTED
MedtronicPumpHistoryDecoder ... (history downloaded)
postProcessSettings: Max Bolus / Max Basal read
pumpDeviceState=Active
```

Firmware-side, the same window: **30 consecutive replies**, every one a full
107-byte packet decoding to 71 bytes, response times ~115 ms.

Zero `No response from RileyLink` after this build was flashed. All 727 in the
AndroidAPS log fall between 20:32 and 21:02 and belong to earlier builds.

### The four real defects

1. **`CONFIG_BT_ATT_PREPARE_COUNT=0`** -- long writes rejected at the ATT layer, so
   every real pump command was refused before reaching the APS handler. The legacy
   firmware ran `nrf_ble_qwr` for exactly this. `verify_gatt.py` passed 41/41
   because it only ever issued simple writes.
2. **No MTU negotiation** -- legacy's `nrf_ble_gatt` did it automatically. Without
   it the link stays at 23 bytes and the client is forced into long writes.
3. **FIFO not drained before RX** -- residue from the preceding transmit was read
   immediately, and a `0x00` among it tripped the terminator check, collapsing
   every listen window to zero length.
4. **Enqueue preemption** -- see below.

### Preemption: faithful to legacy, and wrong

Legacy called `Subg_SetIntFlg()` after a successful enqueue, aborting any
in-flight receive. Reproducing it created a feedback loop: an aborted listen
answers "no reply" in ~360 ms, AndroidAPS reads that as failure and retries
immediately, and the retry aborts the next listen. Seventeen interrupted receives
and zero replies across a full sweep, against a pump that answers in 115 ms when
a listen is allowed to run.

Removed. Commands queue (depth 4) and each listen runs its window. Abort is kept
for disconnect, where cutting a listen short is correct.

**Legacy fidelity is a default, not a goal.** Legacy's behaviour assumed a client
that would not retry into the abort; AndroidAPS does.

### Two of my own errors worth recording

- **`tools/probe_722.py` double-encoded.** AndroidAPS sends the payload *raw* and
  lets the firmware apply the encoding selected by `CMD_SET_SW_ENCODING`. My probe
  pre-encoded 4b6b *and* asked the firmware to encode. Every silent probe I ran
  was my tooling. Worse, I used that broken probe to "clear" the payload-length
  hypothesis -- a conclusion that happened to be right, reached by an invalid
  experiment.
- **I misread the AndroidAPS log twice.** `Got data [DD]` looked like a stale
  response; it was the correct acknowledgement to a register write.
  `mDataQueue size is 421` looked like runaway desync; it was an abandoned reader
  instance from a previous connection. Both sent me chasing problems that did not
  exist.

### Still open

`pumpDeviceState=Sleeping` at 21:04:33, then `WakingUp` -> `PumpUnreachable` at
21:07. Initial communication succeeds; waking the pump after it sleeps does not.
`decodeModel` returns `Unknown_Device`, so replies arrive but do not decode --
distinct from the "no response" failures above and not yet diagnosed.

The wakeup burst is `CMD_SEND_AND_LISTEN` with `repeatCnt=200`. At ~13 ms per
frame that is ~2.6 s of transmission; the legacy timing works out similar, so
burst duration is not obviously the cause. Needs measurement, not assumption --
the last time I reasoned about this without measuring I was wrong twice.

---

## 12. Wake-from-sleep, and what a HackRF settled

Three further firmware defects, plus one AndroidAPS configuration problem.

### 12.1 RSSI was sampled after leaving RX

```c
rf69_set_mode(RF69_MODE_STANDBY);
...
last_rssi = rf69_read_rssi(false);   /* meaningless here */
```

`RegRssiValue` is only valid while the receiver is running. Read after a switch to
standby it returns a stale value, so every reply carried a nonsense RSSI.

That matters more than it looks: **mmtune ranks candidate frequencies purely by
the RSSI of the reply.** With garbage there, the scan could never pick a winner,
no `lastGoodFrequency` was ever recorded, and AndroidAPS re-tuned on every single
connection -- 15 tune runs and 74 scans in one session, always ending on the
916.550 fallback rather than a measured best.

The legacy driver never left RX inside `minimed_rx`, so it read a live value by
construction. Leaving RX is still correct; the read simply has to come first.

### 12.2 A repeat burst abandoned itself on one failed frame

`subg_send_pkt()` returned on the first error from `minimed_tx()`, so a single
missed `PacketSent` inside a 201-frame wakeup burst truncated the whole thing --
silently, because the command still returned a sensible status afterwards. Waking
a sleeping pump depends on a sustained sequence. Failures are now counted and the
burst continues, which is what the legacy driver did by having no error path here
at all.

### 12.3 Frequency changes landed in the middle of a transmit burst

`CMD_UPDATE_REG` runs inline so it cannot be dropped behind a long listen
(section 8.2). But applying it inline retuned the radio *during* a transmit: a
201-frame burst takes ~3.4 s, AndroidAPS writes three frequency registers inside
that window, and the burst ended up scattered across three frequencies. A sleeping
pump never hears a coherent burst on any of them.

Register writes are now recorded in the BLE callback and applied by the APS thread
immediately before the next radio command. The frequency is therefore stable for
the whole duration of any command -- and as a bonus the BLE callback no longer
touches a radio register at all, which removes the SPI race that motivated
queueing this command in the first place.

### 12.4 Tune is now verified by read-back

`apply_pending_freq()` reads `FRF` back and logs a mismatch:

```
tuned to 916549804 Hz (radio reads 916549743)
```

61 Hz is integer rounding in the FSTEP conversion. This exists because a HackRF
capture showed bursts on 916.5477 MHz -- the config-table default -- while the log
claimed a different frequency, and I concluded the tune never reached the chip.
**That conclusion was wrong**: the captured bursts simply happened while the radio
genuinely was at the default, and I had not correlated timestamps before drawing
it. The read-back makes the question unambiguous rather than inferential, which is
why it stays.

### What the HackRF established

Independent confirmation of things that were previously only inferred:

| Measured | Result |
|---|---|
| Are we radiating? | Yes -- 31 dB over noise at a few inches |
| Carrier frequency | 916.5477 MHz, matching the commanded value |
| Burst duration | 3442 / 3443 / 3441 ms, matching `tx burst: ... 3441 ms` exactly |
| Burst coherence | Single frequency, ~98% duty cycle across the burst |

The first capture showed nothing at all and looked alarming; the gain was simply
too low (`-a 0 -l 8 -g 12`). At `-a 1 -l 24 -g 30` the bursts are obvious. Worth
remembering before concluding a transmitter is dead.

### Current state

```
tx burst: 11 bytes x201 -> 201 sent, 0 failed, 3446 ms
rx: 11 bytes  after 342 ms, rssi -93 dBm
rx: 107 bytes after 116 ms, rssi -90 dBm
```

The pump answers the wakeup burst and RSSI is now a real measurement.

**Open:** -86 to -93 dBm is weak for a pump a few inches away. The values are
plausible rather than garbage, so mmtune can rank frequencies -- but if the
reading is taken slightly late, or coupling is poor, tune quality suffers. Worth
measuring against the HackRF's own estimate of the pump's signal.

### Not a firmware problem: the reservoir

AndroidAPS was configured as a 523/723 Revel while the pump is a 722. From its
decoder:

```kotlin
val strokes = pumpModel.bolusStrokes   /* 40 for Revel, 10 for 522/722 */
if (strokes == 40) startIdx = 2
```

On `0B 3E 00 00 ...`, a 722 reads `0x0B3E` = 287.8 U; a Revel reads offset 2,
`0x0000` = 0.0. Its own `decodeModel` detected `raw=722` and discarded it, because
a configured model is never overridden:

```kotlin
if (!medtronicUtil.isModelSet) { medtronicUtil.medtronicPumpModel = pumpModel }
```

Same cause as the impossible `Max Bolus 6400` / `Max Basal 1574.4` warnings. Fixed
by setting the pump type to 522/722 in the app.

## 13. "Tuning is slow": measuring instead of guessing

Reported symptom: getting to the point where AndroidAPS syncs history pages takes
far longer than on the legacy firmware; once syncing starts, speed is normal.

Two consecutive attempts to fix this by optimising the TX burst made things worse
(9.5 below). This section is what happened when the path was finally profiled
rather than reasoned about.

### 13.1 The TX path is airtime-bound: 2.3% is software

Per-stage timing inside `minimed_tx()` for a real 11-byte frame, on hardware:

```
tx profile len=11: standby=0us clear=30us write=122us ->tx=213us drain=15411us
```

365 us of software against **15411 us of airtime -- 2.3% overhead.** The frame is
16-byte preamble + 4-byte sync + 12-byte payload = 256 bits at 16384 bps = 15.6 ms,
which is what `drain` measures. A 201-frame burst takes 3.34 s wall clock, measured,
and essentially all of it is radiated.

There is nothing to win here. Earlier notes claimed "16.4 ms/frame against 9.3 ms
airtime, 43% overhead" -- that airtime figure was computed without counting the
preamble and sync word, and every optimisation attempt built on it was chasing
7 ms that never existed.

Legacy is not faster: `legacy/periph/rf69/rf69.c` uses the same
`RF_BITRATEMSB_16384` and the same `RF_PREAMBLESIZE_LSB_VALUE` (0x10 = 16 bytes),
so its frames cost the same 15.6 ms.

### 13.2 The delay is AndroidAPS-side, and not in the burst at all

Gaps between successive commands from AAPS, one session, RTT timestamps:

```
tune phase   7.830  7.831  7.829  7.830  7.920  9.091  9.090  ...
sync phase   0.450  0.390  0.420  0.480  0.450  0.720  0.810  ...
```

Both phases issue the **same** command -- `14 05 00 00`, `CMD_SEND_AND_LISTEN`
with `repeatCnt = 0`, one frame -- differing only in listen timeout (1250 ms vs
4000 ms) and pump opcode. Firmware radio time is 133-134 ms in both.

So during tune the firmware is **idle for ~7.7 s per command**, waiting on AAPS.
The 9.090 s gaps are 7.83 + 1.26, i.e. the same fixed delay plus a 1250 ms listen
that timed out -- which confirms the gap is `fixed AAPS delay + radio time`.

16 tune steps x 7.83 s is the ~125 s the user observed. None of it is transmit time,
and no firmware change can shorten it.

A further wrong premise died here too: the claim that AAPS sends "31 commands with
`repeatCnt=200`, ~102 s of bursts". In the captured session exactly **two**
commands used `repeatCnt=0xc8`. The histogram of every command sent:

```
  25  14 05 00 00     send_and_listen, repeatCnt=0
   2  14 05 00 c8     send_and_listen, repeatCnt=200
  19  03 06 xx xx     update_reg (frequency)
```

### 13.3 RSSI was measured on empty air (the actual bug this found)

Reply RSSI on consecutive identical commands at a fixed frequency, pump inches
away: `-51, -94, -93, -92, -79, -59, -56, -95, -96`. A 45 dB swing across a static
bench setup is not physical.

12.1 moved the RSSI read to before the switch out of RX, which was necessary but
not sufficient: it still sat *after* the whole packet had been drained from the
FIFO. `RegRssiValue` is a live measurement of present received power, not a
per-packet latch, so by then the pump had stopped transmitting and the reading was
the noise floor -- the same -92..-97 dBm the boot-time survey reports for empty air.

RSSI is now latched on the **first byte** of the packet, while the carrier is
certainly still up. Measured on hardware, replaying the byte-identical AAPS frame:

| | before | after |
|---|---|---|
| replies at/below -90 dBm | 13 / 30 (43%) | 0 / 14 (0%) |
| median | -78 dBm | -62 dBm |
| range | -96 .. -50 | -68 .. -50 |

After the fix, ten consecutive reads at a fixed frequency give -50 to -54 dBm, a
4 dB spread, which is what a static bench setup should look like.

Because mmtune ranks frequencies purely by reply RSSI, a floor reading on ~43% of
replies made the ranking meaningless -- the same failure mode as 12.1, one layer
further in.

### 13.4 The mode cache: added, profiled, removed

Legacy `Rf69_SetMode()` caches the current mode and returns early when it already
matches. That was ported here as a TX speedup, then removed once 13.1 showed
software is 2.3% of a frame -- skipped register writes cannot matter.

It also is not free. Legacy's own `minimed_tx()` comment says *"Rely on the
sequencer to end Transmit mode after PacketSent is triggered"*, so after every
transmit the hardware mode no longer matches the cache and a stale entry silently
suppresses a later mode write. Carrying that failure mode for 0% is a bad trade.

Worth noting it never helped legacy's TX loop either: each frame explicitly sets
STANDBY then TX, so the cached value always differs and both writes always happen.

### 13.5 Tooling errors, again

Two self-inflicted detours while verifying the above, both the same shape as 10.x:

* A replay script sent pump frames without first issuing `CMD_SET_SW_ENCODING`, so
  the firmware transmitted raw bytes. The RTT line `7 B payload -> 7 B encoded`
  (instead of `-> 11 B`) is the tell; the pump silently ignored everything.
* A hand-computed wake-frame CRC was wrong (0x59 against the correct 0x4e). Caught
  only by generating it with `tools/minimed.py` instead of by hand.

**Lesson, restated: replay captured bytes verbatim wherever possible.** The
measurements in 13.3 use the exact 21-byte frame AAPS sent, copied from an RTT
capture, precisely so no local encoding step can invalidate the result.

## 14. Status LED and battery monitor

### 14.1 The LED now shows charge, not BLE state -- deliberate

Legacy drove two discrete active-low LEDs, LED_0 (yellow) and LED_1 (red), as a
bitmask, and used them for **BLE state**:

| Legacy state | Indication |
|---|---|
| `INDICATE_CONNECTED` | 30 ms yellow flash every 10 s |
| `INDICATE_ADV` / `INDICATE_DISCONNECTED` | 30 ms red flash every 10 s |
| `INDICATE_LOW_POWER` | red steady |
| `INDICATE_NONE_SN` | yellow steady |
| boot | red 300 ms, yellow 300 ms, off 400 ms, once |

Note that advertising and disconnected were visually identical, so the LED really
only distinguished "connected" from "not connected".

The XIAO has one onboard RGB device (led0 red P0.26, led1 green P0.30, led2 blue
P0.06, all active-low), so this became a colour model rather than a two-pin
bitmask -- yellow is red+green together.

**Decision: the LED shows battery charge instead of BLE connection state.** Charge
was judged more useful on a device whose connection state is already visible in the
phone app. Consequences, stated plainly:

* `src/indication/indication.c` -- the legacy `Idc_SetType()` priority machine --
  **is not ported at all.** With colour spoken for by charge, there is nothing for
  it to drive.
* There is no longer any local indication of connected vs advertising.
* `INDICATE_NONE_SN` and the factory-test indications have no equivalent.

What *was* kept from legacy: every timing constant (`LED_TIME1` 30 ms,
`LED_TIME2` 300 ms, `LED_TIME3` 400 ms, `LED_TIME4` 10000 ms) and the boot
red/yellow/off announcement. A 30 ms flash every 10 s is ~0.3% duty, which is what
makes a permanently-on indicator affordable on a cell.

The two parallel legacy tables (`RED_LED_TWINKLE_PERIOD` / `RED_LED_TWINKLE_OP`,
carrying the comment "must be defined according to the order of led control
period") are folded into one table of `{ms, colour}` steps, so they cannot drift
apart.

### 14.2 The legacy battery code is not a LiPo profile and was not ported

`app_battery.c` was written for a ~3.2 V cell:

```c
#define BAT_MAX_VOLTAGE 3200                       /* mV */
voltageTable[]    = {3100, 3000, 2900, 2800, 2700, 2600, 2500, 2550, 2400};
percentageTable[] = { 100,   90,   80,   70,   60,   50,   40,   30,   20};
```

A single-cell LiPo is 4.2 V charged with a ~3.0 V floor, so this curve is not
merely miscalibrated -- it is the wrong chemistry and the wrong voltage range.
Reused as-is it would peg a healthy LiPo off the top of the table and call a flat
one healthy. **The scaling, the curve and the thresholds in `src/battery/` are all
new.**

Two defects in the legacy version, recorded because they are easy to reintroduce:

1. **A dead table entry.** `{..., 2500, 2550, 2400}` is not monotonic, and the
   lookup is "first entry below the measured voltage". At 2520 mV the search breaks
   on 2500 and returns 40%; the 2550 mV row (30%) is unreachable for any input.
2. **`BAT_ADC_MAX_VALUE 700`** is a raw-count constant with no stated reference or
   gain, so the conversion cannot be checked against the hardware by reading the
   code. Replaced with `adc_raw_to_millivolts_dt()` plus an explicit divider ratio.

### 14.3 Hardware specifics

**Pin change.** Legacy sampled P0.04 / AIN2 through an external divider. The XIAO
reads AIN7 / P0.31 through its own onboard divider, gated by P0.14.

**Sense enable is held on, not pulsed.** Seeed document that with P0.14 high the
sense path is disabled and P0.31 may rise toward 3.6 V, risking the pin. Legacy
pulsed its ADC around each sample; here the sense path is enabled once at init and
left enabled, which removes the hazard window. Cost is the divider's standing
current, about 2.8 uA at 4.2 V through ~1.5 Mohm.

**ADC gain corrected.** The overlay originally specified `ADC_GAIN_1_6`, giving a
3.6 V full scale against a divider output of ~1.42 V for a 4.2 V cell -- 39% of
range. Changed to `ADC_GAIN_1_4` (2.4 V full scale, ~59% of range). Measured on
hardware: `raw=2150 pin=1259 mV` out of 4095, i.e. 52% of scale, no clipping.

If a different divider ratio ever puts the output above ~0.57 of the cell voltage
this would clip, and clipping is invisible -- every voltage above the limit reads
identically, which looks like a healthy battery regardless of the real state. The
module therefore warns when a reading pins at full scale.

**Divider ratio, calibrated on the bench unit.** The module logs one calibration
line at startup -- raw count, pin millivolts, derived cell millivolts.

Measured: pin `1259 mV` where a meter read **3.78 V** across the cell. That gives a
true ratio of 3.0024 against the nominal 2.9608 for 1M / 510k, i.e. **1.4% high** --
consistent with 1% resistors plus ADC gain and offset error.
`CONFIG_ORANGELINK_BATTERY_FULL_OHMS` is therefore set to **1531215**, not the
nominal 1510000.

That 1.4% was not cosmetic: it moved the reading from 3727 mV / **19% (red)** to
3780 mV / **32% (yellow)**, across a threshold. A small ratio error matters most
exactly where the curve is steep, which is where the warning colours live.

`FULL_OHMS` is consequently a *calibration handle* rather than a resistance -- it
absorbs resistor tolerance and ADC error as well as the divider ratio, so the
calibrated value is expected to deviate from the nominal part values. Recalibrate
per board: scale it by `measured / derived`.

### 14.4 Thresholds

Green at or above 60%, yellow 20-59%, red below 20%, with 3% hysteresis before the
colour is allowed to rise again.

Red begins at 20% rather than 15% because below roughly 20% a 1S LiPo is on the
steep part of its curve (about 3.6 V falling to 3.3 V) and the remaining runtime is
short, so a warning at 15% arrives with little margin. The hysteresis exists
because a cell resting on a threshold will cross it repeatedly under a bursty
transmit load; without it the colour changes every sample.

Percentage comes from a piecewise-linear 1S LiPo open-circuit curve rather than a
straight line between 4.2 V and 3.0 V, because a LiPo sits near 3.8 V for most of
its usable charge -- a linear map would read ~50% for most of the discharge and
then collapse. It is good enough to pick a colour; it is not a fuel gauge, and
during a transmit burst the measured voltage sags and reads low.

### 14.5 The BLE Battery Service reported a permanent 100%

`CONFIG_BT_BAS=y` was already set, but **nothing ever called
`bt_bas_set_battery_level()`**. Zephyr's BAS initialises its characteristic to 100,
so every client -- AndroidAPS included -- read "100%" regardless of the cell. The
value was not stale or miscalculated; it had never been written at all.

Legacy did push this, from a timer handler:

```c
err_code = ble_bas_battery_level_update(&m_bas, Batt_GetLevel(), BLE_CONN_HANDLE_ALL);
```

The port now sets the level on every battery sample. Worth noting the failure mode:
an enabled-but-unwritten service is worse than a missing one, because the client
displays a plausible value instead of nothing.

### 14.6 Cost

FLASH 45.39% (196 KB of 442 KB), RAM 18.59% (48728 B of 256 KB) with both modules
enabled. Both are behind Kconfig (`ORANGELINK_LED`, `ORANGELINK_BATTERY`).

### 14.7 There is currently no flashing path except SWD

Discovered while trying to recover from a failing debug probe. Worth writing down
because it turns a loose jumper wire into a hard stop.

`dts/orangelink-partitions.dtsi` deletes `partition@f4000`, and MCUboot occupies
`0x00000000`-`0x0000C000`. On the XIAO, `0xf4000` is where the factory Adafruit UF2
bootloader lives and `0x0` is where its MBR lives, so:

* **UF2 / double-tap reset does not work.** The nRF52840 boots from `0x0`, which is
  now MCUboot, so the Adafruit bootloader is never entered even if its bytes are
  still physically present at `0xf4000` (our flashes only ever programmed `0x0` and
  slot0, so they were never erased -- merely orphaned). The build still emits
  `zephyr.uf2`; it is a dead artifact on this board.
* **MCUboot serial recovery is not enabled** -- nothing in `sysbuild/mcuboot.conf`
  turns it on.
* **SMP / mcumgr is not in the application.** MIGRATION_NOTES 2.2 records the
  intent that SMP-over-BLE replaces the legacy buttonless DFU service, but it has
  not been implemented, so there is no over-the-air path either.

So SWD is the only way to program this board, and restoring the UF2 bootloader
would itself require SWD -- the recovery path depends on the thing that broke.

**Resolved by 14.9: SMP-over-BLE DFU is now implemented.** It needs no pins, works
over the link AndroidAPS already uses, and removes the single point of failure.
MCUboot serial recovery over USB CDC was the alternative, but it needs a GPIO to
enter recovery (D1 and D3 are free).

Restoring UF2 instead was considered and rejected: the board's stock layout
(`nordic/nrf52840_partition_uf2_sdv7.dtsi`) puts the application at `0x27000` with
the bootloader at `0xf4000`, and Zephyr's `xiao_ble_defconfig` does set
`CONFIG_BUILD_OUTPUT_UF2=y` -- so it is genuinely supported. But it costs MCUboot's
signed dual-slot images and rollback, and it still needs one reliable SWD flash of
the Seeed bootloader package to rebuild `0x0`. Flashing a bootloader over an
intermittent probe risks a partial write with no recovery path at all, which is a
worse failure than the one being fixed.

### 14.8 Verifying firmware without a probe

With SWD down, the Battery Service itself confirmed the flash had taken: reading
characteristic `0x2A19` returned **26%**, where the pre-fix firmware returns a
constant 100. 26% corresponds to ~3.75 V on the curve, consistent with the 3.78 V
measured earlier less some runtime on the cell.

One trap, recorded because it produced a confidently wrong answer first time:
`BleakScanner.find_device_by_name("OrangePro")` resolved a **stale cached device**
at `D1:A9:95:54:8E:38`, not the board at `E6:B5:4D:8C:C1:B9`, and that device
reported 100%. A rescan showed only one `Orange*` device actually advertising.
**Pin verification reads to an address, never a name** -- a name lookup can silently
answer from a different device and the result looks perfectly plausible.

### 14.9 SMP-over-BLE DFU, and a Kconfig trap worth knowing

Implements MIGRATION_NOTES 2.2 and closes 14.7: upgrades now go over BLE with nRF
Connect Device Manager, so a dead SWD probe no longer means no way to flash.

**A silent Kconfig failure nearly shipped a no-op.** The first build looked
successful and added only ~6 KB, which was the tell -- mcumgr and an SMP service
cannot cost 6 KB. `CONFIG_MCUMGR` `depends on NET_BUF && ZCBOR`, and **Kconfig
ignores an assignment whose dependencies are unmet without emitting any warning**.
So `CONFIG_MCUMGR=y`, `CONFIG_MCUMGR_TRANSPORT_BT=y` and `CONFIG_MCUMGR_GRP_IMG=y`
were all silently dropped, the build passed, and the image contained no DFU at all.

It only surfaced by checking the artefact rather than the build result:

```
$ grep -E '^CONFIG_MCUMGR' build/zephyr/.config     # absent
$ nm zephyr.elf | grep -ciE 'smp_bt|img_mgmt|mcumgr'
0
```

After adding `CONFIG_NET_BUF=y` and `CONFIG_ZCBOR=y`:

```
$ nm zephyr.elf | grep -c ...   ->  96
$ nm zephyr.elf | grep smp_bt_svc
00039a74 R smp_bt_svc
```

**Lesson: a Kconfig option that does not appear in the resulting `.config` is not
an error, and an undefined *symbol* warns while an unmet *dependency* does not.
Verify a feature landed by looking for its symbols in the image, not by the build
exiting zero.** (Only `MCUMGR_GRP_IMG_UPLOAD_CHECK_HASH`, which does not exist in
this Zephyr, produced a real warning and aborted the build.)

Cost, measured: FLASH 45.39% -> **48.51%** (196 KB to 214508 B, about +18 KB), RAM
18.59% -> **23.68%** (+13 KB).

**Images self-confirm on a delay, not at boot.** MCUboot runs a freshly uploaded
image in test mode and reverts on the next reset unless something confirms it.
`main()` therefore schedules `boot_write_img_confirmed()` 60 s after startup rather
than calling it immediately -- confirming at boot would make every upload
permanent, including one that crashes seconds later, and over-the-air that leaves
no way to upload a replacement. An image that cannot stay up for a minute rolls
itself back.

**Advertising data is deliberately unchanged.** AAPS matches on it and that path is
working, so the SMP UUID is not advertised; Device Manager can connect and discover
the service anyway. If it ever fails to list the device, the SMP UUID
(`8D53DC1D-1DB7-4CD3-868B-8A527460AA84`) fits in `scan_rsp` at 29 of 31 bytes,
leaving the primary advertising packet untouched.

**Bootstrap caveat.** This build has to reach the board over SWD once before
wireless upgrades are possible, and at time of writing the probe is down
(`Unexpected ACK '0'` on 8 consecutive attempts), so it is committed but not
deployed. The board is still running the previous image -- which does have the
calibration and Battery Service fixes, confirmed by reading 0x2A19 = 26%.

## 15. SWD died, and why it was two faults stacked

Flashing went from "works" to "one attempt in ten" to "never". It turned out to be
two independent problems, and treating them as one wasted several rounds.

### 15.1 Reading the failure signatures

pyOCD's error text distinguishes them precisely, once you stop treating every
failure as "the probe is broken":

| Signature | Layer | Meaning |
|---|---|---|
| `No ACK` | physical | Nothing is driving the bus. Bad joint, wrong pin, no GND. |
| `Unexpected ACK 'n'` | physical | Bus is driven but the framing is garbled. Marginal joint. |
| `FAULT ACK` reading AP#0 IDR | protocol | **Link is fine.** The DP answered; the AHB-AP is disabled. |
| `SoCTarget has no selected core` | protocol | Follows the above -- no core reachable behind a disabled AP. |

**Clock rate is the discriminator.** Signal-integrity faults improve as the clock
drops; 18 attempts across 6 rates from default down to 50 kHz gave *identical*
results, which ruled signal integrity out for that round. Later, after rewiring,
50 kHz produced a clean `FAULT ACK` -- a different fault entirely.

A tally beats a single attempt. Six runs at 50 kHz gave 4x `No ACK` and 2x
`FAULT ACK`, which is what "two faults stacked" looks like: the physical layer is
intermittent, and when it does come up, APPROTECT is waiting behind it.

### 15.2 CORRECTION: APPROTECT was never the problem, and I caused a real one

**This section previously claimed APPROTECT was re-arming every boot and that
programming `UICR->APPROTECT = 0xFFFFFF5A` was the fix. That was wrong, and acting
on it locked the chip.**

The reasoning error was misreading which enum value applies to this part:

```c
#define UICR_APPROTECT_PALL_Enabled     (0x00UL)
#define UICR_APPROTECT_PALL_HwDisabled  (0x5AUL)  /* hardware AND software controlled */
#define UICR_APPROTECT_PALL_Disabled    (0xFFUL)  /* hardware controlled */
```

`0xFF` is itself a *disabled* value. So the erased `0xFFFFFFFF` already meant
protection off, and the premise "UICR is not 0x5A, therefore the chip re-protects
itself" was false. Writing `0x5A` -- the value for hardware-and-software-controlled
devices -- **enabled** protection on this one:

```
NRF52840 APPROTECT enabled: not automatically unlocking [target_nRF52]
Error: 'SoCTarget has no selected core'
```

Recovery was a CTRL-AP mass erase, which resets UICR to `0xFFFFFFFF`. Worth
recording: **pyocd reported the mass erase as failed, but it had completed inside
the chip** -- ERASEALL is a single register write and the part erases internally,
so pyocd only lost the link while polling ERASEALLSTATUS. Reading `0x10001208`
back showed `ffffffff` and the lock was gone. Always read back before concluding a
destructive operation failed.

The `FAULT ACK` readings that started this were real, but they were a *symptom of
the power fault in 15.3*, not of access port protection.

`tools/uicr-approtect-hwdisabled.hex` and `tools/recover-swd.sh` are deleted
rather than fixed. Nothing in this project should write UICR->APPROTECT.

### 15.3 The actual cause: flashing from battery power

The board was running on a part-charged LiPo with USB-C disconnected.

Register reads succeeded. A 200 KB erase/program died partway with
`Unexpected ACK '0'` -- **at every clock rate from 2 MHz down to 50 kHz**, and
under every `--connect` mode. Erase and program draw far more current than reads,
so the cell sagged and the link dropped mid-transfer.

With USB-C connected, MCUboot flashed on the second attempt and the 217 KB
application on the first.

This is the diagnostic that matters, and it is the inverse of the usual advice:

> Short transfers succeeding while long ones fail, *with no sensitivity to clock
> rate*, is a power problem, not a signal-integrity problem. Clock rate is the
> discriminator -- signal integrity improves as the clock drops; a sagging supply
> does not care.

The cost of not knowing this: each failed attempt left slot0 partly erased, so
fifteen retries bricked a board that had been working, and recovery then needed
MCUboot reflashed too.

### 15.4 pyocd commander exits 0 on a fatal error

Two successive versions of the recovery script reported a healthy link on a board
that could not be flashed at all, because both gated on exit status:

```
$ pyocd commander -t nrf52840 -c "read32 0x10000000"
Error while initing target: SWD/JTAG communication failure (Unexpected ACK '0')
$ echo $?
0
```

`pyocd flash` sets a correct exit code; **`pyocd commander` does not.** The script
now gates on the output containing the expected value *and* containing no error
text.

**Lesson, and it is the same one as 14.9 and 13.5: verify the effect, not the
return code.** An exit status, a build succeeding, and a Kconfig assignment are all
claims about what was attempted, not evidence of what happened.

### 15.5 Recovery outcome

Chip fully erased and reflashed from a blank state. Verified over BLE rather than
by trusting the flash log:

```
Orange* devices advertising: 1
  E6:B5:4D:8C:C1:B9  'OrangePro'
  SMP/DFU service 8d53dc1d-1db7-4cd3-868b-8a527460aa84: PRESENT
    characteristic da2e7828-fbce-4e01-ae9e-261174997c48: present
  Battery Level = 30%
  total GATT services: 5
```

Radio self-test after the erase: `ALL PASSED`, `VERSION = 0x24`,
`PA: PA0=0 PA1=1 PA2=1 power=27`.

The BLE address survived the mass erase, so AndroidAPS does not need re-pairing --
the identity is derived from the chip, not from the settings partition. What *was*
lost with the erase: the settings/NVS contents (custom device name and indication
toggles) and the orphaned Adafruit UF2 bootloader at `0xf4000`, so 14.7's UF2
option is now permanently gone rather than merely unreachable.

Wireless DFU is live, which is the point: the next update needs no probe.

## 16. Device renamed, and charger current raised

### 16.1 "OrangePro" -> "ClaudeLink"

One line, because the name was already centralised:

```
CONFIG_BT_DEVICE_NAME="ClaudeLink"
```

`src/main.c` derives `ORANGELINK_DEFAULT_NAME`, the scan-response payload and the
initial IPS custom name from that symbol, so nothing else changed. The name lives
in the scan response rather than the advertising packet (see 8.x), which has room
to spare -- 10 characters plus 2 bytes of header against a 31-byte budget.

**AndroidAPS identifies the device by name, so its configuration must be updated**
-- re-select the device in AAPS after this change. The BLE address is unchanged
(`E6:B5:4D:8C:C1:B9`; it derives from the chip, not from the settings partition),
so nothing needs re-pairing.

### 16.2 Charger current: ~50 mA -> ~100 mA

The XIAO selects charge current with P0.13: high-impedance input gives ~50 mA, an
output driven low gives ~100 mA. Both apply during the constant-current phase only;
the current tapers as the cell fills.

Declared ACTIVE_LOW so the intent reads directly in code -- `GPIO_OUTPUT_ACTIVE`
drives the pin low and selects the higher current, while `GPIO_INPUT` leaves it
floating for the lower one:

```
chg_current: chg_current {
        gpios = <&gpio0 13 GPIO_ACTIVE_LOW>;
        label = "Charge current select";
};
```

Set once in `battery_init()`, gated by `CONFIG_ORANGELINK_BATTERY_FAST_CHARGE`
(default y). A failure is logged but not fatal -- it only leaves the charger at its
default rate.

> **Check the cell before enabling this.** 100 mA is 1C for a 100 mAh pack and 2C
> for a 50 mAh one, above what small cells are rated to accept. The Kconfig help
> says to leave it off below roughly 200 mAh.

The bench unit runs an **1800 mAh** cell, so 100 mA is **0.056C** -- nowhere near a
safety limit, and the only real consideration is time:

| | from 36% | from empty |
|---|---|---|
| 100 mA (this setting) | ~15 h | ~23 h |
| 50 mA (fast charge off) | ~30 h | ~47 h |

100 mA is the XIAO charger's maximum, so there is no firmware route to charging
faster than the left column -- that would need external charging hardware. The
figures include roughly 1.3x for the constant-voltage taper.

A large cell also makes the readings steadier: voltage sag under a transmit burst
scales with C-rate, and a 1800 mAh pack barely notices the radio, so the
threshold hysteresis in 14.4 matters less here than it would on a small cell.

Verified in hardware rather than from the log, because the RTT window closed before
the message was emitted:

```
PIN_CNF[13] = 0x00000003   DIR=output, input buffer disconnected, no pull
DIR  bit13  = 1            output
IN   bit13  = 0            driven low -> ~100 mA
```

Charge observed climbing 30% -> 36% over the session, consistent with the higher
rate.

**Method note.** A first pass at decoding `DIR` by hand got bit 13 wrong and very
nearly recorded a working feature as broken. Reading `PIN_CNF[n]` directly is
better than masking `DIR`/`IN` by eye: it reports direction, input buffer, pull and
drive for one pin in a single word. Same lesson as 13.x, 14.9 and 15.4 -- check the
artefact, and do the arithmetic with a tool.

## 17. Power and sleep: one real regression, now fixed

Asked directly whether the port mirrors the original's power behaviour. Mostly yes;
one thing was badly wrong.

### 17.1 The RFM69 was never put to sleep

Legacy's lifecycle keeps the radio asleep unless it is actually in use:

| Legacy site | Action |
|---|---|
| `Rf69_DevParaCfg()` | ends with `Rf69_SetMode(dev, RF69_MODE_SLEEP)` |
| `Subg_SendPkt()` | `rf_stop()` after the repeat loop -> SLEEP |
| `Subg_GetPkt()` | `rf_stop()` after the receive -> SLEEP |

This port set `RF69_MODE_STANDBY` on **every** path and never once used
`RF69_MODE_SLEEP` -- the enum value and its register mapping existed with no call
site. The boot register dump showed it plainly: `0x01 OPMODE = 0x04`, which is
`RF_OPMODE_STANDBY`.

Datasheet-typical for the SX1231/RFM69 is **1.25 mA in standby against 0.1 uA
asleep**, so the radio was awake permanently and dominated the idle budget by more
than an order of magnitude over everything else on the board.

Rough figures for the 1800 mAh cell -- **estimates from datasheet numbers, not
measurements**, and worth confirming with a meter:

| | idle current | 1800 mAh lasts |
|---|---|---|
| before (radio in STANDBY) | ~1.3 mA | ~8 weeks |
| after (radio asleep) | ~50-80 uA | well over a year, where self-discharge starts to dominate |

Fixed by mirroring legacy exactly: SLEEP at the end of `rf69_config_916()`, once
per burst at the end of `subg_send_pkt()` (after the repeat loop, where
`Subg_SendPkt()` puts `rf_stop()`, not per frame), and on every exit path of
`subg_get_pkt()` including abort and timeout. The boot loopback also sleeps the
radio when it finishes, so the register dump reports the idle state rather than
whatever the last test left behind -- otherwise the invariant is unverifiable from
the log.

SPI still works with the part asleep, so the deferred frequency writes in
`apply_pending_freq()` do not need it woken first.

Verified on hardware: `OPMODE = 0x00` at idle, self-test `ALL PASSED`, and the pump
answered **8/8 model reads at -53 to -54 dBm** afterwards -- so the saving costs
nothing in reliability, which is unsurprising given legacy did the same thing.

### 17.2 What already matched

* **Wakeup sources.** LED heartbeat 30 ms every 10 s (legacy `LED_TIME1`/`LED_TIME4`),
  battery sampling every 180 s (legacy `BAT_LOW_DET_INVL`), IPS timer tick every
  60 s and only while connected (legacy `BLE_TMR_TICK_ONE_MIN`), advertising at
  300 ms. The RFM69 retest work correctly stops once the radio answers rather than
  polling forever.
* **CPU idle.** Legacy ran `nrf_pwr_mgmt_run()` in its super-loop; here `main()`
  returns and Zephyr's idle thread handles it, entering System ON sleep on WFI/WFE.
  Equivalent in effect.
* **Logging.** Legacy shipped with logging enabled -- `KIT_LOG_SUPORT` is defined in
  `project/app/config/kit_config.h` -- so keeping it on is parity, not a deviation.

### 17.3 Remaining gaps, not yet addressed

Both are secondary to 17.1 and neither has been measured:

* ~~**UART console and UART log backend are enabled**~~ -- **done, see 17.4.**
* **No `CONFIG_PM_DEVICE`**, so peripherals are never suspended. Notably the legacy
  driver de-initialised the SPI bus between transfers while this port leaves it
  bound to Zephyr permanently -- a deliberate deviation recorded in the overlay for
  correctness reasons, but it has a power cost that has not been quantified.

The honest next step for both is a current measurement rather than more reasoning
from datasheets.

### 17.4 UART removed; RTT is the only backend

Turning off `CONFIG_LOG_BACKEND_UART` alone would have saved nothing. The board marks
`uart0` `"okay"`, so with `CONFIG_SERIAL=y` the nrfx UARTE driver still initialises
the peripheral at boot whether or not anything logs to it.

**Disabling `CONFIG_UART_CONSOLE` directly does not work either.** The board sets
`BOARD_SERIAL_BACKEND_CDC_ACM=y`, and that block in
`boards/common/usb/Kconfig.cdc_acm_serial.defconfig` carries
`config UART_CONSOLE default CONSOLE`, which overrides an explicit `n` from
`prj.conf`. Kconfig says so plainly if you read the build output:

```
warning: UART_CONSOLE (defined at boards/common/usb/Kconfig.cdc_acm_serial.defconfig:16,
drivers/console/Kconfig:42) was assigned the value 'n' but got the value 'y'.
```

The fix is to disable the *backend choice*, not the symbol it forces:

```
CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n
CONFIG_SERIAL=n
CONFIG_UART_CONSOLE=n
CONFIG_LOG_BACKEND_UART=n
```

`&uart0` is also disabled in the overlay so the pins are released rather than merely
unused, which genuinely frees D6/D7.

Worth noting the devicetree console had been pointing at `board_cdc_acm_uart` while
`CONFIG_USB_DEVICE_STACK` was never enabled, so UART console output had nowhere to
go in the first place. `CONFIG_RTT_CONSOLE=y` was already set.

**An implicit dependency came off with it.** That same CDC block sets
`CONFIG_LOG_PROCESS_THREAD_STARTUP_DELAY_MS=4000` inside `if LOG`, which is what had
been holding the log thread long enough for a debugger to attach before the boot
burst flushed. Losing it truncated the boot log -- `[PASS] freque---`, with the
self-test summary and register dump dropped. Set explicitly now.

Verified: zero `was assigned the value` warnings, **zero UARTE symbols in the
`.elf`** (the driver is gone, not merely idle), boot log complete with no
truncation, `OPMODE = 0x00` at idle, and over BLE the device still advertises as
`ClaudeLink` with the SMP/DFU service present and a live battery reading.

Flash and RAM both fell, which is a useful cross-check that the driver really went:

| | before | after |
|---|---|---|
| FLASH | 214704 B (48.55%) | **192412 B (43.51%)** |
| RAM | 62084 B (23.68%) | **54220 B (20.68%)** |

The power saving itself is still unmeasured -- see the note in 17.3 about needing a
current meter rather than more datasheet arithmetic.

## 18. A dead castellation, and three modules replaced for nothing

The RFM69 stopped answering: `REG_VERSION=0xFF` on every 5-second retry. Three
modules were swapped chasing it -- two 916 MHz and a 433 MHz unit borrowed purely
to test the SPI link, since all RFM69 variants share the SX1231 die and report
`0x24` regardless of band. **All three were fine.** The fault was the XIAO's D9
castellation, or its solder joint.

### 18.1 What the debugger could and could not prove

Everything here was measured over SWD with the core halted, which turned out to be
a good way to test a board with no working radio:

| Test | Result | What it ruled out |
|---|---|---|
| Drive each SPI pin high/low, read back | all follow | nRF pins healthy *at the die* |
| Pull-up / pull-down on MISO | follows both | nothing external driving it |
| Drive MOSI/SCK/NSS, watch MISO | no coupling | no solder bridges |
| NSS asserted low, MISO pull test | still floating | a powered, selected SX1231 **must** drive MISO -- it wasn't |
| Module rail, by meter | 3.3 V | module is powered |

**A pin test cannot reach the castellation or its solder joint.** That is the one
link in the chain the debugger cannot see, and it is where the fault was.

One false start worth recording: the first pin-integrity run reported three pins
"damaged". It was wrong -- `SPIM2` owned P1.13/14/15 through PSEL, so GPIO writes
had no effect, which is exactly why NSS (a plain `cs-gpios` pin) was the only one
that appeared to work. Writing 0 to the SPIM `ENABLE` register at `0x40023500`
releases the pins and the test then gives real answers.

### 18.2 The measurement that actually found it

Using the debugger as a signal source: halt the core, release the pins from SPIM,
and latch one pin high while holding the rest low. Then probe with a meter.

Driving D9 high measured **0.8 V, and later 1.7 V**, at the module end. A
*wandering* mid-rail value is the signature of a resistive path -- not a clean
connection (3.3 V) and not a clean break (0 V, or floating). Driving D9 low
reached 0 V cleanly, so the asymmetry pointed at a poor joint rather than a short.

The decisive step was checking the destination **before** committing to it: D7's
castellation measured a clean 3.3 V under identical conditions. Same driver, same
conditions, different pad -- which isolates the pad and its joint from everything
else.

### 18.3 The fix is a bodge, and is kept as one

MISO moved to D7 (P1.12), which was only free because removing the UART earlier
released D6/D7. With it, the self-test passes (`VERSION = 0x24`, `ALL PASSED`) and
the pump answers **5/5 model reads at -40 dBm reporting model '722'** -- the
strongest link yet measured, against -50..-54 dBm previously.

**This is not the design.** `boards/xiao_ble.overlay` still specifies D9, and the
default build produces `SPIM_MISO -> P1.14`. The workaround lives in
`boards/bodge-miso-d7.overlay` and applies only when explicitly added to
`EXTRA_DTC_OVERLAY_FILE`. Both were verified by decoding `spi2_default` out of the
generated `zephyr.dts` for each build.

To retire it: repair the D9 joint, build without the overlay, confirm the
self-test passes.

Caveat on the diagnosis: moving to D7 changed the pin, the wire *and* the joint at
once, so it does not strictly prove the castellation itself was bad rather than
that end of the old wire. If D9 is ever needed, re-solder it fresh and retest
rather than assuming the pad is dead.

### 18.4 Lesson

Three modules were replaced before anything was measured. The sequence that
actually worked was: prove the die is fine, prove the module is powered, prove
there is no bridge, then use the debugger as a signal generator and a meter as the
receiver to find where a known-good signal stops arriving. **The instrument to
reach for on "the peripheral does not answer" is a voltmeter and a latched pin,
not another part.**

## 19. Would it fit on a RAK4600? Measured, not estimated

The RAK4600 is **nRF52832 + SX1276**. That matters because the SX1276 *does*
support OOK -- `RF_OPMODE_MODULATIONTYPE_OOK` plus a dedicated OOK demodulator
(`REG_OOKPEAK`, `REG_OOKFIX`, `REG_OOKAVG`, OOK bit-sync) -- so unlike the SX1262
it can carry the Medtronic link at all. Watch the part numbers: **RAK4631 is
nRF52840 + SX1262**, more MCU but the wrong radio.

That leaves resources as the only question, so it was measured rather than
extrapolated.

### 19.1 Method

Zephyr has no RAK4600 board, so `nrf52dk/nrf52832` stands in: same die, same
512 KB / 64 KB budget, and its **default partition layout is already MCUboot
dual-slot** (48 K boot, 2 x 220 K slots, 24 K storage) -- no custom partition file
needed.

`boards/nrf52dk_nrf52832.overlay` supplies the devicetree nodes the application
references (`orangelink-led-{r,g,b}`, `vbat_enable`, `chg_current`, `zephyr,user`
io-channels, and the `rf69` node). **Pin choices there are arbitrary and nothing is
wired** -- they exist only so the code links and can be measured. It is a
feasibility artifact, not a proposed pinout.

### 19.2 Result: it fits, with OTA rollback intact

| variant | FLASH | of 220 K slot | RAM | of 64 K |
|---|---|---|---|---|
| as-is | 179104 B | **82.6%** | 51596 B | **78.7%** |
| buffers trimmed, DFU kept | 179104 B | 82.6% | 41632 B | **63.5%** |
| no SMP DFU | 166780 B | 76.9% | 38368 B | 58.5% |

RAM is the binding constraint, and roughly 10 KB of it is recoverable by config
alone: `MCUMGR_TRANSPORT_NETBUF_SIZE` 2475->1024, `NETBUF_COUNT` 4->2,
`SEGGER_RTT_BUFFER_SIZE_UP` 2048->512, `LOG_BUFFER_SIZE` 1024->512. Those affect
**DFU throughput and log fidelity only**. The settings that are load-bearing for
pump traffic were deliberately left alone and verified present in the trimmed
build: `BT_ATT_PREPARE_COUNT=12` (long writes from AndroidAPS),
`BT_L2CAP_TX_MTU=247`, `BT_BUF_ACL_RX_SIZE=251`.

So the realistic target is the middle row: **82.6% flash, 63.5% RAM, keeping
signed dual-slot OTA.**

### 19.3 Why there is more headroom than it looks

Our own code is a small fraction of the image:

```
aps                 3008 B      rf69 driver         5802 B
subg                4264 B      ips                 2148 B
main                2314 B      battery             1674 B
led                  837 B      4b6b + manchester    674 B
                                --------------------------
our application total          20721 B text, 3013 B bss
```

**20.2 KB of a 175 KB image.** The rest is Zephyr, the BLE controller and host,
mcumgr and libc -- none of which grows as features are added. Adding application
features is cheap here; the fixed cost is what nearly fills the part.

### 19.4 Caveats

* **The SX1276 driver does not exist.** These figures include the SX1231/RFM69
  driver, which is 5802 B. A replacement is a rewrite (different register map,
  FIFO semantics and DIO mapping), but even doubling that figure is +6 KB against
  ~37 KB of flash headroom, so it is not a fit risk. `sx1276Regs-Fsk.h` is already
  in-tree, which helps.
* **The DK is not the module** -- resolved against the datasheet in 19.5.
* nRF52832 has no USB, which costs nothing here since the UART and USB console
  were already removed.

### 19.5 Checked against the RAK4600 datasheet

Two of the three open questions are now settled, one favourably, and a new
requirement and a new constraint appeared.

**DIO wiring: resolved, and it is fine.** DIO0-DIO4 are all routed to the MCU:

| SX1276 | nRF52832 |
|---|---|
| SCK / MOSI / MISO / NSS | P0.07 / P0.05 / P0.06 / P0.04 |
| DIO0 | P0.27 |
| DIO1 | P0.28 |
| DIO2 | P0.29 |
| DIO3 | P0.30 |
| DIO4 | P0.31 |
| DIO5 | **NC** |

Only DIO5 is unconnected, and nothing here needs it. The design's FifoNotEmpty-
style interrupt has several candidate mappings available, so receive does **not**
degrade to SPI polling. This was the one unknown that could have forced a design
change, and it does not.

**Band: fine.** The module datasheet gives 863-870 MHz (EU) / **902-928 MHz (US)**,
and the US part covers 916.5 MHz.

**NEW REQUIREMENT -- the RF switch.** The module has an antenna switch driven by
`VCTL1` on **P0.16** and `VCTL2` on **P0.15**. The RFM69 has no such thing, so this
is new driver work with no analogue in the current code: the TX/RX path must be
switched with the radio mode. It is small, but getting it wrong means either no
transmit or no receive, with the SPI link looking perfectly healthy throughout.

**NEW CONSTRAINT -- almost no analog pin left.** Of the eight SAADC inputs, seven
are consumed by the radio:

```
AIN0  P0.02   not exposed on the module
AIN1  P0.03   ** the only exposed analog-capable pin **
AIN2  P0.04   SX1276 NSS
AIN3  P0.05   SX1276 MOSI
AIN4  P0.28   SX1276 DIO1
AIN5  P0.29   SX1276 DIO2
AIN6  P0.30   SX1276 DIO3
AIN7  P0.31   SX1276 DIO4
```

P0.03 is also the configured MCU reset pin. So battery sensing is possible, but it
costs the hardware reset (reset is re-assignable via `UICR.PSELRESET`) -- or the
battery monitor goes. Digital pins are not the problem; P0.18, P0.19, P0.12, P0.13,
P0.09, P0.10, P0.14, P0.17, P0.22 and P0.23 are available for the LED and the rest.

### 19.6 The one thing that decides it

**Which nRF52832 variant is fitted.** Neither the module nor the breakout datasheet
says, and it is the difference between comfortable and impossible:

| variant | flash / RAM | verdict |
|---|---|---|
| **QFAA** | 512 KB / 64 KB | fits as measured in 19.2 -- 82.6% flash, 63.5% RAM trimmed, OTA intact |
| **QFAB** | 256 KB / 32 KB | **does not fit** |

For QFAB the RAM settles it on its own: measured usage is 50.4 KB as-is and 40.7 KB
after trimming, against 32 KB available. Closing an 8.7 KB gap would mean cutting
`BT_ATT_PREPARE_COUNT` and the ATT MTU, which are exactly the settings the
AndroidAPS long-write path depends on. Flash is no better -- 175 KB against 92 KB
per slot in a dual-slot layout, and 95% of a single 184 KB slot with OTA given up.

**Confirm QFAA before buying anything.** Everything else here is tractable work;
this one is binary.

### 19.7 Battery sensing without an analog pin

19.5 left battery measurement as the awkward part: only AIN1 (P0.03) is both
exposed and analog-capable, and it is the configured MCU reset pin. Three ways out,
one of which removes the constraint entirely.

**A. Use AIN1 and give up hardware reset.** Cheapest in parts and code -- change the
channel and the divider ratio and the existing module works unchanged.
`UICR.PSELRESET` is re-assignable, and reset is still available over SWD, from
software, and implicitly through the BLE DFU path. Note P0.03 would need an
*external* divider: a 4.2 V cell exceeds the pin's absolute maximum, and unlike the
XIAO there is no onboard one.

**B. I2C fuel gauge -- recommended.** P0.12/P0.13 are exposed and otherwise idle, so
this sidesteps the analog constraint completely and keeps the reset pin. Zephyr
already ships the subsystem and suitable 1S LiPo parts (`max17048`, `lc709203f`).
Measured cost of adding `CONFIG_FUEL_GAUGE` + MAX17048:

| | FLASH | of slot | RAM |
|---|---|---|---|
| baseline | 179104 B | 82.6% | 51596 B |
| + MAX17048 | 182644 B | 84.2% | 51724 B |
| **cost** | **+3540 B** | +1.6pp | **+128 B** |

Affordable, and some of it comes back by deleting the ADC path -- the divider
arithmetic, the `lipo_curve` table and the calibration logging (`battery.c` is
1674 B today). It is also *better data*: a real state-of-charge algorithm instead of
the open-circuit voltage curve this port uses, which its own comments concede "is
not a fuel gauge" and which reads low under a transmit burst. The cost is one more
IC on the board.

**C. Measure VDD with no pin at all.** `NRF_SAADC_VDD` exists, and the SAADC can
sample the supply rail directly. But the module is specified 2.0-3.6 V while a 1S
LiPo is 3.0-4.2 V, so a regulator is required -- and then VDD is constant and says
nothing about the cell until it reaches dropout. Only worth considering if the
chemistry changes to something that stays under 3.6 V (2xAA, or a 3 V coin cell),
where it becomes free and pin-less.

Note that neither datasheet mentions a charger, so charging hardware is a
board-level problem on the RAK4600 regardless of which option is chosen.

## 20. Which Semtech parts can carry this link

The requirement is narrow and unforgiving: **OOK, at ~916 MHz**. Medtronic pumps use
on-off keying, and this port depends on it
(`RF_DATAMODUL_MODULATIONTYPE_OOK` in the 916 config table).

OOK support below was checked against the Semtech reference drivers in-tree; the
frequency ranges are datasheet figures and worth confirming per part.

### Will work

| Part | Modulation | Range | Notes |
|---|---|---|---|
| **SX1231 / SX1231H** | FSK/OOK | 290-1020 MHz | what this project uses -- RFM69W / RFM69HCW |
| SX1232 / SX1233 | FSK/OOK | 290-1020 MHz | same family, harder to source |
| SX1238 | FSK/OOK | 290-1020 MHz | harder to source |
| **SX1272 / SX1273** | LoRa + FSK/OOK | 860-1020 MHz | OOK confirmed in-tree |
| **SX1276 / SX1277 / SX1279** | LoRa + FSK/OOK | 137-1020 MHz | OOK confirmed in-tree -- RFM95W |

### Will not work

| Part | Why |
|---|---|
| SX1261 / SX1262 / SX1268 | GFSK + LoRa only. `PACKET_TYPE_GFSK`/`PACKET_TYPE_LORA`, no OOK |
| LR1110 / LR1120 | no OOK |
| SX1280 / SX1281 | 2.4 GHz -- wrong band entirely |
| SX1211 / SX1212 | 300-510 MHz -- does not reach 916 |
| **SX1278** | **trap:** same die family as SX1276 but the low-band part (137-525 MHz). RFM98 is SX1278 and is 433-only |

### Two traps worth naming

**Band-specific matching.** The die being wideband does not make the *module*
wideband. This was demonstrated directly in section 18: a 433 MHz RFM69 borrowed to
test the SPI link reported `VERSION = 0x24` and passed every digital self-test,
while being useless on air at 916. Pick the 868/915 module variant, not just the
right die.

**SX1276 vs SX1278.** Identical family, and the part numbers differ by one digit,
but SX1278 tops out around 525 MHz. For 916 MHz the module to look for is **RFM95W**.

### Effort, if moving to SX127x

Less than it looks. Comparing register namespaces:

```
SX1231 (ours)          84 registers
SX1276 FSK/OOK         77 registers
shared names           54
```

`AFCBW, BITRATEMSB/LSB, DIOMAPPING1/2, FIFO, FIFOTHRESH, FRFMSB/MID/LSB,
IRQFLAGS1/2, LNA, OCP, ...` all carry over. Of the 30 SX1231-only registers, 16 are
`AESKEY1..16` and the rest are mostly `AUTOMODES` -- none of which this project
uses. So an SX127x port is closer to a translation of `rf69.c` than a design from
scratch, though the OOK demodulator has real controls the SX1231 lacks
(`REG_OOKPEAK`, `REG_OOKFIX`, `REG_OOKAVG`, OOK bit-sync) which need tuning against
a pump rather than assuming.

## 21. Where the power actually goes: measured, and a PA theory that died

Everything in this section replaces estimates with measurements. Three independent
instruments were used because the first one lied.

### What AndroidAPS actually asks for

Two AAPS log captures (33 and 38 minutes of a live session) were decoded for RFSpy
traffic. Requested listen timeouts are not what the radio does -- `subg_get_pkt()`
returns the moment a packet arrives -- so both the requested and the real durations
were measured by timing each command to its response:

| timeout | count | requested | actual | mean |
|---------|------:|----------:|-------:|-----:|
| 1.25 s  |   169 |     211 s |  116 s | 0.69 s |
| 2.00 s  |    14 |      28 s |    3 s | 0.25 s |
| 4.00 s  |   115 |     460 s |  162 s | 1.41 s |
| 25.00 s |    84 |    2100 s |  727 s | 8.65 s |
|         |       |  **2799 s** | **1008 s** | |

Requested timeouts imply 99% duty. Reality is **43.8%** (47.0% in the other
capture). Sizing anything off the requested figures overestimates by 2.8x.

Byte 3 of each command is the repeat count, and it is 200 on exactly the 84
commands carrying the 25 s timeout -- the wake sequence. At 15.6 ms per frame that
is 3.14 s of continuous airtime per wake, and 259 s of transmit across the session,
leaving 749 s of genuine receive.

### The currents

Measured with a multimeter in series with the battery, USB disconnected (Q3
disconnects the cell whenever USB is present, so a battery-side measurement reads
zero with the cable in):

| | measured | datasheet |
|---|---------:|----------:|
| idle (BLE connected, radio asleep) | < 150 uA | -- |
| receive | **17 mA** | 16 mA + MCU |
| transmit @ +13 dBm, PA1+PA2 | **33 mA** | -- |
| transmit @ +13 dBm, PA1 only | **31 mA** | -- |

Receive matches the datasheet to within a milliamp, which is what validates the
method. Transmit does not, and the reason is structural: **`IDDT` assumes a
continuous carrier.** OOK keys the PA off for every zero bit, and this link is
exactly 50% ones -- every one of the 16 4b6b codewords in `encode_4b[]` has three
ones in six bits, the preamble is 0xAA, and the sync is FF 00 FF 00. So:

    continuous carrier @ +13 dBm    45 mA   (Table 4, PA0)
    synthesiser alone (IDDFS)        9 mA   (runs continuously)
      -> PA contribution            36 mA
    OOK average = 9 + 0.5 x 36    = 27 mA
    + nRF52832 with BLE up         ~2 mA   -> ~29 mA predicted, 31-33 measured

**A USB power meter was tried first and was worthless** -- no resolution at idle,
and it reported the PA comparison backwards (23 mA vs 25-27 mA, favouring the wrong
configuration). Every conclusion drawn from it was wrong. It is recorded here only
so the mistake is not repeated.

### The PA experiment, and why it failed

Fitting the datasheet's two PA_BOOST figures (+17 dBm/95 mA, +20 dBm/130 mA) gives
~43% marginal efficiency over ~60 mA of fixed bias, predicting that +13 dBm costs
74 mA on PA1+PA2 against ~45 mA on PA1 alone -- a 40% saving for a one-line change.

It was built, flashed and measured. **PA1 alone is better by 6%, not 40%** -- 31 mA
against 33 mA, worth about 0.2 days out of 8, while forfeiting all headroom above
+13 dBm. Link quality was identical (12/12 pump replies both ways). Default stays
PA1+PA2.

The fit was invalid because +20 dBm requires the high-power TESTPA registers, so the
two points are in different operating modes and do not lie on one curve. And the
whole premise was weaker than it looked: **OOK halves whatever PA difference
exists**, because the PA is only energised half the time. PA-level optimisation is
inherently worth half here what it would be on an FSK link.

The Kconfig was restructured anyway: power is now expressed in dBm
(`ORANGELINK_RFM69_TX_DBM`) with an explicit PA stage choice, so switching stage
cannot silently change what leaves the antenna -- which the old raw-OutputPower
field made easy, since the two stages use different power formulas.

### Where the energy goes

    RX    749 s x 17 mA  = 12,733 mA*s    60%
    TX    259 s x 33 mA  =  8,547 mA*s    38%
    idle 2301 s x 0.15   =     345 mA*s     2%
                           ------------
                           21,625 mA*s / 2301 s = 9.4 mA  ->  ~8.0 days on 1800 mAh

**Receive dominates.** An earlier draft of this section claimed the opposite, on the
strength of the 74 mA transmit figure; that was wrong. The lever is receive duty
cycling, not the PA.

### The pump's reply preamble, measured with a HackRF

`subg_get_pkt()` holds the receiver on continuously for a window whose length AAPS
chooses. The RFM69's hardware Listen Mode (`RegListen1/2/3`) can duty-cycle the
receiver inside that window invisibly to AAPS -- provided the idle half is shorter
than the incoming preamble.

Our preamble is 16 bytes (`RF_PREAMBLESIZE = 0x0010`). **The pump's is not
documented anywhere**, and structurally so: every open implementation sync-word
hunts rather than requiring a preamble. `ps2/subg_rfspy` sets `PKTCTRL1 = 0x00`, so
the CC1101's preamble quality threshold is zero; `ps2/rtlmm` squelches then shifts
bits against `0xff00ff00`. Nobody had to characterise it, so nobody did. Our own
hardware cannot see it either -- with `RF_SYNC_ON | RF_SYNC_FIFOFILL_AUTO` the
preamble is consumed by bit sync and never reaches the FIFO.

So it was captured directly: HackRF One at 916.1 MHz (500 kHz low, to keep the
signal off DC), 2 Msps, during a live pump exchange. Demodulated as OOK with the
bit rate taken as known (16384 bps) and only phase recovered -- estimating the rate
quantises the symbol period to whole samples, drifts ~2 bits per frame and destroys
the sync search.

| | preamble | duration |
|---|---------:|---------:|
| our transmission | 128 bits / 16.0 bytes | 7.81 ms |
| pump's reply | >= 91 bits / ~11.4 bytes | ~5.55 ms |

Our figure coming out at exactly 128 bits is what validates the demodulator. The
pump's 91 is a **lower bound** -- the count walks back from the sync word and stops
at the first non-alternating pair, so one noisy bit at the weak leading edge
truncates it; the true value is probably 96 bits (12 bytes, 5.86 ms).

All four replies were identical and each arrived a consistent **+73.4 ms** after the
transmission, which suggests the receiver could stay asleep for the first ~70 ms of
every listen window.

### What Listen Mode would be worth

Against a ~5.5 ms preamble, 4 ms idle + 1 ms receive is a 20% duty cycle, taking
receive from 16 mA to ~3.2 mA effective:

| | average | battery |
|---|--------:|--------:|
| today | 9.4 mA | 8.0 days |
| with Listen Mode | 5.2 mA | **~14 days** |
| EFR32FG28 migration | 4.7 mA | ~16 days |

**~1.8x for a register block, against ~2x for a full platform migration.** Note that
RSSI-threshold triggering is unreliable with OOK; Listen Mode should trigger on
SyncAddressMatch, which requires the receive window to span preamble plus the 4-byte
sync.

### Open items from this work

- **Listen Mode is implemented but not validated on the bench.** It is opt-in
  (`CONFIG_ORANGELINK_RFM69_LISTEN`, default n) because the idle period is set
  against a preamble measured from one pump on one bench. `ListenEnd` is 00, not
  the more obvious 01/10, because those end the receive on PayloadReady and this
  driver runs fixed-length 255 with CRC off, so PayloadReady never fires -- packets
  terminate on a zero byte in `subg_get_pkt()` instead. Acceptance is RSSI-only at
  a threshold of 180 (-90 dBm) rather than the normal 228 (-114 dBm), which would
  wake on noise every cycle; requiring SyncAddressMatch would mean holding the
  receiver on across preamble plus sync, which is the cost being avoided. Defaults
  are 3264 us idle / 1472 us receive = 31% duty, taking receive from 16 mA to ~5 mA
  and the projection from 8.0 to ~12.9 days.

  **Measured on the bench, and it holds.** Receive current fell from 17 mA to
  **5.5 mA** (multimeter in series with the battery, back-to-back `CMD_GET_PKT`
  with no transmit), against ~6 mA predicted. Reply rate stayed 12/12 with every
  reply a full model response. Reworking the energy budget with the measured
  figure gives 5.65 mA average and **~13.3 days, a 1.66x gain**.

  Worth being clear about the failure mode, because it is better than it first
  looks: a false RSSI trigger cannot corrupt a receive. `RF_SYNC_ON |
  RF_SYNC_FIFOFILL_AUTO` means the FIFO only fills after a sync match, so noise
  above the threshold merely leaves the part in RX for the rest of that window
  (`ListenEnd = 00` having stopped Listen mode). In a noisy environment the
  saving degrades toward continuous RX; the packet still arrives.

  **Still default n**, for one reason only: the idle period is sized against a
  preamble measured from one pump on one bench. Sweep `LISTEN_IDLE_US` upward
  until replies start dropping to find the real margin before trusting it on any
  other hardware.
- **BLE drops during sustained wake bursts.** Seen in most soak runs. This would
  appear to AAPS as exactly the stalled commands visible in the captured logs.
- **Battery percentage is fiction when no cell is fitted.** With the cell removed the
  sense node is pulled to ground through R6 (2 MOhm) and reads 0%, but while the
  MCP73831 is still driving VBAT it read a confident 73%. Neither is a measurement.
  `battery.c` should detect the absence of a cell rather than report a number.
- **`rf69_set_power_level()` is effectively dead.** Both calls are in the boot
  self-test and the restore is immediately overridden by `rf69_config_916()`.
- **`cmd_update_reg` cannot reach RFM69 registers.** Only 0x02/0x09/0x0A/0x0B/0x0C
  are handled, so Listen Mode cannot be swept over BLE without a firmware change. A
  debug-only raw register command behind a Kconfig would make that experiment cheap.
