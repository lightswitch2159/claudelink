# BLE GATT Service Specification (Derived from Source)

**Phase 0, task 7.** The `docs/gatt-service-spec.md` referenced by the migration
brief does not exist in the upstream repository, so this specification was
reconstructed from `lib/pump/ble_services/ble_ips/ble_ips.c`,
`ble_ips.h`, `project/app/src/app_ble.c` and `project/app/config/sdk_config.h`.

Every value here is what the shipping firmware actually does. The companion
mobile app depends on all of it, so the NCS port must reproduce it exactly.

---

## 1. Effective MTU and the resulting length limits

```c
/* project/app/config/sdk_config.h:9915 */
#define NRF_SDH_BLE_GATT_MAX_MTU_SIZE   247
```

```c
/* ble_ips.h:65 */
#define BLE_IPS_MAX_DATA_LEN  (NRF_SDH_BLE_GATT_MAX_MTU_SIZE - OPCODE_LENGTH - HANDLE_LENGTH)
                           /* = 247 - 1 - 2 = 244 */
```

| Quantity | Value |
|---|---|
| Negotiated ATT MTU (max) | **247** |
| Max notification/write payload | **244** |
| Data characteristic max length | **150** (`min(150, 244)`) |
| Custom Name max length | **30** (`min(30, 244)`) |
| Response Count / Timer Tick / LED Mode length | **1** |

The 150-byte Data cap comes from `BLE_RESPONSE_MAX_LEN` in `app_aps.c`, not from
the MTU — the MTU is generous enough that the application constant binds first.

**Zephyr equivalent:** `CONFIG_BT_BUF_ACL_RX_SIZE=251`, `CONFIG_BT_L2CAP_TX_MTU=247`,
`CONFIG_BT_ATT_PREPARE_COUNT` as needed. Note Zephyr does not silently truncate to
the negotiated MTU the way the SoftDevice helper did — the port must clamp writes
to 150 explicitly.

---

## 2. Insulin Pump Service (IPS)

**Service UUID:** `0235733B-99C5-4197-B856-69219C2A3845` (16-bit alias `0x733B`)

The base UUID is stored little-endian in the source and the 16-bit alias is
substituted into bytes 12–13 per the SoftDevice vendor-UUID scheme. Each
characteristic registers its **own full 128-bit UUID** via `sd_ble_uuid_vs_add`
— they are *not* derived from the service base. Two of them (Response Count and
Timer Tick) share the same 16-bit alias `0x7910` and differ only in the full
128-bit value, so an implementation that keys off 16-bit aliases will break.

`NRF_SDH_BLE_VS_UUID_COUNT` is `9` (1 service + 6 characteristics + headroom).

### Characteristics

| # | Name | UUID (128-bit) | Properties | Len | Var? | User Description | CCCD |
|---|---|---|---|---|---|---|---|
| 1 | Data | `C842E849-5028-42E2-867C-016ADADA9155` | Read, Write | ≤150 | Yes | `"Data"` | — |
| 2 | Response Count | `6E6C7910-B89E-43A5-A0FE-50C5E2B81F4A` | Read, **Notify** | 1 | No | `"Response Count"` | Yes |
| 3 | Timer Tick | `6E6C7910-B89E-43A5-78AF-50C5E2B86F7E` | Read, **Notify** | 1 | No | `"Timer Tick"` | Yes |
| 4 | Custom Name | `D93B2AF0-1E28-11E4-8C21-0800200C9A66` | Read, Write | ≤30 | Yes | `"Custom Name"` | — |
| 5 | Version | `30D99DC9-7C91-4295-A051-0A104D238CF2` | Read | 13 | No | `"Version"` | — |
| 6 | LED Mode | `C6D84241-F1A7-4F9C-A25F-FCE16732F14E` | Read, Write | 1 | No | `"LED Mode"` | — |

All six use `SEC_OPEN` for read, write and CCCD write — **no pairing, no bonding,
no encryption requirement**. Attribute values live in the stack
(`BLE_GATTS_VLOC_STACK`), not in application memory.

`Version` returns the fixed string **`"ble_rfspy 2.0"`** (13 bytes). Note this is
distinct from the APS protocol version string `"subg_rfspy 2.2"` returned by
`CMD_GET_VER` over the Data characteristic.

Every characteristic carries a Characteristic User Description (`0x2901`)
descriptor with the exact string in the table. These are readable and the app may
rely on them; reproduce them.

---

