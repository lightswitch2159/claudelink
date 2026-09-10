# Decision Record: XIAO nRF52840 as the Target Platform

**Date:** 2026-09-10
**Supersedes:** [`DECISION-hardware-frozen.md`](DECISION-hardware-frozen.md) — hardware is open again
**Status:** Recommendation — **viable, and the best option evaluated so far.** Needs sign-off.

## Summary

The Seeed Studio XIAO nRF52840 makes the NCS migration **sensible again**, for the
reason that killed it on nRF52810: it keeps Bluetooth OTA.

| | nRF52810 (current) | XIAO nRF52840 |
|---|---|---|
| Flash | 192 KB — **fully committed** | **1 MB** internal + 2 MB external QSPI |
| RAM | 24 KB — 8.4 KB free | **256 KB** |
| MCUboot dual-slot OTA | Does not fit | **Fits easily** |
| Zephyr board definition | Must be written from scratch | **Already exists upstream** (`xiao_ble`) |
| Secondary update path | None | **USB** (nRF52840 has native USB) |
| RF layout work | — | **None for BLE** — module is pre-built |

The memory wall is gone by an order of magnitude, and the decisive argument
against NCS — surrendering the ability to patch the fleet over BLE — no longer
applies. **Recommend proceeding with the NCS migration on this platform.**

---

## 1. Pin budget — the real engineering question

This design needs more I/O than a first glance suggests, because **it has two
RFM69 radios**, not one:

```c
/* periph/rf69/rf69.h:37 */
RF69_DEV_FREQ433 = 0,        /* Omnipod,  433 MHz */
RF69_DEV_FREQ916N868         /* Minimed,  916/868 MHz */
```

Both are used in `app_subg.c`, each with its own chip select (legacy P0.12 and
P0.20). Any pin budget that assumes one radio is wrong.

The XIAO exposes **11 GPIO** (D0–D10). Raw requirement is 10 signals — which
would fit with one pin spare and no room for the interrupt-driven redesign the
timing analysis calls for. **But two requirements are satisfied by onboard
peripherals and cost no header pins at all:**

### Absorbed by onboard hardware — 0 header pins

| Function | XIAO onboard | Notes |
|---|---|---|
| LED 0 / LED 1 | RGB LED: R=**P0.26**, G=**P0.30**, B=**P0.06** | Common anode, **active LOW** — matches the legacy active-low LEDs |
| Battery ADC | **P0.31** (AIN7) + **P0.14** read-enable | Replaces P0.04/AIN2 and the external divider |

The legacy indication patterns include `LED_ACT_YELLOW_TWINKLE`, i.e. the original
board had red + green discrete LEDs and made yellow by driving both. An RGB LED
produces yellow directly, so the capability is preserved — but see §3.1, the
driver's two-pin model has to change.

> **Electrical hazard, must be handled in code:** Seeed's documentation states
> that when **P0.14 is HIGH the battery sense path is disabled and P0.31 may reach
> 3.6 V, risking damage to the pin.** P0.14 must be held **LOW** whenever P0.31 is
> read. This is a hard requirement on the ported `app_battery.c`, not a
> nice-to-have.

### Header pin allocation — 9 of 11 used

| Function | XIAO | nRF52840 | Note |
|---|---|---|---|
| SPI SCLK | D8 | P1.13 | XIAO's default SPI pins — natural fit |
| SPI MISO | D9 | P1.14 | |
| SPI MOSI | D10 | P1.15 | |
| RFM69 #1 (433) NSS | D0 | P0.02 | Manual GPIO CS, as in the legacy driver |
| RFM69 #2 (916/868) NSS | D1 | P0.03 | |
| **RFM69 #1 DIO0 (IRQ)** | D2 | P0.28 | **New** — see §1.1 |
| **RFM69 #2 DIO0 (IRQ)** | D3 | P0.29 | **New** |
| PWM motor | D4 | P0.04 | Requires a driver transistor — §3.2 |
| PWM buzzer | D5 | P0.05 | XH_5102-equivalent variant |
| *spare* | D6 | P1.11 | Default UART TX |
| *spare* | D7 | P1.12 | Default UART RX |

**Two pins spare.** Options for them: the RFM69 `RESET` lines, or keep them as a
UART debug console. A console is arguably free anyway — the nRF52840 has native
USB, so a USB CDC console costs no header pins, and RTT over SWD is also
available. Recommend allocating D6/D7 to RFM69 RESET and taking the console over
USB or RTT.

### 1.1 The DIO0 lines are a real improvement, not just a port

The legacy driver configures DIO0 on the radio side —
`{ REG_DIOMAPPING1, RF_DIOMAPPING1_DIO0_00 } //DIO0 is the only IRQ we're using` —
but **no MCU interrupt is ever wired up.** There is no GPIOTE configuration
anywhere in `rf69.c`. The firmware polls `REG_IRQFLAGS2` over SPI in busy-wait
loops instead.

