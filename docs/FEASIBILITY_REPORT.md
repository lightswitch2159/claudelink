# Phase 0 Feasibility Report — Orangelink-Firmware → nRF Connect SDK

**Date:** 2026-09-10
**Scope:** Phase 0 only (feasibility gate). No porting work performed.
**Status:** **GATE NOT PASSED as specified.** Two of the brief's premises are false.

---

## Verdict

| Question | Answer |
|---|---|
| Can NCS v3.4.0 + BLE + MCUboot fit on nRF52810? | **No.** Not with dual-slot OTA, not with single-slot. |
| Is the NCS bare-metal option a smaller-footprint alternative? | **No — it does not support nRF52 at all.** nRF54L Series only. |
| Recommended chip | **nRF52832** for the stated goal — but see the nRF54L question below before committing. |
| Recommended architecture | **Zephyr RTOS.** There is no bare-metal NCS option for this silicon family. |
| Can Phase 1 start now? | **No.** Three blockers must clear first: hardware decision, legal review, security disclosure. |

The migration is technically achievable, but **not on the current hardware**, and
the plan needs restructuring around that fact before any code is written.

---

## 1. What was verified, and what was not

Per your decision, the proof-of-fit build was **not** performed — the environment
had no `west`, no ARM toolchain and no Zephyr SDK, and installing them (~6–10 GB)
was judged not worth it given the conclusion was already determined by published
figures plus measured legacy data.

| Phase 0 task | Status | Evidence |
|---|---|---|
| 1. Fork, remove `nrfSDK/` and private keys | **Done** | 4 private keys deleted; 175 MB → 4.9 MB |
| 2. NCS workspace via `west init`/`west update` | **Not done** | Toolchain not installed (your call). `west.yml` provided so Phase 1 can run it |
| 3. Proof-of-fit build, nRF52810 | **Not done — inferred** | Nordic's published figures + measured legacy budget. **The one unverified conclusion.** |
| 4. Verify bare-metal supports BLE on nRF52810 | **Done — answered NO** | Nordic product docs; see §3 |
| 5. Generate new MCUboot ECDSA P-256 keys | **Done** | Generated, functionally validated, distinct from legacy |
| 6. Extract real flash usage from Keil artifacts | **Done** | [`legacy-flash-budget.md`](legacy-flash-budget.md) |
| 7. Extract `NRF_SDH_BLE_GATT_MAX_MTU_SIZE` | **Done — 247** | [`gatt-service-spec.md`](gatt-service-spec.md) §1 |
| 8. Document APS frames + config storage | **Done** | [`aps-protocol-spec.md`](aps-protocol-spec.md), [`config-storage-spec.md`](config-storage-spec.md) |
| 9. Feasibility report | **This document** | |

### Confidence in the nRF52810 verdict

High, on three independent lines of evidence:

1. **Nordic staff, on the exact question.** For nRF52810 under NCS: minimal BLE
   peripheral = **132.8 KB flash / 16.8 KB RAM**; the *standard* config consumes
   **97.95% of SRAM** at baseline; adding `CONFIG_BOOTLOADER_MCUBOOT` **overflows
   flash by ~64 KB**. Nordic's own recommendation in that thread is serial-recovery
   DFU, external flash, or staying on the nRF5 SDK — not "optimise harder."
2. **Measured legacy budget.** The entire working product today — MBR + SoftDevice
   + application — is **131.4 KB**. The NCS platform baseline *alone*, with no
   application, is 132.8 KB. The platform costs more than the whole current
   product.
3. **Only 22 KB of the 192 KB part is uncommitted today**, and the bootloader slot
   is already 94.8% full.

A local build would change the decimal places, not the outcome. If you want it
verified first-party anyway, the right time is as the opening task of Phase 1 on
the *chosen* chip.

---

## 2. Flash and RAM analysis

### Measured today (nRF52810, 192 KB / 24 KB)

Full detail in [`legacy-flash-budget.md`](legacy-flash-budget.md). Summary:

