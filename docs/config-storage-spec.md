# Configuration Storage and Protocol Specification (Derived from Source)

**Phase 0, task 8.** Extracted from `project/app/src/app_config.c`,
`project/app/inc/app_config.h` and `boards/bd_*_config.h`.

Two separate things are documented here because the code couples them:

1. The **persisted structures** in flash (FDS → Zephyr Settings)
2. The **configuration protocol** carried over NUS (not over the IPS service)

---

## 1. Persisted structures

```c
#define CFG_STORE_ADDR         FLASH_PAGE_0_ADDR   /* 0x27000 */
#define CFG_INIT_FLG           0xAA55AA55
#define CFG_MOTION_DATA_SIZE   16

typedef struct {                 /* NOT packed */
    uint8_t  advName[30];        /* BLE_IPS_MAX_CUS_NAME_CHAR_LEN */
    uint8_t  advNameLen;
    uint32_t advNameInitFlg;     /* CFG_INIT_FLG when valid */
} stBleCfg_t;                    /* offset 0, sizeof 36 (1 pad byte at 31) */

typedef struct {
    uint8_t  data[16];
    uint32_t initFlg;            /* CFG_INIT_FLG when valid */
} stMotionCfg_t;                 /* offset 36, sizeof 20 */

typedef struct {
    stBleCfg_t    ble;
    stMotionCfg_t motion;
} stCfgStorage_t;                /* sizeof 56 */
```

`advNameInitFlg` sits at offset **32**, not 31 — the compiler inserts a padding
byte after `advNameLen` to align the `uint32_t`. Any migration tool that reads
legacy records must respect that padding.

Only two of the 16 motion bytes are used:

| Byte | Meaning | Default |
|---|---|---|
| `data[0]` | LED indication enable (1 = yellow twinkle on request) | `1` |
| `data[1]` | Motor vibration enable (1 = short vibrate on request) | `1` |
| `data[2..15]` | Unused, never written | `0` |

### Storage layout

| Constant | Value | Notes |
|---|---|---|
| `FLASH_BOOTLOADER_ADDRESS` | `0x28000` | |
| `FLASH_PAGE_SIZE` | `4096` | |
| `FLASH_PAGE_NUM` | `2` | Comment in the header says 3 — the code says 2 |
| `FLASH_START_ADDR` / `FLASH_PAGE_1_ADDR` | `0x26000` | |
| `FLASH_PAGE_0_ADDR` | `0x27000` | **`stCfgStorage_t` lives here** |
| `FLASH_END_ADDR` | `0x27FFF` | |

`stCfgStorage_t` occupies 56 bytes of the 4096-byte page at `0x27000`.
`FCT_SN_STORE_ADDR` (serial number, `app_factory.c`) uses the other page at
`0x26000`.

### First-boot initialisation

`Cfg_StorageLoad()` runs after the SoftDevice is enabled (flash access requires
it) and validates each section independently against `CFG_INIT_FLG`:

```
if (ble.advNameInitFlg    != 0xAA55AA55) → advName = BLE_DEFAULT_NAME, len = strlen(...)
if (motion.initFlg        != 0xAA55AA55) → data[0] = 1, data[1] = 1
if (anything was defaulted)              → write the whole 56-byte struct back
```

`BLE_DEFAULT_NAME` is `"Orange"` (XH601) or `"OrangePro"` (XH_5102), so **the
persisted name is board-specific**. A device flashed with the wrong board build
will keep whatever name is already in flash — the flag is set, so no re-default
happens.

### Writes are debounced, and the write is whole-struct

Both `Cfg_SetAdvName()` and `motion_data_set()` mutate the in-RAM copy and start a
single-shot timer; the handler writes all 56 bytes:

```c
static void cfg_update_handle(void *p_context) {
    Flash_Write(CFG_STORE_ADDR, &config, sizeof(stCfgStorage_t));
}
```

There is no read-modify-write of individual fields and no wear levelling beyond
what FDS provides.

> **Bug to fix, not port:** `motion_data_set()` uses
> `app_timer_start(..., APP_TIMER_TICKS(CFG_UPDATE_TIMEOUT), ...)` but
> `Cfg_SetAdvName()` passes the raw constant:
> `app_timer_start(m_cfg_update_timer_id, CFG_UPDATE_TIMEOUT, NULL)`.
> `CFG_UPDATE_TIMEOUT` is `20`, intended as 20 ms. The name path therefore fires
> after 20 *ticks* (well under a millisecond) instead of 20 ms. It happens to work
> because the write completes anyway, but the two paths do not behave the same. In
> Zephyr both become `k_work_schedule(&w, K_MSEC(20))` and the inconsistency
> disappears.

