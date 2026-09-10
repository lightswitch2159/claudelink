# RileyLink APS Protocol Specification (Derived from Source)

**Phase 0, task 8.** Frame structures extracted from `project/app/src/app_aps.c`.

Protocol identity: **`subg_rfspy 2.2`** (returned by `CMD_GET_VER`).
Transport: the IPS **Data** characteristic — see
[`gatt-service-spec.md`](gatt-service-spec.md) for the write/notify handshake.

> **All multi-byte fields are big-endian on the wire.** The firmware byte-swaps
> them in place on receipt via `Kit_ReverseTwoBytes` / `Kit_ReverseFourBytes`.
> This is the single easiest thing to get wrong in the port.

---

## 1. Outer framing

### Request (client → device, written to Data)

```
┌────────┬────────┬───────────────────────────┐
│ byte 0 │ byte 1 │ bytes 2..N                │
│ length │ opcode │ command parameters        │
└────────┴────────┴───────────────────────────┘
```

`byte 0` is the count of all following bytes, i.e. `1 + len(parameters)`.

Validation in `Aps_PutCmd()` — a frame is **silently dropped** (no error response)
if any of these hold:

```c
len == 0
pBuf[0] != len - 1     /* declared length must match actual */
len == 1
```

### Response (device → client, read from Data)

Two shapes:

| Shape | Bytes | Used for |
|---|---|---|
| Status only | `[code]` | Acknowledgements and all errors |
| Data | `[0xDD] [payload…]` | Any command returning data |

`0xDD` (`RESPONSE_CODE_SUCCESS`) prefixes every data-bearing response.

| Code | Value | Meaning |
|---|---|---|
| `RESPONSE_CODE_SUCCESS` | `0xDD` | Success |
| `RESPONSE_CODE_RX_TIMEOUT` | `0xAA` | Listen window expired |
| `RESPONSE_CODE_CMD_INTERRUPTED` | `0xBB` | Aborted (BLE dropped to advertising) |
| `RESPONSE_CODE_PARAM_ERROR` | `0x11` | Bad parameter |
| `RESPONSE_CODE_UNKNOWN_COMMAND` | `0x22` | Defined but **never sent** — unknown opcodes are logged and dropped |

Maximum response payload: `BLE_RESPONSE_MAX_LEN` = **150** bytes.

---

## 2. Command set and dispatch

| Opcode | Command | Params | Response | Executed |
|---|---|---|---|---|
| `0x01` | `CMD_GET_STATE` | — | `[0xDD]"OK"` | Queue |
| `0x02` | `CMD_GET_VER` | — | `[0xDD]"subg_rfspy 2.2"` | Queue |
| `0x03` | `CMD_GET_PKT` | `stCmdGetPkt_t` | `[0xDD]stSubgRespPkt_t` or `0xAA`/`0xBB` | Queue |
| `0x04` | `CMD_SEND_PKT` | `stCmdSendPkt_t` | `[0xDD]` | Queue |
| `0x05` | `CMD_SEND_AND_LISTEN` | `stCmdSendAndListen_t` | `[0xDD]stSubgRespPkt_t` or `0xAA`/`0xBB` | Queue |
| `0x06` | `CMD_UPDATE_REG` | `[addr][value]…` | `[0xDD]` | **Immediate — see §5** |
| `0x07` | `CMD_RESET` | — | *none* — opcode is **not handled** | — |
| `0x08` | `CMD_LED` | — | `[0xDD]` | Queue (no-op) |
| `0x09` | `CMD_READ_REG` | `[addr]` | `[0xDD][value]` | Queue |
| `0x0A` | `CMD_SET_MODE_REG` | — | `[0xDD]` | Queue (no-op) |
| `0x0B` | `CMD_SET_SW_ENCODING` | `[type]` | `[0xDD]` or `0x11` | Queue |
| `0x0C` | `CMD_SET_PREAMBLE` | `[hi][lo]` | `[0xDD]` | Queue |
| `0x0D` | `CMD_RESET_RADIO_CFG` | — | `[0xDD]` | Queue |
| `0x0E` | `CMD_GET_STATISTICS` | — | `[0xDD]stCmdGetStatisticsRespPkt_t` | Queue |

`CMD_RESET` (`0x07`) has **no case in the dispatch switch** — it falls to `default`,
is logged as unknown, and produces no response at all. The client will time out.
Reproduce this behaviour (or fix it deliberately and tell the app owner).

`CMD_LED` (`0x08`) and `CMD_SET_MODE_REG` (`0x0A`) acknowledge success and do
nothing.

