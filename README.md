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
