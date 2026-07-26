# K-Line KWP2000 ECU Interface

## What This Project Is

A Raspberry Pi Pico (RP2040) firmware that communicates with Bosch ME7.5 automotive ECUs over the K-Line bus using KWP2000 (ISO 14230). The end goal is injecting a custom fast-logging handler into ECU RAM to enable high-speed multi-variable datalogging.

A companion project at `../../misc/logger_handler/` contains a 236-byte C166 assembly handler (`fastlogging_ramhandler`) whose source describes a **0xBE / 0xFE** service. **That is not the binary this project actually uses.** `load-handler`/`start-logging` inject `handler_setzi.bin` (582 bytes), a different build of the same idea that responds on **0xB7 / 0xF7**. The 0xBE references throughout the older docs are for the other variant — see `KWP2000_COMMAND_REFERENCE.md`.

**Status (2026-07-26): fast logging works end to end.** `connect` -> `start-logging` -> `read-log` returns live multi-variable data (verified: battery voltage matched the bench supply, coolant temp matched room temperature). See "What Was Wrong" below for the bugs that were blocking it.

## Architecture

Dual-core design on the RP2040:
- **Core 0** (`ecu_state_machine.c`): K-Line init, KWP2000 packet send/receive, heartbeat keep-alive. Runs the ECU connection state machine.
- **Core 1** (`console.c`): USB serial console — text parsing and printing only.
- **Command layer** (`command.c`): the single place that turns "do a thing to the ECU" into core 0 messages (handler loading, the `start-logging` sequence, raw frames). It knows nothing about how a command arrived or where its output goes — a frontend supplies a `CommandHost` (`report` + `pump` callbacks). The RPi 5 host link becomes a second frontend against this same interface.
- **Inter-core comms** (`ring_buffer.c`): lock-free SPSC ring buffer, 128 messages of 128 bytes each. Exactly one core pushes and one pops a given buffer; `__dmb()` barriers order the payload copy against the index update. Message types live in `messages.h`.
- **Logging** (`log.c`): core 0 must never `printf` — stdio_usb is not multicore-safe and can block for milliseconds mid-transaction. Core 0 calls `klog()`, which queues the line for core 1 to print and *drops* it if the queue is full (reporting the gap), so the K-line path never waits on the console.

## Hardware

| Pin/Setting | Value |
|-------------|-------|
| K-Line RX | GPIO 18 (PIO0) |
| K-Line TX | GPIO 15 (PIO1) |
| Baud rate | 10,400 bps |
| UART format | 8N1 via PIO (not hardware UART) |
| Host interface | USB serial (stdio_usb) |

## Source Files

```
src/
  main.c                 - Entry point, ring buffer init, launches core1
  uart.c / uart.h        - PIO UART init and byte-level I/O
  uart_rx.pio            - PIO state machine for 8N1 TX/RX
  kline.c / kline.h      - 5-baud slow init (0x88 wakeup), sync/key byte handshake
  kwp2000.c / kwp2000.h  - Packet framing, checksum, send/receive, DTC parsing
  ecu_state_machine.c/h  - Core 0 state machine (IDLE -> CONNECTING -> CONNECTED)
  command.c / command.h  - Frontend-agnostic command layer (CommandId -> messages)
  console.c / console.h  - Core 1 USB text console; parses text into Commands
  ring_buffer.c / ring_buffer.h - Inter-core message queue (SPSC, lock-free)
  messages.h             - Inter-core message type enum
  log.c / log.h          - klog(): core 0 logging via the message queue
```

Binary blobs linked into firmware:
- `handler.bin` / `handler_setzi.bin` - ECU RAM handler binaries (converted to ELF objects via objcopy at build time)

## Build

```bash
./build.sh
# Output: build_linux/k_line_kwp.uf2
```

Requires Pico SDK at `$PICO_SDK_PATH` (default: `/opt/pico-sdk`). Uses CMake + GNU ARM toolchain. SDK version 2.1.1.

## KWP2000 Packet Format

```
[Length] [Service ID] [Data...] [Checksum]
```
- Length = 1 + number of data bytes (includes SID in the count)
- Checksum = Length + SID + sum of all data bytes
- ECU echoes all sent bytes before responding; the response parser skips echo bytes
- Positive response SID = request SID + 0x40
- Negative response = `0x7F [rejected SID] [NRC]`

## K-Line Init Sequence

1. Line idle 1.5s (high)
2. Send 0x88 at 5 baud (200ms/bit) - programming mode wakeup for ME7.5
3. Receive sync byte 0x55 from ECU
4. Receive two key bytes
5. Send complement (0xFF - key_byte_2)
6. Receive address byte: 0xEE = programming mode, 0xCC = KWP2000 mode

## Console Commands

