# Board definitions

Empty by design. Devicetree overlays are **Phase 1**, and they cannot be written
until the chip decision (blocker B1 in `docs/FEASIBILITY_REPORT.md`) is made —
the board name and SoC in every overlay filename depend on it.

Pin assignments to encode, from `legacy/boards/bd_xh601_config.h` and
`bd_xh_5102_config.h` (identical except the buzzer):

| Function | Pin | Notes |
|---|---|---|
| LED 0 / LED 1 | P0.05 / P0.06 | Active low |
| Battery ADC | P0.04 | SAADC AIN2 |
| SPI MOSI / MISO / SCLK | P0.15 / P0.16 / P0.18 | SPIM0, RFM69 |
| SPI NSS 0 / NSS 1 | P0.12 / P0.20 | **Manual GPIO chip select, not the SPI peripheral's CS** |
| PWM motor | P0.30 | 50% duty |
| PWM buzzer | P0.28 | **XH_5102 only**, 50% duty, top = 366 |

Other board-specific values that move out of headers:

| | XH601 | XH_5102 |
|---|---|---|
| Default BLE name | `"Orange"` | `"OrangePro"` |
| FW version | 2.05 | 1.00 |
| HW version | 2.05 | 2.06 |

Do **not** reproduce the legacy flash constants (`FLASH_BOOTLOADER_ADDRESS` etc.).
The layout changes completely under MCUboot, and the legacy scatter region
overlapped the FDS pages — see `docs/legacy-flash-budget.md`.