Queue depth is **1** (`APS_CMD_QUEUE_SIZE 2` with a FIFO that reserves a slot).
Commands are drained by a 10 ms repeating timer (`APS_CMD_LOOP_TIME_MS`).

---

## 3. Wire structures

All are `__attribute__((packed))`. Multi-byte fields big-endian.

### `CMD_GET_PKT` (0x03) parameters — 5 bytes

```c
typedef struct __attribute__((packed)) {
    uint8_t  listenChan;      /* accepted, never used */
    uint32_t listenTimeout;   /* BE, milliseconds; 0 = wait forever */
} stCmdGetPkt_t;
```

### `CMD_SEND_PKT` (0x04) parameters — 6-byte header + payload

```c
typedef struct __attribute__((packed)) {
    uint8_t  sendChan;        /* accepted, never used */
    uint8_t  repeatCnt;
    uint16_t repeatIntvl;     /* BE, ms between repeats */
    uint16_t preambleExtend;  /* BE */
    uint8_t  sendPkt[];       /* flexible array */
} stCmdSendPkt_t;
```

### `CMD_SEND_AND_LISTEN` (0x05) parameters — 12-byte header + payload

```c
typedef struct __attribute__((packed)) {
    uint8_t  sendChan;        /* accepted, never used */
    uint8_t  repeatCnt;
    uint16_t repeatIntvl;     /* BE */
    uint8_t  listenChan;      /* accepted, never used */
    uint32_t listenTimeout;   /* BE, ms */
    uint8_t  retryCnt;        /* decremented in place on each retry */
    uint16_t preambleExtend;  /* BE */
    uint8_t  sendPkt[];
} stCmdSendAndListen_t;
```

Retry loop: on `RX_TIMEOUT` with `retryCnt > 0`, resend (with `repeatCnt` forced
to `0`) and listen again, decrementing `retryCnt`. Worst-case blocking time is
`(retryCnt + 1) × listenTimeout` — see §6.

### Packet response payload — 2-byte header + decoded packet

```c
typedef struct __attribute__((packed)) {
    uint8_t rssi;             /* CC111x-encoded, see §4 */
    uint8_t pktCnt;           /* low byte of the RX packet counter */
    uint8_t pkt[107];
} stSubgRespPkt_t;
```

Transmitted length is `2 + decodedLength`, so `pkt` is truncated to the real
packet length. The response on the wire is `[0xDD][rssi][pktCnt][packet…]`.

### `CMD_GET_STATISTICS` (0x0E) response — 20 bytes

```c
typedef struct __attribute__((packed)) {
    uint32_t updTime;            /* BE, ms = apsCmdLoopCnt × 10 */
    uint16_t rxOverflowCnt;      /* hardcoded 0 */
    uint16_t rxFifoOverflowCnt;  /* hardcoded 0 */
    uint16_t pktRxCnt;           /* BE */
    uint16_t pktTxCnt;           /* BE */
    uint16_t crcFailCnt;         /* hardcoded 0 */
    uint16_t spiSyncFailCnt;     /* hardcoded 0 */
    uint16_t placeholder0;       /* 0 */
    uint16_t placeholder1;       /* 0 */
} stCmdGetStatisticsRespPkt_t;
```

Only `updTime`, `pktRxCnt` and `pktTxCnt` carry real data. Note `updTime` is
derived from the loop counter, so it counts time since `Aps_StartLoop()`, not
since boot, and stops advancing when the loop is stopped.

### Internal queue element (not on the wire)

```c
typedef struct {              /* NOT packed — has padding */
    eCmdTypes_t cmd;          /* enum → 4 bytes */
    int8_t      rssi;
    uint16_t    pktLen;
    uint8_t     pkt[123];     /* APS_MAX_PARA_LEN(16) + SUBG_MAX_PKT_LEN(107) */
} stApsReqPkt_t;              /* sizeof = 132 */
```

---

## 4. Register interface

### `CMD_UPDATE_REG` (0x06) — `[addr][value]`

Frames carry 2 bytes (AndroidAPS) or 10 bytes (Loop); only the first two are read.
Requires `len >= 2`, otherwise returns **without any response**.

| Addr | Effect |
|---|---|
| `0x02` | `usePktLen = 1`; set fixed RX payload length to `value` |
| `0x09`–`0x0B` | Store into `subgFreqReg[addr - 0x09]`, then recompute and apply frequency |
| `0x0C` | If `value == 0x59` and mode ≠ MINIMED_WWL: switch to MINIMED_WWL and reconfigure the radio |
| other | Logged and ignored |

