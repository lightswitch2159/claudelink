# Migration Notes

Every intended or discovered deviation from the original firmware's behaviour.
Living document — append as porting proceeds.

**Phase 0 status: no code ported, so no behavioural deviations exist yet.**
Everything below is either a *planned* deviation (with rationale) or a
*discrepancy found* between the migration brief, the scoping document, and what
the source actually does.

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

The scoping document treats `CONFIG_KERNEL=n` as an NCS Kconfig that might reduce
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

## 2. Planned deviations from original behaviour

Deliberate changes. Each needs sign-off before Phase 4 completes.

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
| Trailing single zero byte stripped for Minimed, **kept** for Omnipod | Get it wrong and Omnipod packets corrupt silently. |
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
modules -- `RF69_DEV_FREQ433` (Omnipod) and `RF69_DEV_FREQ916N868` (Minimed) --
each with its own chip select. Only one is fitted now.

Not ported, deliberately:

- the 433 MHz (`freq433CfgTbl`) and 868 MHz (`freq868CfgTbl`) register tables
- the second radio instance, its chip select and its DIO0 line
- Omnipod-specific paths in `app_subg.c` (`omnipod_rx`, the 80-byte packet mode)
- `SUBG_MODE_OMNIPOD` and `SUBG_MODE_MINIMED_WWL`

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
the legacy driver **never set `PayloadLength` for TX**. So every transmission sent
a full 255-byte frame, padding the tail with whatever the underrunning FIFO
produced.

The legacy firmware hid this: `wait_tx_done()` only waited for the FIFO to *drain*,
and `rf_stop()` then forced SLEEP, truncating the transmission mid-packet. That
works against a receiver which stops at the zero terminator, but it wastes airtime,
radiates padding, and makes the send-and-listen turnaround far slower than
necessary.

`minimed_tx()` now sets `PayloadLength` to the real frame size (data + terminator).
Measured **136 ms -> 13 ms**, a 10x reduction in airtime and turnaround.

> **DEVIATION to validate against the 722.** The pump has only ever seen the
> truncated-255 behaviour. If it turns out to depend on that tail, revert to
> drain-then-STANDBY. This is the first thing to check when real pump testing
> starts.

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