> **Also note:** `Cfg_SetAdvName()` has `config.ble.advNameInitFlg = CFG_INIT_FLG;`
> **commented out**. It works only because the flag was already set during
> `Cfg_StorageLoad()`. Set it explicitly in the port; relying on that is fragile.

---

## 2. Mapping FDS → Zephyr Settings

The natural mapping splits the monolithic struct into individual keys:

| Settings key | Type | Legacy source |
|---|---|---|
| `orangelink/ble/name` | string, ≤30 bytes | `ble.advName` + `ble.advNameLen` |
| `orangelink/ind/led_en` | `uint8_t` | `motion.data[0]` |
| `orangelink/ind/motor_en` | `uint8_t` | `motion.data[1]` |
| `orangelink/sn` | 4 bytes | `app_factory.c` serial number page |

Recommended Kconfig: `CONFIG_SETTINGS=y`, `CONFIG_SETTINGS_NVS=y`,
`CONFIG_NVS=y`, with a `storage_partition` of at least 3 sectors (NVS needs a
spare sector to garbage-collect; two is the documented minimum and leaves no
margin).

The `initFlg` pattern disappears — Settings signals absence by not invoking the
`h_set` callback, so defaults are applied by initialising the variables before
`settings_load()`.

### Do the migration decision consciously

NVS and FDS have incompatible on-flash formats. A device reflashed from the
legacy image to the NCS image will find its config area unreadable and fall back
to defaults, meaning **a user's custom device name is lost and the device
re-advertises as `"Orange"`/`"OrangePro"`**.

Three options, in ascending cost:

1. **Accept the reset.** Document it. The only user-visible loss is a custom name
   and two indication toggles. Given that Phase 5 already concludes devices need
   an SWD reflash (see `FEASIBILITY_REPORT.md`), the config is being wiped by the
   flashing process anyway. **This is the recommended option.**
2. **Read the legacy record once.** The 56-byte struct is at a fixed address in a
   known format; a one-shot import at first boot is ~40 lines. Only worth it if
   the NCS image somehow lands without an erase-all.
3. Full bidirectional migration tooling — not justified here.

Option 1 is recommended precisely *because* the flash layout change already
forces a full erase. Do not build migration tooling for a scenario that cannot
occur.

---

## 3. Configuration protocol (over NUS)

**This runs over the Nordic UART Service, not the IPS service.** The migration
brief's service list omits it; without NUS the configuration protocol, the
factory-test protocol and the buzzer trigger all stop working.

### Dispatch

`nus_data_handler()` (`app_ble.c:229`) switches on the **first byte** of the NUS
write:

| First byte | Constant | Action |
|---|---|---|
| `FCT_REQ_SATRT_TEST` | — | `Fct_StartLoop()` (sic — typo is in the original) |
| `FCT_REQ_HEADER` | — | `Fct_PutReq(&data[1], len - 1)` |
| `FCT_REQ_STOP_TEST` | — | `Fct_StopLoop()` |
| **`CFG_REQ_HEADER`** | **`0xDD`** | `Cfg_PutReq(&data[1], len - 1)` |

### Request frame

```
┌────────┬────────┬───────────────┐
│ 0xDD   │ type   │ params (≤2 B) │
└────────┴────────┴───────────────┘
```

| Type | Constant | Params | Board |
|---|---|---|---|
| `0x01` | `CFG_REQ_MOTION_DATA_GET` | — | both |
| `0x02` | `CFG_REQ_MOTION_DATA_SET` | `[index][value]`, exactly 2 | both |
| `0x03` | `CFG_REQ_BATT_VOLT_GET` | — | both |
| `0x04` | `CFG_REQ_CALLING` | — | **XH_5102 only** (`#if defined(BOARD_XH_5102)`) |
| `0xFF` | `CFG_REQ_NONE` | — | sentinel, not a command |

Requests are queued (depth 1) and drained by a 100 ms repeating timer
(`CFG_REQ_LOOP_TIME_MS`). `Cfg_PutReq()` drops the frame if `cfgLoopStart` is
false — the loop starts on `BLE_GAP_EVT_CONNECTED`.

### Response frame

```c
typedef struct __attribute__((packed)) {
    uint8_t header;    /* 0xDD */
    uint8_t type;      /* echoes the request type */
    uint8_t errCode;
    union {
        struct { uint8_t data[16]; } motion;   /* CFG_REQ_MOTION_DATA_GET */
        struct { uint8_t voltHigh, voltLow; } batt;  /* CFG_REQ_BATT_VOLT_GET */
    } para;
} stCfgRespPkt_t;
```

