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