## 3. The request/response handshake — get this exactly right

The Data characteristic has **no notify property**. Responses are delivered by a
two-step handshake, implemented in `Ble_IpsNotifyRespCntAndSendData`
(`app_ble.c:665`):

```
1. Client WRITES a command to  Data            (triggers BLE_IPS_EVT_DATA_RX)
2. Firmware queues it, executes it            (app_aps.c: aps_cmd_loop)
3. Firmware SETS the Data attribute value      (sd_ble_gatts_value_set)
4. Firmware INCREMENTS Response Count and NOTIFIES it
5. Client, on the notification, READS Data
```

Ordering is mandatory: the value must be in place **before** the Response Count
notification fires, and the counter is only incremented when the value write
succeeded:

```c
err_code = ble_ips_data_send(data, len, &m_ips);
if (err_code == NRF_SUCCESS) {
    ble_ips_response_cnt_notify(&m_ips);   /* only on success */
}
```

**Response Count semantics:** `uint8_t`, incremented per successful response,
reset to `0` when it reaches `0xFF` — so it cycles `0x00`–`0xFE` and **never
takes the value `0xFF`**. Initialised to `0` at service init.

In Zephyr, step 3 is a write to the characteristic's backing value plus step 4 as
`bt_gatt_notify()`. Because Zephyr's `bt_gatt_notify()` is asynchronous and can
return `-ENOMEM` when buffers are exhausted, the port must use
`bt_gatt_notify_cb()` and only advance the counter on the sent callback, or it
will drift from the legacy behaviour under load.

---

## 4. Timer Tick

```c
#define BLE_TMR_TICK_ONE_MIN  60000   /* ms */
```

A repeating `app_timer` started at service init increments an internal `uint8_t`
every **60 seconds**, applies the same `>= 0xFF → 0` wrap as Response Count,
writes the attribute value, and notifies **only if the client has subscribed**
(`is_tmr_tick_notification_enabled`).

Zephyr equivalent: `k_work_delayable` rescheduled at 60 s, or `k_timer` posting to
the system workqueue. Do not notify from the timer ISR directly.

---

## 5. Other services registered

From `services_init()` (`app_ble.c:590`):

| Service | Legacy implementation | NCS target |
|---|---|---|
| Queued Write | `nrf_ble_qwr_init` | Built in; no code needed |
| Battery Service | `ble_bas_init` | `CONFIG_BT_BAS=y` |
| **IPS (custom)** | `ble_ips_init` | `BT_GATT_SERVICE_DEFINE` — see above |
| Buttonless DFU | `ble_dfu_buttonless_init` | Replaced by SMP-over-BLE (`CONFIG_MCUMGR`) |
| Nordic UART Service | `ble_nus_init` | `CONFIG_BT_NUS` or a hand-rolled service |

**NUS is not decorative** — `app_config.c` uses `Ble_NusSendData()` as the
transport for the configuration protocol (motion data get/set, battery voltage,
buzzer trigger). See [`config-storage-spec.md`](config-storage-spec.md). The
migration brief's service list omits this dependency.

The Buttonless DFU service disappears in the port. Anything in the mobile app
that triggers DFU by writing to it will need updating to the SMP protocol; this
is a **companion-app-visible breaking change** and needs to be raised with
whoever owns that app.

---

## 6. Advertising

```c
#define BLE_ADV_INTERVAL   480    /* × 0.625 ms = 300 ms */
#define BLE_ADV_DURATION   0      /* 0 = no timeout, advertise forever */
#define BLE_TX_POWER_LEVEL 4      /* dBm */
```

> The source comment claims "187.5 ms" — it is stale. `480 × 0.625 ms = 300 ms`.
> 300 ms is the correct value and matches the migration brief.

| Parameter | Value |
|---|---|
| Interval | 300 ms (480 × 0.625 ms) |
| Duration | Unlimited (continuous) |
| Type | Connectable undirected |
| Flags | `LE_ONLY_GENERAL_DISC_MODE` |
| Advertised services | IPS UUID, as Complete List of 128-bit UUIDs |
| TX power | +4 dBm |
| Device name | From config storage; default `"Orange"` (XH601) / `"OrangePro"` (XH_5102) |

The GAP device name attribute is set to `BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS` —
the name is **not** a readable GATT attribute, only an advertising field. A naive
Zephyr port exposes a writable/readable Device Name by default; match the legacy
behaviour deliberately or the app may behave differently.