| Region | Size | Used | Free |
|---|---|---|---|
| MBR | 4 KB | 4 KB | — |
| SoftDevice S112 v6.1.1 | 96 KB | 96 KB | — |
| Application | 52 KB | **31.4 KB** | 20.6 KB |
| FDS config | 8 KB | 8 KB | — |
| Bootloader (Secure DFU) | 24 KB | **22.75 KB** | 1.25 KB |
| MBR params + boot settings | 8 KB | 8 KB | — |
| **Flash total** | **192 KB** | **~170 KB** | **~22 KB** |
| **RAM total** | **24 KB** | 11.0 KB SD + 4.9 KB app | **8.4 KB** |

> **The scoping document's flash map is wrong.** It states the SoftDevice is ~25 KB
> and the application region ~124 KB. Measured: S112 is **96 KB** and the
> application region is **52 KB**. The document overstates available application
> space by ~72 KB. Any estimate built on it is invalid — this correction is the
> single most consequential finding of Phase 0.

### Projected under NCS, nRF52810 — does not fit

| Component | Flash | RAM |
|---|---|---|
| MCUboot, single-slot, ECDSA P-256 | 24–32 KB | (boot only) |
| NCS minimal BLE peripheral (kernel + host + SoftDevice Controller) | ~133 KB | 16.8 KB |
| Orangelink application logic | 31–45 KB | 5–8 KB |
| Settings / NVS (3 sectors minimum) | 12 KB | ~1 KB |
| **Required** | **200–222 KB** | **23–26 KB** |
| **Available** | **192 KB** | **24 KB** |
| **Result** | **over by 8–30 KB** | **zero to negative margin** |

That is the *single-slot* case, which sacrifices safe rollback. Dual-slot OTA needs
a further ~165 KB and is not close. Nordic's "~64 KB over" figure reflects the
default dual-slot, non-minimal configuration; both estimates land on
**infeasible**.

RAM is the harder wall than flash. At 16.8 KB baseline there is ~7 KB left, and the
application needs 107-byte packet buffers, two command FIFOs, a 132-byte queue
element, plus Zephyr's per-thread stacks (main, BT RX, system workqueue, ISR) —
each of which costs 512 B–2 KB. Trimming to fit would mean removing the RTOS
services that are the reason to adopt an RTOS.

### Projected under NCS, nRF52832 (512 KB / 64 KB) — fits with margin

Example partition layout:

| Partition | Range | Size |
|---|---|---|
| MCUboot | `0x00000`–`0x0C000` | 48 KB |
| Primary slot | `0x0C000`–`0x3E000` | 200 KB |
| Secondary slot | `0x3E000`–`0x70000` | 200 KB |
| Settings (NVS) | `0x70000`–`0x78000` | 32 KB |
| Reserved | `0x78000`–`0x80000` | 32 KB |

Application image ≈ 178 KB in a 200 KB slot → **~11% headroom**, with 32 KB
reserved to reallocate. RAM ≈ 25 KB of 64 KB → **~60% free**. Dual-slot OTA with
safe rollback works. This is a comfortable target, not a marginal one.

---

## 3. Bare-metal: the option does not exist for this silicon

**Blocker 2 from the brief is resolved as unavailable.**

The scoping document treats `CONFIG_KERNEL=n` as a Kconfig switch within NCS that
might yield a smaller footprint. It is not:

- **nRF Connect SDK Bare Metal is a separate SDK** — a distinct repository
  (`ncs-bm` / `nrf-bm`), currently at version 2.0.99, not a configuration of NCS.
- **Supported devices are nRF54L Series only**: nRF54L15, nRF54L10, nRF54L05,
  nRF54LM20, nRF54LS05, nRF54LV10. No nRF52 part appears in the supported hardware.
- Nordic's product page states it plainly: the Bare Metal option supports
  Bluetooth LE development **on the nRF54L Series**.

So there is no smaller-footprint NCS variant for nRF52810 or nRF52832.
**Zephyr RTOS is the only NCS architecture available for nRF52.** Remove the
bare-metal branch from the plan.

This does not mean the timing risk is unmanageable — it means it must be managed
inside Zephyr. See §5.

---