```
connect                  - Initiate K-Line connection (5-baud init)
disconnect               - Close ECU connection
ecu-id                   - Read ECU ID (SID 0x1A, param 0x9B)
read-dtcs                - Read + decode fault codes (SID 0x18, params 0x00 0xFF 0x00)
clear-dtcs               - Clear fault codes (SID 0x14, params 0xFF 0x00)
diag-session             - Start diagnostic session (SID 0x10, param 0x86)
load-handler             - Write handler_setzi.bin to ECU RAM at 0x387A00
start-logging            - Full fast-logging setup: heartbeat, load, redirect, init, set vars
read-log                 - Sample the logged variables (bare 0xB7 -> 0xF7)
fill-distributor-table   - (PRJ variant only) fill 48-entry table with handler addr
cmd:XXXX                 - Send raw hex KWP2000 command (e.g., cmd:1A9B for ECU ID)
raw:XXXX                 - Same as cmd: but dumps the unparsed reply bytes (debugging)
heartbeat:MS             - Set keep-alive interval in ms (use ~2000 during logging)
baud:N                   - Set K-line bit rate at runtime (10400 is the working rate)
```

### Fast logging: the normal flow

```
connect          # 5-baud init
start-logging    # injects handler and sets everything up (see below)
read-log         # sample; repeat as needed
```

`start-logging` runs, in order: `heartbeat:2000` -> `diag-session` -> `load-handler`
-> redirect `0xE228` -> init trigger (`0x3E`) -> set variables (`0xB7` + list). Keeping
the heartbeat short is essential — the KWP session times out in ~3-5s of silence, and a
dropped session is unrecoverable without an **ECU power cycle**. Connect once and keep
sampling; do not disconnect/reconnect mid-session.

## KWP2000 Services Used

| SID  | Name | Data sent | Purpose |
|------|------|-----------|---------|
| 0x10 | StartDiagnosticSession | 0x86 | Enter manufacturer-specific session |
| 0x14 | ClearDTCs | 0xFF 0x00 | Clear all stored fault codes |
| 0x18 | ReadDTCByStatus | 0x00 0xFF 0x00 | Read all stored fault codes |
| 0x1A | ReadECUIdentification | 0x9B | Get ECU identification string |
| 0x23 | ReadMemoryByAddress | [3-byte addr] [1-byte size] | Read raw ECU memory |
| 0x3D | WriteMemoryByAddress | [3-byte addr] [1-byte size] [data] | Write to ECU memory |
| 0x3E | TesterPresent | (none) | Keep-alive heartbeat (5s default) |

### Raw command format for cmd:

- **Write 4 bytes to 0x387800**: `cmd:3D38780004DEADBEEF`
  - `3D` = WriteMemoryByAddress, `387800` = address, `04` = size, `DEADBEEF` = data
- **Read 4 bytes from 0x387800**: `cmd:2338780004`
  - `23` = ReadMemoryByAddress, `387800` = address, `04` = size

## Handler Injection Flow (working path)

1. `load-handler` writes `handler_setzi.bin` (582 bytes) to ECU RAM at **0x387A00** in 8-byte chunks via SID 0x3D. The blob is self-contained: first 192 bytes are the service table (48 entries pointing at `0x387AC6`), the rest is code.
2. **Redirect** the dispatcher pointer at **0xE228** to `0x387A00` (`cmd:3D00E22804003AE100`). Without this the ECU keeps using its original BootRom table and the handler is never reached. This is the step that was missing for a long time.
3. **Init trigger:** the first service call after the redirect makes the handler copy the original table into its own; `start-logging` sends `0x3E` for this (it returns SNS itself — expected).
4. **Set variables** with `0xB7` + format byte + address list, then sample with bare **0xB7** (response **0xF7**).

The PRJ variant (`handler.bin` at `0x387ACC` + `fill-distributor-table`) is a separate,
older path and is **not** what works today. Its `fill_distributor_table` also writes the
handler pointer in the wrong byte order (`00 38 7A CC` instead of little-endian
`CC 7A 38 00`) — left as-is since that path is unused.

## What Was Wrong (2026-07 debugging session)

Fast logging had never worked reliably. The blocking bugs, in order of impact:

1. **Chunk offset truncation** (`dashboard.c`): `uint8_t data_offset = chunk_number * 8`
   wrapped at 256, so any handler over 256 bytes had its first 256 bytes written three
   times and the rest never sent. `handler_setzi.bin` (582 B) was always half-garbage;
   `handler.bin` (236 B) squeaked under the limit, which is why *it* seemed to work.
2. **Half-duplex echo FIFO overflow** (`kwp2000.c`): the whole frame was transmitted
   before reading the echo, overflowing the ~9-byte PIO RX FIFO and dropping echo bytes
   on any frame >9 bytes. This desynced the reply parser and made the handler look like
   it was rejecting valid `0xB7` requests. Now the echo is read/verified inline per byte.
3. **Missing service-table redirect**: loading the handler is useless until `0xE228` is
   repointed (step 2 above). The old flow never did this.