All paths that reach the end return `0xDD`.

### `CMD_READ_REG` (0x09) — `[addr]`

Requires `len >= 1` or returns without response.
Addresses `0x09`–`0x0B` return the stored frequency byte; **every other address
returns the constant `0x5A`** (a stub, not a real register read).

### Frequency computation

```c
regValue = (subgFreqReg[0] << 16) | (subgFreqReg[1] << 8) | subgFreqReg[2];
freqHz   = (regValue * 24000000) >> 16;     /* RILEY_LINK_FXOSC = 24 MHz */
```

Default `subgFreqReg = {0x12, 0x14, 0x83}` → `0x121483` → **433.92 MHz**.

The resulting frequency selects the sub-GHz mode, and an out-of-band frequency is
logged and **discarded** — the radio keeps its previous setting:

| Band | Range | Mode set |
|---|---|---|
| 868 MHz | 866–870 MHz | `SUBG_MODE_MINIMED_WWL` |
| 916 MHz | 914–918 MHz | `SUBG_MODE_MINIMED_NAS` |
| 433 MHz | 431–435 MHz | `SUBG_MODE_OMNIPOD` |

### RSSI encoding

The device emulates a TI CC111x so RileyLink clients can interpret it:

```c
cc111xRssi = (uint8_t)((rssi_dBm + 73) * 2);
```

Preserve exactly, including the `uint8_t` truncation for strong or weak signals.

---

## 5. Software encoding

`CMD_SET_SW_ENCODING` (0x0B) selects the line coding applied to every subsequent
TX and RX payload:

| Value | Encoding | Encode length | Decode |
|---|---|---|---|
| `0` | `ENCRYPT_NONE` | `len` (memcpy) | `len` |
| `1` | `ENCRYPT_MANCHESTER` | `len × 2` | `decode_manchester()` |
| `2` | `ENCRYPT_4B6B` | `encode_4b6b()` | `decode_4b6b()` |

Any other value returns `0x11` and leaves the setting unchanged.
`CMD_RESET_RADIO_CFG` resets it to `ENCRYPT_NONE` and the preamble to `0`.

Despite the `ENCRYPT_*` naming, this is **line coding, not encryption**. Port
`4b6b.c` and `manchester.c` verbatim and cover them with `ztest` unit tests
(round-trip plus known-vector) — they implement medical device wire formats and a
one-bit error changes what the pump receives.

### Trailing-zero truncation — subtle and load-bearing

Both `cmd_send_pkt` and `cmd_send_and_listen` do this **before** encoding:

```c
if ((sendPktLen > 0 && p->sendPkt[sendPktLen - 1] == 0)
    && (Subg_GetMode() != SUBG_MODE_OMNIPOD)) {
    sendPktLen--;
}
```

A single trailing zero byte is stripped for Minimed, kept for Omnipod. Only one
byte, only if it is the last. Get this wrong and Minimed packets are one byte too
long while Omnipod packets are silently corrupted.

---

## 6. Concurrency and timing — the real porting problems

These are architectural, not mechanical. They are the reason Workstream 4 is
riskier than "port the business logic" suggests.

### 6.1 `CMD_UPDATE_REG` runs in BLE callback context, unsynchronised

Every other command is queued and executed from the 10 ms timer. `CMD_UPDATE_REG`
is not:

```c
if (cmd == CMD_UPDATE_REG) {
    cmd_update_reg(pBuf + 2, len - 2);   /* immediate, in the BLE event handler */
    return;
}
```

It can call `Subg_SetMode()` and `Subg_CfgRf()`, which drive the RFM69 over SPI —
**while the APS loop may be mid-SPI-transaction for a different command**. There
is no lock. Today this is mitigated only by the legacy priority arrangement
(SoftDevice event handling vs. `app_timer` SWI). Under Zephyr the two contexts are
genuinely concurrent threads, and this becomes a real data race on the SPI bus.

**The port must add explicit mutual exclusion around all RFM69 access**
(`k_mutex`, or serialise everything onto one work queue). Do not port this
structure as-is.

### 6.2 `Subg_GetPkt()` blocks for the entire listen timeout

`app_subg.c` implements RX with unbounded busy-wait polling and blocking delays:

```c
while (!Rf69_IsFifoFull(RF69_DEV_FREQ433)) { ... }
while (Rf69_IsFifoEmpty(dev)) { ... }
Kit_DelayMs(1);  Kit_DelayUs(200);
```