## 4. Blockers

### B1 — Hardware decision (blocking all porting work)

nRF52810 cannot host this port. A chip decision is required before Phase 1.

**Possibly cheaper than the scoping document assumes:** the nRF52832 is available
in the same **QFN48 6×6 mm** package as the nRF52810, and the pins this design
uses (P0.04/05/06/12/15/16/18/20/28/30) are compatible across both. If the board
carries the QFN48 part, this may be a **part substitution plus a power/decoupling
review**, not a redesign. That must be confirmed against the schematic and BOM,
**neither of which is in this repository** — request them before costing the
change.

### B2 — Legal review (blocking publication of the fork)

All 44 application source files carry:

> "This program is free software; you can redistribute it and/or modify it under
> the terms of the GNU General Public License version 2 as published by the Free
> Software Foundation."

That is GPL v2 **only**, not "v2 or later". NCS and Zephyr are Apache 2.0. GPL v2
and Apache 2.0 are generally understood to be incompatible for combined works,
which puts distributing a combined firmware image in question. There is no
top-level `LICENSE` file.

**I am not qualified to give a legal opinion and this is not one.** Route to
counsel. Practical avenues to put in front of them:

- Ask the copyright holder (Fractal Auto Technology Co., Ltd. / Ribin Huang) to
  relicense as GPL v2-**or-later** or Apache 2.0
- Request a linking exception
- Establish whether the GPL v2 headers were intentional or template boilerplate —
  the repository has one commit and a one-line README, so the licensing may never
  have been considered deliberately

This blocks *distribution*, not private development. Internal bench work can
proceed in parallel; publishing cannot.

### B3 — Security disclosure (should precede publishing the fork)

Phase 0 found **three remotely reachable stack buffer overflows in the currently
shipping firmware**, all the same unchecked-`memcpy` pattern:

| Function | File | Destination | Max write | Overflow |
|---|---|---|---|---|
| `Aps_PutCmd()` | `app_aps.c:686` | 123 B | 148 B | ~25 B |
| `Cfg_PutReq()` | `app_config.c:253` | 2 B | 242 B | ~234 B |
| `Fct_PutReq()` | `app_factory.c:434` | 20 B | 242 B | ~222 B |

Each computes a length from the BLE frame and copies without bounding it against
the destination. All IPS and NUS characteristics are `SEC_OPEN`; the device
requires **no pairing, bonding or encryption**, and `Cfg_StartLoop()` arms the
second path on every `BLE_GAP_EVT_CONNECTED`. Any device in radio range that can
connect can reach all three.

Details and fixes: [`aps-protocol-spec.md`](aps-protocol-spec.md) §7,
[`config-storage-spec.md`](config-storage-spec.md) §4.

This affects **deployed devices today**, independent of the migration. Recommended
sequence:

1. Notify the upstream maintainer privately before this fork is made public.
2. Decide whether to ship a bounds-check patch **on the existing nRF5 SDK build**.
   That is a handful of lines and needs no migration — it is available now, and
   the migration is months away.
3. Fix in the port, with `ztest` cases feeding maximum-length and malformed frames
   to each parser.
4. Independent review, per the safety notice.

I have not written or attempted any exploit, and recommend nobody does outside a
controlled bench environment.

### B4 — No OTA path for fielded devices (confirmed, not newly discovered)

The flash layout changes completely and MCUboot cannot read Nordic Secure DFU
packages. There is no supported OTA route from the current firmware to an NCS
image. Compounding it: the existing bootloader slot is **94.8% full**, so a
migration shim cannot be added to the current bootloader in place.

With a chip change (B1) this becomes academic — new silicon means new hardware
means SWD programming at manufacture. **Decide B1 first; if the chip changes,
in-field OTA migration is off the table by construction** and should be dropped
from scope rather than designed around.

---

## 5. Architecture recommendation

**Zephyr RTOS** — not a preference, the only option (§3).

The scoping document's top risk ("sub-GHz timing breaks under Zephyr RTOS", High /
Critical) is real, and Phase 0 sharpened it into something specific rather than
vague. Two concrete problems, both in [`aps-protocol-spec.md`](aps-protocol-spec.md) §6:

1. **`Subg_GetPkt()` blocks for the full client-supplied `listenTimeout`**, via
   unbounded `while (!Rf69_IsFifoFull(...))` polling and `Kit_DelayMs`.
   `CMD_SEND_AND_LISTEN` multiplies it by `retryCnt + 1`. A single command can hold
   the CPU for **seconds**. Under the legacy super-loop this sits below the
   SoftDevice's radio ISRs and BLE survives. Under Zephyr the SoftDevice Controller
   still preempts, but a multi-second busy-wait in a `k_timer` handler or the system
   workqueue **starves the Bluetooth host thread** — the link stays up while the
   device stops answering ATT.

2. **`CMD_UPDATE_REG` executes synchronously in the BLE callback** while the APS
   timer loop may be mid-SPI-transaction on the same RFM69. There is no lock. Today
   the legacy priority arrangement hides it; under Zephyr these are genuinely
   concurrent threads and it becomes a real SPI data race.

Mitigation, to be designed in Phase 1 rather than discovered in Phase 4:

- A **dedicated cooperative sub-GHz thread**, priority below the BT RX thread
- **RFM69 DIO lines as GPIO interrupts with `k_sem` handoff**, replacing FIFO polling
- `k_sleep` / `k_busy_wait` chosen deliberately per call site, not a blanket substitution
- **One `k_mutex` owning all RFM69 access**, with `CMD_UPDATE_REG` routed through the
  same queue as everything else
- Logic-analyser comparison against legacy timing before trusting any of it

Two findings *reduce* scope:

- **The custom heap allocator is dead code.** `kit_heap` appears **zero** times in
  `app.map`; the C library heap is discarded too. The application performs no
  dynamic allocation. Migration task 23 is a deletion, and
  `CONFIG_HEAP_MEM_POOL_SIZE` stays 0.
- **FDS → Settings needs no migration tooling.** The chip change forces an
  erase-and-reflash, so there is no scenario where a legacy FDS record must be read
  by NCS firmware. Accept the config reset (a custom device name and two toggles),
  document it, and skip the tool.

---

## 6. The question the brief does not ask

**If the hardware has to change anyway, the real choice is not nRF52810 vs
nRF52832. It is nRF52832 vs nRF54L.**

| | nRF52832 + NCS/Zephyr | nRF54L + NCS Bare Metal |
|---|---|---|
| Fits | Yes, comfortably | Yes |
| Architecture | Zephyr RTOS — full rewrite of every integration surface | SoftDevice-style, **deliberately modelled on the nRF5 SDK** |
| Timing risk for sub-GHz | **Real** — §5 mitigations required | **Largely avoided** — no RTOS scheduler in the path |
| nRF52 status in NCS | **Feature complete** — v3.4.0 is the last release adding features | Current platform, forward-looking |
| Support horizon | 5-year LTS bug/security fixes, no new features | Full ongoing development |
| Hardware change | Possibly a QFN48 part swap (verify) | New silicon, layout, RF re-qualification |
| Regulatory | Modest — same radio family | Larger — likely re-certification |
| Porting effort | 10–16 weeks (scoping estimate, now better founded) | Plausibly **less** application-layer work; more hardware work |

Nordic built the Bare Metal option specifically to ease migration from the nRF5
SDK to nRF54L, with "architecture and API similarities to the nRF5 SDK." This
codebase *is* an nRF5 SDK / SoftDevice application with a super-loop and no
dynamic allocation. It may be a closer fit to nRF54L Bare Metal than to Zephyr —
and it would sidestep the single largest technical risk in the whole project.

This is not a recommendation to switch. It is a recommendation to **make the
comparison explicitly before spending 10–16 weeks**, because the nRF52 line is
feature-complete and the hardware is being opened up regardless. The BOM, RF
re-certification and schedule inputs needed to decide are outside this repository.

---

## 7. Recommendations

**Do not start Phase 1 yet.** In order:

1. **Resolve B1 (chip).** Obtain the schematic and BOM. Confirm the nRF52810
   package. Cost the nRF52832 swap. Run the §6 comparison. *Owner: hardware +
   product. Blocking everything.*
2. **Open B2 (legal) in parallel** — it has the longest lead time and gates
   publication, not development.
3. **Act on B3 (security) now.** It is independent of the migration, affects
   deployed devices, and the fix is a few lines. Notify upstream; consider a patch
   release on the existing nRF5 SDK build.
4. **Drop in-field OTA migration from scope** once B1 confirms a chip change (B4).
5. **Then** re-run Phase 0 tasks 2 and 3 as the first Phase 1 task on the chosen
   chip: `west init`, `west update`, and a real proof-of-fit build with BLE + SPI +
   MCUboot. Treat a first-party number as the actual gate.
6. **Revise the effort estimate.** The 10–16 week figure was built on a flash map
   that was wrong by 72 KB and on a bare-metal option that does not exist. Re-plan
   after B1.

### Also revise these specifics in the plan

| Brief says | Reality |
|---|---|
| Task 13: "Request 2M PHY on connection" | Legacy firmware only *responds* to PHY requests, with `PHY_AUTO`. Requesting 2M is a behaviour change, not a port. |
| Task 23: "Replace the custom heap allocator" | Dead code — delete it. No allocator needed. |
| Task 21: FDS → Settings migration | No migration tooling needed; erase-and-reflash makes it moot. |
| Service list omits NUS | NUS carries the entire config and factory-test protocol. Not optional. |
| Advertising "300 ms" | Correct — but the source comment says 187.5 ms and is wrong. Trust the constant. |
| Buttonless DFU service | Disappears under SMP. **Companion-app-visible breaking change** — raise with the app owner. |

---

## 8. Safety

This firmware sits in the communication path of insulin pump hardware (Medtronic
Minimed 722, Omnipod). Nothing in this report has been validated on hardware, and
no firmware was built or flashed.

- Bench and test hardware only. No clinical or real-patient use.
- The three buffer overflows in §B3 are in **deployed** firmware. They are patient
  safety issues, not only security issues, and warrant a decision now rather than
  at the end of a migration.
- The MCUboot signing key is a patient-safety control. See
  [`../keys/README.md`](../keys/README.md) — the generated keys are development
  keys and must not be promoted to production.
- Every change requires independent review and hardware-in-the-loop validation
  before any field use. In particular the encoding paths (`4b6b.c`,
  `manchester.c`) and the sub-GHz state machine must be validated against real
  pump hardware, since a single flipped bit changes what a pump receives.

---

## Sources

Nordic Semiconductor, consulted 2026-09-10:

- [nRF Connect SDK gets Long-term support with version 3.4.0](https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/nrf-connect-sdk-gets-long-term-support-with-version-3-4-0) — LTS status; nRF52 Series declared feature complete; 5-year support
- [nRF Connect SDK memory requirement for nrf52810 OTA](https://devzone.nordicsemi.com/f/nordic-q-a/96536/nrf-connect-sdk-memory-requirement-for-nrf52810-ota) — 132.8 KB / 16.8 KB minimal figures; MCUboot overflow; Nordic's recommendations
- [Bare Metal option for nRF54L Series](https://www.nordicsemi.com/Products/Development-software/nRF-Connect-SDK/Bare-Metal-option-for-nRF54L-Series) — Bluetooth LE support on nRF54L Series only
- [nRF Connect SDK Bare Metal documentation](https://nrfconnectdocs.nordicsemi.com/ncs-bm/latest/nrf-bm/index.html) — distinct repository; supported device list; v2.0.99
- [Memory footprint optimization](https://nrfconnectdocs.nordicsemi.com/ncs/latest/nrf/test_and_optimize/optimizing/memory.html) — constrained-device guidance

Measured data: `app.uvprojx`, `Peanut_secure_boot.uvprojx`, `app.map`, `boot.map`,
`app.hex`, `boot.hex`, `s112_nrf52_6.1.1_softdevice.hex`, `sdk_config.h`, and the
application sources, all from upstream commit `16f1f27`.