That polling is the root of the timing problem described in
[`aps-protocol-spec.md`](aps-protocol-spec.md) §6.2. Since the carrier board is
being respun anyway, **route DIO0 from each radio to the MCU.** It converts the
single largest technical risk in the migration from "mitigate with careful thread
priorities" into "handle an interrupt," and it costs two GPIO the budget can
afford.

This is the strongest argument for the XIAO beyond raw memory: on nRF52810 there
were no spare pins to do this with.

---

## 2. What gets easier

### 2.1 Flash is a non-issue

| Partition | Size |
|---|---|
| MCUboot | 48 KB |
| Primary slot | 448 KB |
| Secondary slot | 448 KB |
| Settings (NVS) | 32 KB |
| **Total** | **976 KB of 1024 KB** |

The application needs ~180 KB in a 448 KB slot. Alternatively put the secondary
slot on the onboard **2 MB external QSPI flash** and give the application the
whole internal bank. Either works — this is a comfortable decision rather than a
constrained one.

RAM: ~25 KB of 256 KB. Thread stacks become free, which is what makes the §1.1
redesign practical.

### 2.2 Board support already exists

`xiao_ble` is an upstream Zephyr board with documentation in the NCS tree, and
`west build -b xiao_ble` works today. Phase 1 task 6 shrinks from "write two board
definitions from scratch" to "write one `.overlay` on top of an existing board."

> **Verify first:** board support was confirmed present in NCS 3.0.1's Zephyr tree
> and in Zephyr mainline. Confirm it is in **v3.4.0** specifically as the opening
> task of Phase 1, along with a real proof-of-fit build.

### 2.3 Two independent update paths

BLE OTA via SMP **plus** USB. For a device with known vulnerabilities that need
patching, having a second channel that does not depend on the BLE stack working is
a genuine safety property. The nRF52810 had neither.

---

## 3. What to watch

### 3.1 The LED driver changes behaviour, not just pins

`periph/led/led.c` is written against two independent GPIO with timer-based
twinkle patterns. The XIAO has one common-anode RGB device. The patterns are
reproducible — arguably better, since colours become explicit rather than
emergent — but this is a **rewrite of the driver's model, and indication
behaviour is user-visible.** Get the intended pattern set confirmed by whoever
owns the product behaviour before porting, and record any deviation in
[`MIGRATION_NOTES.md`](../MIGRATION_NOTES.md).

### 3.2 GPIO cannot drive the motor directly

XIAO GPIO are limited to **15 mA**, and there is a documented Seeed forum thread
about being unable to drive a vibration motor disc from a XIAO nRF52840 pin. The
motor needs a MOSFET or driver IC on the carrier board. The legacy board may
already have one — **confirm against the existing schematic**, which is still not
in this repository.

### 3.3 Replace the Adafruit UF2 bootloader — security requirement

The XIAO ships with the Adafruit nRF52 bootloader, which exposes UF2 drag-and-drop
flashing. **UF2 performs no signature verification.** Left in place on a shipping
device it is an unsigned-firmware-load path reachable by anyone with physical
access, on a device in the insulin pump communication path.

Required before any field use:

- Replace with MCUboot, signed with the keys in [`../keys/README.md`](../keys/README.md)
- Set `UICR APPROTECT` (the legacy build did this — `nrfjprog --memwr 0x10001208 --val 0xFFFFFF00`)
- Confirm no residual UF2 or serial-recovery path accepts unsigned images

### 3.4 A dev board in a medical-adjacent product — product questions, not technical ones

These are outside my scope to judge but must be answered by someone:

- **Lifecycle:** will Seeed commit to availability and change notification for the
  product's lifetime? A dev board is not usually sold with an EOL commitment.
- **Regulatory:** can Seeed supply the documentation needed for your submission?
  The XIAO carries CE/FCC marks, and not laying out the BLE radio is a real
  advantage, but a body-worn accessory integration needs traceable module-level
  documentation.
- **Mechanical:** the USB-C connector and exposed castellated pads are ingress and
  ESD paths in a body-worn device. Form factor is 21 × 17.5 mm — confirm it fits
  the enclosure.
- **Supply chain:** single-source module from one vendor.

### 3.5 The sub-GHz timing risk is reduced, not removed

nRF52840 is the same 64 MHz Cortex-M4 core (with FPU). This is **memory relief,
not scheduling relief.** Zephyr will still starve the Bluetooth host thread if
`Subg_GetPkt()` is ported as a multi-second busy-wait
([`aps-protocol-spec.md`](aps-protocol-spec.md) §6.2).

What changes is that the fix is now *affordable*: generous thread stacks are free,
and §1.1 gives you the interrupt lines to build it properly. Plan the dedicated
sub-GHz thread and DIO0-driven RX in Phase 1 — do not discover it in Phase 4.

### 3.6 nRF52840 is also feature-complete in NCS

