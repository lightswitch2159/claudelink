# Legacy Flash and RAM Budget (Measured)

**Phase 0, task 6.** Extracted from the committed Keil build artifacts, not estimated.

Sources of truth used:

| Fact | Extracted from |
|---|---|
| Region base/size assignments | `app.uvprojx`, `Peanut_secure_boot.uvprojx` (`OCR_RVCT4` = IROM1, `OCR_RVCT9` = IRAM1) |
| Actual image sizes | `app.map`, `boot.map` (armlink load/execution region sizes and totals) |
| SoftDevice extent | Address span of `update/s112_nrf52_6.1.1_softdevice.hex` |
| Storage/settings pages | `bd_xh601_config.h` flash constants, `.map` load regions at `0x2E000`/`0x2F000` |

Target: **nRF52810_xxAA — 192 KB flash (`0x30000`), 24 KB RAM (`0x6000`)**, confirmed
by the Keil device string `IRAM(0x20000000,0x6000) IROM(0x00000000,0x30000)`.

---

## Correction to the scoping document

The hardware specification's flash map is wrong in a way that changes the
migration conclusion. It should be replaced by the measured map below.

| Region | Spec document claimed | **Measured** | Delta |
|---|---|---|---|
| SoftDevice S112 | ~25 KB (`0x1000`–`0x7000`) | **96 KB** (`0x1000`–`0x19000`) | **+71 KB** |
| Application | ~124 KB (`0x7000`–`0x1F000`) | **52 KB** (`0x19000`–`0x26000`) | **−72 KB** |
| Bootloader | ~36 KB | **24 KB** (`0x28000`–`0x2E000`) | −12 KB |

S112 v6.1.1 occupies `0x1000`–`0x19000`. This is corroborated three ways: the
SoftDevice hex spans `0x000000`–`0x018C10`; the application's IROM1 base is
`0x19000`; and the DFU scripts declare `--sd-req 0xB8` (the S112 6.1.1 firmware ID).

The practical consequence: the spec implied ~124 KB of application space, so a
Zephyr port looked merely tight. There is actually **52 KB**, and the SoftDevice
alone eats half the part. The budget is far worse than the scoping document
assumed.

---

## Measured flash map (nRF52810, 192 KB)

```
0x00000  ┌────────────────────────────────────┐
         │ MBR v2.4.1                          │   4 KB   ( 4,096 B)
0x01000  ├────────────────────────────────────┤
         │ SoftDevice S112 v6.1.1              │  96 KB   (98,304 B)
         │                                     │
0x19000  ├────────────────────────────────────┤
         │ Application                         │  52 KB   (53,248 B)
         │   used:  31.4 KB (32,176 B)         │   ← 20.6 KB free
         │   free:  20.6 KB                    │
0x26000  ├────────────────────────────────────┤
         │ FDS config storage (2 × 4 KB pages) │   8 KB   ( 8,192 B)
0x28000  ├────────────────────────────────────┤
         │ Bootloader (Nordic Secure DFU)      │  24 KB   (24,576 B)
         │   used:  22.75 KB (23,292 B) = 94.8%│   ← 1.25 KB free
0x2E000  ├────────────────────────────────────┤
         │ MBR parameters page                 │   4 KB
0x2F000  ├────────────────────────────────────┤
         │ Bootloader settings page            │   4 KB
0x30000  └────────────────────────────────────┘
```

**Committed: 170 KB of 192 KB. Free: ~22 KB, and 20.6 KB of that is inside the
application slot.**

### Application, exact figures (`app.map`, XH_5102 build)

```
Load Region LR_IROM1  Base 0x00019000  Size 0x7F38  Max 0xF000  COMPRESSED[0x7DAC]
Exec Region ER_IROM1  Base 0x00019000  Size 0x7CF8  Max 0xF000
Exec Region RW_IRAM1  Base 0x20002B00  Size 0x1390  Max 0x3500  COMPRESSED[0xB4]

Total ROM Size (Code + RO Data + RW Data)   32,176 B  (31.42 KB)
Code                                        30,336 B
RO Data                                      1,660 B
RW Data                                        576 B
ZI Data                                      4,432 B   (includes the 2 KB stack)
```