Transmitted length depends on the command:

| Command | Length sent | Bytes |
|---|---|---|
| `MOTION_DATA_GET` | `3 + 16` | `19` |
| `MOTION_DATA_SET` | `3` | header/type/err only |
| `BATT_VOLT_GET` | `3 + 2` | `5` |
| `CALLING` | `3` | |
| unknown type | `3` | `errCode = 0xDD` |

Battery voltage is big-endian (`voltHigh` = `mV >> 8`, `voltLow` = `mV & 0xFF`).

| Response code | Value |
|---|---|
| `CFG_RESP_SUCCESS` | `0xAA` |
| `CFG_RESP_SENSOR_FAIL` | `0xBB` |
| `CFG_RESP_PARAM_ERROR` | `0xCC` |
| `CFG_RESP_UNKNOWN_CMD` | `0xDD` |

Note `CFG_RESP_UNKNOWN_CMD` and `CFG_REQ_HEADER` are both `0xDD`, in different
positions. Not a bug, but easy to confuse while porting.

### `MOTION_DATA_SET` has immediate side effects

```
index 0x00, value 1 → persist data[0]=1, Led_Ctrl(LED_ACT_YELLOW_TWINKLE)
index 0x00, value 0 → persist data[0]=0, Led_Ctrl(LED_ACT_NONE)
index 0x01, value 1 → persist data[1]=1, Motor_Ctrl(MTR_ACT_SHORT_VIBRATE)
index 0x01, value 0 → persist data[1]=0, Motor_Ctrl(MTR_ACT_NONE)
```

Any other index: no store, no actuation, but still `CFG_RESP_SUCCESS`. A
non-2-byte parameter returns `CFG_RESP_PARAM_ERROR` and does not persist.

---

## 4. Security finding: the same unchecked-length bug, twice more

[`aps-protocol-spec.md` §7](aps-protocol-spec.md) documents an unchecked `memcpy`
in `Aps_PutCmd()`. The identical pattern appears twice more, both reachable over
NUS. **This is a systemic input-validation gap across all three command
entry points, not an isolated defect.**

### `Cfg_PutReq()` — `app_config.c:237`

```c
#define CFG_REQ_PARA_MAX_LEN 2
typedef struct { eCfgReqType_t type; uint8_t paraLen; uint8_t para[2]; } stCfgReqPkt_t; /* sizeof 8 */
...
stCfgReqPkt_t req = { .type = type, .paraLen = len - 1 };
memcpy(req.para, pBuf + 1, req.paraLen);     /* no bound against sizeof(req.para) */
```

`BLE_NUS_MAX_DATA_LEN` is `247 - 3` = **244**. A 244-byte NUS write beginning with
`0xDD` yields `len = 243` and `paraLen = 242`, so `memcpy` writes **242 bytes into
a 2-byte array** — roughly 234 bytes past the end of an 8-byte stack structure.

### `Fct_PutReq()` — `app_factory.c:431`

```c
#define FCT_REQ_PARA_MAX_LEN 20
...
.paraLen = len - 1,
memcpy(req.para, pBuf + 1, req.paraLen);
```

Same shape, 20-byte destination, overflow up to ~222 bytes.

### Reachability

`Cfg_StartLoop()` is called from `BLE_GAP_EVT_CONNECTED` (`app_ble.c:460`), so the
`Cfg` path is armed on every connection. All IPS and NUS characteristics are
`SEC_OPEN` and the device requires no pairing, bonding or encryption. Any device
in radio range that can complete a connection can reach all three paths.

`sn_burn()` in the same file is *correctly* guarded
(`else if (len != FCT_SN_CODE_SIZE) → PARAM_ERROR`), which shows the author knew
the pattern — the three queue-put functions simply missed it.

### Fix pattern for the port

Bound-check at every deserialisation boundary, before the copy:

```c
if (len < 1 || (size_t)(len - 1) > sizeof(req.para)) {
    return;   /* or respond with the protocol's parameter-error code */
}
```

In Zephyr, the GATT write callbacks receive `len` and `offset` and must validate
both — `offset` is a second, independent overflow vector that the SoftDevice
helper library used to handle. Do not assume the framework checks it.

### Recommended handling

1. Treat these as findings against the **currently deployed** firmware, not just
   the port. Deployed devices are affected today.
2. Report to the upstream maintainer (Ribin Huang / Fractal Auto Technology)
   before this fork is published, so a fix can precede public disclosure.
3. Fix all three in Phase 4 and add `ztest` cases that feed maximum-length and
   malformed frames to each parser.
4. Have the fixes reviewed by someone other than the author of the port, per the
   safety notice.
