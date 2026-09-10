# Decision Record: nRF52810 Hardware Is Frozen

**Date:** 2026-09-10
**Supersedes:** the chip recommendation in [`FEASIBILITY_REPORT.md`](FEASIBILITY_REPORT.md) §7
**Status:** Recommendation. Needs sign-off.

## Constraint

The nRF52810 cannot be changed. nRF52832 and nRF54L are both off the table, which
removes every option the Phase 0 report recommended.

## Recommendation

**Abandon the NCS migration for this hardware. Modernise the build system and fix
the security defects on the nRF5 SDK instead.**

This is not a fallback. On frozen nRF52810 hardware it is the better engineering
decision, for one decisive reason:

> **Every NCS path on nRF52810 costs you Bluetooth OTA updates.**
>
> MCUboot does not fit in 192 KB (see [`FEASIBILITY_REPORT.md`](FEASIBILITY_REPORT.md) §2).
> Nordic's own suggestions for this exact constraint are serial-recovery DFU,
> external flash, or staying on the nRF5 SDK. Without MCUboot there is no
> SMP-over-BLE, so field updates become SWD- or serial-only.
>
> This firmware has **three known, remotely reachable buffer overflows**. Migrating
> to NCS would mean surrendering the only channel you have for shipping the fix to
> deployed devices — in order to gain a build system you can obtain without
> migrating at all.

The trade is backwards. Rejecting NCS here preserves the ability to patch the
fleet, which on a device in the insulin pump communication path is the property
that matters most.

## Why nRF5 SDK is a defensible place to stay

Nordic's position, stated directly:

> "The nRF5 SDK is not deprecated and it will be maintained for the foreseeable
> future." Bug fixes and security updates remain available as needed; only
> features beyond Bluetooth LE 5 are excluded.

The application needs no post-BLE-5 features. It is a BLE 4.2/5.0 peripheral
bridging to an external sub-GHz radio, and nothing in the roadmap requires
Bluetooth features the nRF5 SDK lacks.

Staying also **removes the legal blocker from the critical path.** Combining
GPL v2-only application code with Apache 2.0 Zephyr code was the exposure
([`FEASIBILITY_REPORT.md`](FEASIBILITY_REPORT.md) §B2). Remaining on the nRF5 SDK
keeps the licensing situation exactly as upstream already ships it — a
pre-existing condition rather than a new incompatibility. Still worth counsel
before publishing the fork, but it no longer gates development.

## What you give up

Stated plainly, so the decision is made with open eyes:

| Lost | Does it matter here? |
|---|---|
| Bluetooth features beyond BLE 5.0 | No — not needed by this application |
| Zephyr ecosystem: drivers, `ztest`, devicetree, partition manager | Partly. Unit testing is recoverable (see Step 2); the rest is not |
| A larger pool of engineers familiar with the platform | Real long-term risk, but unactionable while hardware is frozen |
| Automatic protection against the FDS/scatter overlap bug class | Recoverable by hand: assert the linker region ends below `0x26000` |
| Long-term platform trajectory | Deferred, not lost. Revisit at the next hardware revision |

None of these are actionable on frozen nRF52810 hardware, which is what makes the
trade acceptable.

---

## Recommended plan

Four steps, deliberately decoupled so a security fix never waits on a migration.

### Step 1 — Fix the three overflows. Days, not weeks. Start here.

On the **current** SDK, current SoftDevice, existing DFU pipeline. No build-system
change, no SDK bump, no legal dependency.

| Function | File | Destination | Max write |
|---|---|---|---|
| `Aps_PutCmd()` | `app_aps.c:686` | 123 B | 148 B |
| `Cfg_PutReq()` | `app_config.c:253` | 2 B | 242 B |
| `Fct_PutReq()` | `app_factory.c:434` | 20 B | 242 B |

Bound-check before each `memcpy`, and return the protocol's parameter-error code
where one exists rather than dropping the frame silently. Detail in
[`aps-protocol-spec.md`](aps-protocol-spec.md) §7 and
[`config-storage-spec.md`](config-storage-spec.md) §4.

Ships to the fleet over the existing Nordic Secure DFU path. There is **20.6 KB of
free flash** in the application slot, so the fix costs nothing structural.

Add regression tests for the three parsers (see Step 2 — the encoding and parsing
code is host-testable without any SDK).

### Step 2 — Move off Keil to GCC + CI. Weeks.

This is where most of the value people actually want from "update the SDK" lives,
and it is fully available on this hardware.

Groundwork already in place:

- `nrfSDK/components/softdevice/s112/toolchain/armgcc/armgcc_s112_nrf52810_xxaa.ld`
  — **the GCC linker script for exactly this chip and SoftDevice is already
  vendored**
- The SDK ships `pca10040e` examples targeting nRF52810 + S112 with `armgcc`
  Makefiles to model the build on
- The code is C99 and has no Keil-specific pragmas

What has to be obtained: the vendored `nrfSDK/` is a **stripped partial copy**
(only `components`, `external`, `integration`, `modules`). It has no
`components/toolchain/` and no `modules/nrfx/mdk/`, so `gcc_startup_nrf52810.S`
and `system_nrf52810.c` are missing — the Keil build took those from the Device
Family Pack instead. Replace the partial copy with a **pinned, complete SDK**
(documented version, checksum, fetched by script rather than committed).

Deliverables:

1. `Makefile` (or CMake) building both board variants with `arm-none-eabi-gcc`
2. Pinned SDK fetch script replacing the stripped in-tree copy
3. GitHub Actions CI building both variants on every push, publishing the `.hex`
   and the `.map` — with **a build step that fails if the image would grow past
   `0x26000`**, closing the FDS-overlap hole described in
   [`legacy-flash-budget.md`](legacy-flash-budget.md)
4. Host-compiled unit tests for `4b6b.c`, `manchester.c` and the three command
   parsers — pure C, no SDK dependency, no hardware needed
5. Static analysis in CI

Gains: no Keil licence needed to build or patch, reproducible builds, and the
regression safety net that makes any later change — including a future NCS port —
tractable.

> Watch for one documented trap: builds for real nRF52810 silicon must **not**
> define `DEVELOP_IN_NRF52832`. The current Keil project correctly omits it;
> preserve that.

### Step 3 — Decide the SDK version bump separately. Do not couple it to Step 1.

The build is on `NRF_SD_BLE_API_VERSION=6` / S112 6.1.1, i.e. nRF5 SDK ~15.3–16.0.
Only **17.1.0** (August 2021) receives maintenance fixes today.

Upgrading costs a BLE API v6 → v7 migration plus, because the SoftDevice major
version changes, a combined **SD + BL DFU** to the fleet — a materially riskier
update than an application-only DFU. Worth doing, but as its own project with its
own validation. Never bundle it with a security patch.

### Step 4 — Consider rotating the DFU signing key. Security decision, needs sign-off.

All four legacy DFU private keys were committed in plaintext upstream, so anyone
who cloned the repository can sign firmware your deployed devices will accept.

Uncomfortable but true: the compromised public key is compiled into the bootloader
on every fielded device, so the fix must travel through the compromised channel.
The Nordic Secure DFU bootloader **can** be updated over DFU, so a bootloader
carrying a new public key can be signed with the old key and pushed. The
bootloader slot is 94.8% full, but a key swap is size-neutral, so it fits.

This closes the window going forward. It does not retroactively protect devices,
and a bootloader DFU is the riskiest update type there is — a failure can brick a
device. Sequence it after Step 1, treat it as a separate release, and validate on
bench hardware extensively.

### Step 5 — Revisit NCS at the next hardware revision, not before.

Keep the Phase 0 documents. The GATT, APS and configuration specs are
**SDK-independent** — they describe the wire protocols and the companion app's
expectations, and they remain the specification for any future port. Whenever
hardware opens up, that is the moment to compare nRF52832 against nRF54L
([`FEASIBILITY_REPORT.md`](FEASIBILITY_REPORT.md) §6), and to do it as a
new-hardware project rather than a port of existing devices.

---

## What changes in the repository

The NCS scaffolding is now speculative rather than the active plan:

| File | Disposition |
|---|---|
| `west.yml`, `CMakeLists.txt`, `prj.conf` | Keep, clearly marked as deferred to a future hardware revision |
| `boards/README.md` | Keep — the pin map is correct regardless of SDK |
| `keys/` | Keep. The new MCUboot keys are unused for now; the P-256 keys remain the right shape if Step 4 proceeds, though Nordic DFU and MCUboot use different signature formats |
| `docs/*-spec.md` | Keep, unchanged. SDK-independent and still the source of truth |
| `docs/FEASIBILITY_REPORT.md` | Keep. Its chip recommendation is superseded by this record; its measurements and blockers stand |
| `legacy/` | Becomes the **active** source tree, not a reference copy |

If Steps 1–2 are approved, `legacy/` should be promoted to the repository root and
the fork reframed as a maintained nRF5 SDK project rather than a migration
in progress.

---

## Safety

Unchanged, and more pointed now that a fix is on the near-term path:

- The three overflows are in **deployed** firmware. They are patient-safety issues
  as much as security issues.
- Step 1 must have independent review and hardware-in-the-loop validation against
  real pump hardware before release.
- Step 4 (bootloader DFU) can brick devices on failure. Bench validation only,
  extensively, before any field rollout.
- Bench and test hardware only throughout. No clinical or real-patient use.

## Sources

- [nRF Connect SDK and nRF5 SDK statement](https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/nrf-connect-sdk-and-nrf5-sdk-statement) — nRF5 SDK not deprecated, maintained for the foreseeable future, security fixes as needed
- [nRF5 SDK v17.1.0 release notes](https://developer.nordicsemi.com/nRF5_SDK/nRF5_SDK_v17.x.x/doc/17.1.0/release_notes.txt) — latest version, August 2021
- [nRF Connect SDK memory requirement for nrf52810 OTA](https://devzone.nordicsemi.com/f/nordic-q-a/96536/nrf-connect-sdk-memory-requirement-for-nrf52810-ota) — MCUboot does not fit; Nordic's recommendations for this constraint
- [nRF52810 minimal project with armgcc Makefile](https://devzone.nordicsemi.com/f/nordic-q-a/36081/nrf52810-minimal-project-example-with-makefile-for-arm-gcc) — `pca10040e` examples; the `DEVELOP_IN_NRF52832` trap
- [How to set up nRF5 SDK with ARM GCC](https://blog.makerdiary.com/how-to-set-up-nrf5-sdk-with-arm-gcc/) — GCC toolchain setup; the `components`/`external`/`integration`/`modules` subset