Zephyr: `BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, 480, 480, NULL)` with
`BT_DATA_BYTES(BT_DATA_UUID128_ALL, ...)`. TX power needs
`CONFIG_BT_CTLR_TX_PWR_PLUS_4` (nRF52 sets radio TX power at build time via
Kconfig, not per-connection as `sd_ble_gap_tx_power_set` did).

---

## 7. Connection parameters

| Parameter | Value |
|---|---|
| Min connection interval | 50 ms (`MSEC_TO_UNITS(50, UNIT_1_25_MS)` = 40) |
| Max connection interval | 100 ms (= 80) |
| Slave latency | 0 |
| Supervision timeout | 5000 ms (= 500) |

Zephyr: `CONFIG_BT_PERIPHERAL_PREF_MIN_INT=40`, `..._MAX_INT=80`,
`..._LATENCY=0`, `..._TIMEOUT=500`.

### PHY — a discrepancy with the brief

The migration brief (task 13) says "Request 2M PHY on connection." **The legacy
firmware does not do that.** It only *responds* to a peer-initiated PHY update
request, and answers with `BLE_GAP_PHY_AUTO` for both directions:

```c
case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
    ble_gap_phys_t const phys = { .rx_phys = BLE_GAP_PHY_AUTO,
                                  .tx_phys = BLE_GAP_PHY_AUTO };
    sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
}
```

It never calls `sd_ble_gap_phy_update` on its own initiative. Actively requesting
2M PHY in the port would be a **behaviour change**, not a port. Recommendation:
enable 2M PHY support (`CONFIG_BT_CTLR_PHY_2M=y`, on by default) and let the
central drive it, matching today's behaviour. If 2M is genuinely wanted, treat it
as a separate, deliberately reviewed change with its own interoperability
testing.

---

## 8. Custom Name write has a side effect

Writing the Custom Name characteristic does not just store a string
(`app_ble.c:277`):

```c
case BLE_IPS_EVT_CUS_NAME_RX:
    Cfg_SetAdvName(...);                 /* persist (20 ms debounce timer) */
    bleNameChangeFlg = true;
    sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE);
```

**The device deliberately disconnects** so it can restart advertising under the
new name, and it reports the HCI reason `CONN_INTERVAL_UNACCEPTABLE` (`0x3B`)
rather than anything name-related. The mobile app almost certainly special-cases
this. Preserve the disconnect, and preserve the odd reason code.

---

## 9. Application-facing event model

`ble_ips` delivers three event types to the application (`ble_ips.h:78`):

| Event | Trigger | Consumer |
|---|---|---|
| `BLE_IPS_EVT_DATA_RX` | Write to Data | `Aps_PutCmd()` — with the current connection RSSI attached |
| `BLE_IPS_EVT_CUS_NAME_RX` | Write to Custom Name | `Cfg_SetAdvName()` + disconnect |
| `BLE_IPS_EVT_LED_MODE_RX` | Write to LED Mode | Handler is **empty** — accepted and ignored |

On `DATA_RX` the handler calls `sd_ble_gap_rssi_get()` and passes the RSSI into
the APS layer. Zephyr requires `CONFIG_BT_CTLR_CONN_RSSI=y` and an HCI
`Read RSSI` command for the equivalent; there is no drop-in API. `LED Mode` and
the APS `CMD_LED` (`0x08`) are both accepted-and-ignored no-ops that still must
return success, so the app does not see an error.

---

## Port acceptance checklist

- [ ] `nRF Connect for Mobile` shows service `0235733B-…` with exactly 6 characteristics
- [ ] All 6 full 128-bit UUIDs byte-identical (verify Response Count vs Timer Tick are distinct)
- [ ] All 6 User Description strings present and exact
- [ ] CCCDs present on Response Count and Timer Tick only
- [ ] Data accepts a 150-byte write and reads back ≤150 bytes, variable length
- [ ] Response Count increments 0x00→0xFE and wraps, skipping 0xFF
- [ ] Data value is observably set *before* the Response Count notification
- [ ] Timer Tick notifies every 60 s only when subscribed
- [ ] `Version` reads exactly `"ble_rfspy 2.0"`
- [ ] Advertising at 300 ms, +4 dBm, continuous, IPS UUID in the payload
- [ ] Device name not exposed as a readable GATT attribute
- [ ] Connects without pairing; no security prompt on any characteristic
- [ ] Custom Name write persists the name and disconnects with HCI reason `0x3B`