The entire nRF52 Series is declared feature complete in v3.4.0. You get 5 years of
LTS bug and security fixes but no new features. Acceptable — this application needs
nothing beyond BLE 5.0 — but make it a conscious choice.

**With hardware open again, nRF54L is technically back on the table** and is
current-generation rather than feature-complete
([`FEASIBILITY_REPORT.md`](FEASIBILITY_REPORT.md) §6). The XIAO's advantages over
it are concrete and immediate: off-the-shelf availability, existing Zephyr board
support, no RF layout, and a mature toolchain. Unless there is a long-horizon
platform mandate, the XIAO is the pragmatic pick. Worth one explicit
conversation rather than defaulting.

---

## 4. The installed base does not go away

**New hardware does not fix deployed devices.** Every nRF52810 unit in the field
still carries the three remotely reachable buffer overflows
([`aps-protocol-spec.md`](aps-protocol-spec.md) §7,
[`config-storage-spec.md`](config-storage-spec.md) §4), and the only channel that
reaches them is the existing nRF5 SDK DFU path.

**Step 1 of [`DECISION-hardware-frozen.md`](DECISION-hardware-frozen.md) remains
valid and should proceed in parallel:** fix the three overflows on the current
nRF5 SDK build and ship via the existing DFU pipeline. It is days of work, needs
no new hardware, and has no dependency on the migration. It is easy to lose sight
of the installed base the moment a new platform appears — don't.

The DFU signing key rotation question (Step 4 there) also still applies to fielded
devices.

---

## 5. Revised plan

| # | Work | Notes |
|---|---|---|
| **0** | **Fix the three overflows on nRF5 SDK, ship to the existing fleet** | Parallel, independent, start now |
| 1 | Answer §3.4 product questions; confirm the schematic re: §3.2 | Blocking the hardware commit |
| 2 | Re-open the legal review — Zephyr is back in scope | GPL v2 vs Apache 2.0, [`FEASIBILITY_REPORT.md`](FEASIBILITY_REPORT.md) §B2. **Blocks publication** |
| 3 | Confirm `xiao_ble` in NCS v3.4.0; `west init` + `west update` + **real proof-of-fit build** | First code task. The gate Phase 0 could not close |
| 4 | Carrier board: 2× RFM69 + **DIO0 routed**, motor driver, buzzer, battery | Respin, but no BLE RF layout |
| 5 | Phase 1–6 of the original brief, with the §3 corrections | LED model, battery pins, DIO0 interrupts, MCUboot replacing UF2 |

Blockers 1 and 2 are the gates. Item 0 should not wait for either.

---

## 6. Verdict

**Recommend proceeding.** The XIAO nRF52840 removes the blocker that made this
migration inadvisable, preserves BLE OTA, comes with existing Zephyr board
support, and — via the DIO0 lines the pin budget can now afford — lets you fix
the timing risk properly instead of working around it.

The open questions are about **productising a dev board** (§3.4), not about
whether the software can be built. Those are the ones to resolve first.

Two things must not get lost: the legal review comes back with Zephyr, and the
fielded nRF52810 devices still need their security patch regardless.

## Safety

Unchanged, plus two specific to this platform:

- **P0.14 must be held LOW during battery reads** or P0.31 can be damaged (§1).
- **The UF2 bootloader must be removed** before any field use — it accepts
  unsigned firmware (§3.3).
- Bench and test hardware only. No clinical or real-patient use. Independent
  review and hardware-in-the-loop validation against real pump hardware before
  any field deployment.

## Sources

- [XIAO BLE (Sense) — Zephyr board documentation in the NCS tree](https://nrfconnectdocs.nordicsemi.com/ncs/3.0.1/zephyr/boards/seeed/xiao_ble/doc/index.html) — `xiao_ble` board support
- [XIAO BLE (Sense) — Zephyr Project documentation](https://docs.zephyrproject.org/latest/boards/seeed/xiao_ble/doc/index.html) — upstream board definition
- [Getting Started with Seeed Studio XIAO nRF52840](https://wiki.seeedstudio.com/XIAO_BLE/) — RGB LED pins, battery sense P0.31/P0.14 and the 3.6 V hazard, memory
- [XIAO nRF52840 with Zephyr RTOS](https://wiki.seeedstudio.com/XIAO-nRF52840-Zephyr-RTOS/) — build/flash flow, UF2 bootloader
- [Seeed forum: cannot drive a vibration motor disc from XIAO GPIO](https://forum.seeedstudio.com/t/xiao-sense-nrf52840-non-usable-current-output-from-gpio-pins-cant-turn-on-vibration-motor-disc/276409) — §3.2
- [nRF Connect SDK LTS v3.4.0](https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/nrf-connect-sdk-gets-long-term-support-with-version-3-4-0) — nRF52 Series feature complete, 5-year support

Design I/O requirements measured from `periph/rf69/rf69.h`, `rf69.c`,
`project/app/src/app_subg.c` and `boards/bd_*_config.h` at upstream commit `16f1f27`.
