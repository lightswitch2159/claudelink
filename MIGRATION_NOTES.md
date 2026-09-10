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