4. **Wrong service ID**: the setzi handler answers on **0xB7**, not the 0xBE the docs said.
5. **Heartbeat killed the link on negative responses**: a `0x7F` reply still proves the
   ECU is alive, but the old code treated any non-OK heartbeat as a disconnect — tearing
   down the connection during the post-redirect window where SNS is expected.
6. **PIO program re-added every connect**: `uart_pio_init_tx/rx` called `pio_add_program`
   on each connect, exhausting PIO instruction memory after ~5-8 connects and wedging the
   Pico (looked like an ECU failure). Now loaded once.
7. **"ECU crashes" were usually session timeouts**: with the heartbeat parked, the KWP
   session dropped in the gaps between test commands. Reframed as a keepalive problem, not
   ECU instability. `setTimingParams` (`83030001001400`) genuinely does wedge the session
   though — do not send it.

## Logging Variable Format

**0xB7 handler (working, `handler_setzi.bin`):** the request is `0xB7` + one format
byte (`0x03`, meaning TBD) + one 3-byte big-endian address per variable. The reply
(`0xF7`) packs the values in request order. The default set in `start-logging` is
nmot, ub, wped, plsol, tmot — addresses/scaling from `me7log/ecu_files/8N0906018BP 0002.ecu`.

**Variable size is encoded in the address.** Bit 0x40 of the first address byte means
"2-byte variable"; without it the variable is 1 byte. Cross-checked against the `.ecu`
file: `78 4B 12` is `tats_w` at 0x384B12 size 2, `40 F9 A4` is `mshfm_w` at 0x00F9A4
size 2, while `38 0A 32` is `tmot` at 0x380A32 size 1. So **the reply length is
computable from the request alone** — the firmware can frame a sample without knowing
anything about scaling, which is what keeps decode on the host side. The leading `0x03`
is *not* a variable count: it is identical across every var-list length in `me7log`.

**0xBE handler variant (not used):** each entry is 4 bytes `[length][3-byte addr]`,
array terminated by `0x00`.

## Inter-Core Message Types

```c
MSG_ECU_DATA         = 0x01  // ECU response data (core0 -> core1)
MSG_COMMAND          = 0x03  // KWP2000 command to send (core1 -> core0)
MSG_ACK              = 0x04  // Command succeeded
MSG_NACK             = 0x05  // Command failed (data[0] = error code)
MSG_CONNECT_ECU      = 0x06  // Request connection
MSG_DISCONNECT_ECU   = 0x07  // Request disconnection
MSG_ECU_DISCONNECTED = 0x08  // Connection lost (unsolicited)
MSG_SET_HEARTBEAT    = 0x09  // Set heartbeat interval (4 bytes, big-endian ms)
MSG_RAW_COMMAND      = 0x0A  // Send frame, dump every reply byte verbatim (raw:)
MSG_SET_BAUD         = 0x0B  // Change K-line bit rate (4 bytes, big-endian)
MSG_LOG              = 0x0C  // Text line from core0 for core1 to print
```

## ResponseStatus (kwp2000.h)

`RESPONSE_NEGATIVE` (ECU replied 0x7F) is distinct from `RESPONSE_ERROR` (timeout /
malformed). A negative response means the ECU is alive and rejected the request, so the
heartbeat must **not** disconnect on it — only a true `RESPONSE_ERROR` indicates a dead link.

## Key Constants

| Constant | Value |
|----------|-------|
| SERIAL_BAUD | 10,400 |
| PIO_RX_PIN | 18 |
| PIO_TX_PIN | 15 |
| MAX_DATA_SIZE | 255 |
| MAX_RESPONSE_SIZE | 80 |
| RING_BUFFER_SIZE | 128 |
| MAX_MESSAGE_SIZE | 128 |
| DEFAULT_HEARTBEAT_INTERVAL_MS | 5000 |

## ECU Target

- **Platform**: Bosch ME7.5 (Infineon C167CR, 16-bit, segmented memory)
- **Actual part number on the bench**: `8N0906018BP 0002` (confirmed via `ecu-id`). The `8D0907551K` that appeared in older docs was a copy-paste from the `logger_handler` example and is **not** this ECU — the variable addresses/scaling come from `me7log/ecu_files/8N0906018BP 0002.ecu`.
- **RAM segment for handler**: #038h (addresses like 0x387A00)
- **Original 0xE228 value**: `D0 27 06 02` (far ptr 0x81A7D0, BootRom service table)
- **Init address byte 0xEE**: Programming mode (what we get)
- **Init address byte 0xCC**: KWP2000 mode
- **Bench note**: DTCs P0113/P0118/P0562/P0600/P0685/P0238 etc. are all "sensor/relay not connected" faults, normal on the bench; the injected handler works regardless of them.

## Full KWP2000 Service Reference

See `KWP2000_COMMAND_REFERENCE.md` for the complete ISO 14230 service ID table, negative response codes, and flash reprogramming sequence.