`listenTimeout` is a client-supplied `uint32_t` in milliseconds, and
`CMD_SEND_AND_LISTEN` multiplies it by `retryCnt + 1`. A single command can
occupy the CPU for **seconds**.

Under the legacy design this runs in `app_timer`'s SWI handler, below the
SoftDevice's radio interrupts, so BLE survives. Under Zephyr, the SoftDevice
Controller still preempts (it is a high-priority ISR), but a multi-second
busy-wait inside a `k_timer` handler or the system workqueue **starves the
Bluetooth host thread**, stalling ATT/GATT. Symptom: the connection stays up but
the device stops answering.

**Recommended port:** give the sub-GHz path its own cooperative thread at a
priority below the BT RX thread, replace `Kit_DelayMs`/`Kit_DelayUs` with
`k_sleep`/`k_busy_wait` as appropriate, and use the RFM69 DIO lines as GPIO
interrupts with `k_sem` handoff instead of polling the FIFO. Benchmark against the
legacy timing with a logic analyser before trusting it (Workstream 6).

The abort condition is `Ble_GetState() == BLE_STATE_ADV`, i.e. the RX loop breaks
when the BLE link drops. That cross-layer coupling has to be preserved — it is
what produces `RESPONSE_CODE_CMD_INTERRUPTED`.

### 6.3 Packed-struct member addresses will not compile cleanly under GCC

The parsing code takes the address of unaligned packed members and casts:

```c
Kit_ReverseFourBytes((uint32_t *)&p->listenTimeout);
```

In `stCmdSendAndListen_t`, `listenTimeout` sits at offset 4 of a packed struct
overlaid on a byte buffer — no alignment guarantee. This is undefined behaviour;
GCC (the NCS toolchain) emits `-Waddress-of-packed-member` and is free to
generate an aligned load. ARMCC5 tolerated it.

**Do not port these casts.** Replace the overlay-and-byte-swap approach with
explicit byte extraction, e.g. `sys_get_be32(&buf[4])` from
`<zephyr/sys/byteorder.h>`. This is mechanical, but it must be done at every one
of the six swap sites or the port will misparse commands in ways that only appear
at runtime.

---

## 7. Security finding in the existing firmware

> Reported here because Phase 0 is the right place to catch it, and because the
> port should not carry it forward. This is a defect in the **currently shipping**
> firmware, not something introduced by the migration.

**Heap/stack buffer overflow in `Aps_PutCmd()`, reachable from an unauthenticated
BLE write.**

```c
/* app_aps.c */
uint8_t pkt[APS_MAX_PARA_LEN + SUBG_MAX_PKT_LEN];   /* 16 + 107 = 123 bytes */
...
if (pBuf[0] != len - 1 || len == 1) return;         /* only checks self-consistency */
stApsReqPkt_t req = { .cmd = cmd, .pktLen = len - 2, .rssi = rssi };
memcpy(req.pkt, pBuf + 2, req.pktLen);              /* no bound against sizeof(req.pkt) */
```

The Data characteristic accepts writes up to `BLE_IPS_MAX_DATA_CHAR_LEN` = **150**
bytes with `is_var_len = true`. A well-formed 150-byte write (`pBuf[0] = 149`)
passes validation and yields `pktLen = 148`, so `memcpy` writes **148 bytes into a
123-byte array — a 25-byte overflow** past the end of a 132-byte stack structure.

Reachability: the IPS characteristics are all `SEC_OPEN`, and the device requires
no pairing, bonding, or encryption. Any device in radio range that can connect can
reach this path.

**Fix in the port** (and consider whether the legacy firmware warrants a patch
release independently of the migration):

```c
if (len < 2 || (size_t)(len - 2) > sizeof(req.pkt)) {
    return;   /* or respond RESPONSE_CODE_PARAM_ERROR */
}
```

Related, lower-severity observations in the same area:

- `cmd_update_reg` and `cmd_read_reg` return **without any response** on a short
  frame, so a malformed command hangs the client until it times out.
- Malformed frames are dropped silently rather than answered with
  `RESPONSE_CODE_PARAM_ERROR`, which is defined but never sent.
- `stSubgRespPkt_t subgResp;` is uninitialised, though the code only transmits
  `2 + decodedLength` bytes, so no uninitialised memory is currently disclosed.
  Zero-initialise it anyway; the invariant is one edit away from breaking.

Given the device's role in the insulin pump communication path, these should be
reviewed by someone other than whoever writes the port, and the overflow should
be validated on bench hardware before and after the fix.