The XH601 build (`update/app_601.hex`) is 31,600 B — 576 B smaller, consistent
with the buzzer driver being compiled out.

### Bootloader, exact figures (`boot.map`)

```
Load Region LR_IROM1  Base 0x00028000  Size 0x5AFC  Max 0x6000
Total ROM Size                              23,300 B  (22.75 KB)
Total RW Size (RW + ZI)                     19,956 B  (19.49 KB)
```

**The bootloader slot is 94.8% full.** There is no room to grow the existing
bootloader in place, which independently rules out any "add a migration shim to
the current bootloader" strategy.

---

## Measured RAM map (nRF52810, 24 KB)

```
0x20000000  ┌──────────────────────────────────┐
            │ SoftDevice S112 (config-dependent)│  11,008 B  (10.75 KB)  44.8%
0x20002B00  ├──────────────────────────────────┤
            │ Application RW + ZI               │   5,008 B  ( 4.89 KB)  20.4%
            │   of which stack: 2,048 B         │
0x20003E90  ├──────────────────────────────────┤
            │ Unused                            │   8,560 B  ( 8.36 KB)  34.8%
0x20006000  └──────────────────────────────────┘
```

The SoftDevice's 11,008 B RAM demand follows from the BLE configuration in
`sdk_config.h`: `NRF_SDH_BLE_GATT_MAX_MTU_SIZE=247`,
`NRF_SDH_BLE_GATTS_ATTR_TAB_SIZE=3000`, `NRF_SDH_BLE_VS_UUID_COUNT=9`,
one peripheral link, `NRF_SDH_BLE_GAP_EVENT_LENGTH=6`.

**Real RAM headroom: 8.36 KB.**

---

## Two findings worth acting on

### 1. The custom heap is dead code

`userKit/heap/kit_heap.c` declares `static uint8_t heap[10240]` plus a 640-entry
allocation table. Neither appears in `app.map` — `grep -c kit_heap app.map` returns
**0**. The C library heap is also discarded (`Removing
arm_startup_nrf52810.o(HEAP), (2048 bytes)`). Total ZI is 4,432 B, far below the
10 KB the heap array alone would need.

**The application performs no dynamic allocation.** Migration task 23 ("replace
the custom heap allocator") is therefore a deletion, not a port, and
`CONFIG_HEAP_MEM_POOL_SIZE` can stay at 0. Do not budget 10 KB of RAM for a heap
that was never linked; the `HEAP_SIZE` figure in the hardware spec is misleading.

### 2. The application scatter region overlaps the FDS storage pages

The linker is given `LR_IROM1 0x00019000 0x0000F000` → `0x19000`–`0x28000`, but
FDS claims `0x26000`–`0x28000` (`FLASH_BOOTLOADER_ADDRESS - 2 * FLASH_PAGE_SIZE`).
**The last 8 KB is double-booked.** Today the image ends at `0x20F38`, ~21 KB
short of the collision, so it is latent — but an application that grew past
`0x26000` would silently link over its own configuration storage.

Also note the comment in `bd_xh601_config.h` says FDS needs "3*4096" pages while
`FLASH_PAGE_NUM` is set to `2`. The code and its comment disagree; the code wins
(8 KB reserved), but confirm the intent before mapping the Zephyr partition.

Under Zephyr this class of bug is structurally prevented — partitions come from a
single devicetree/partition-manager source and overlaps are a build error. Worth
noting as a concrete benefit of the migration.

---

## What this budget means for NCS

The entire working legacy product — MBR + SoftDevice + application — is
**131.4 KB**. Nordic's published figure for a *minimal* NCS Bluetooth peripheral
on nRF52810, carrying no application code, no MCUboot, no SPI driver and no
settings storage, is **132.8 KB**.

The baseline platform cost under NCS therefore exceeds the entire current
shipping image. See [`FEASIBILITY_REPORT.md`](FEASIBILITY_REPORT.md) for the
resulting chip recommendation.
