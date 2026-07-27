# K-Line KWP2000 ECU Interface

## What This Project Is

A Raspberry Pi Pico (RP2040) firmware that communicates with Bosch ME7.5 automotive ECUs over the K-Line bus using KWP2000 (ISO 14230). The end goal is injecting a custom fast-logging handler into ECU RAM to enable high-speed multi-variable datalogging.

A companion project at `../../misc/logger_handler/` contains a 236-byte C166 assembly handler (`fastlogging_ramhandler`) whose source describes a **0xBE / 0xFE** service. **That is not the binary this project actually uses.** `load-handler`/`start-logging` inject `handler_setzi.bin` (582 bytes), a different build of the same idea that responds on **0xB7 / 0xF7**. The 0xBE references throughout the older docs are for the other variant — see `KWP2000_COMMAND_REFERENCE.md`.

**Status (2026-07-26): fast logging works end to end.** `connect` -> `start-logging` -> `read-log` returns live multi-variable data (verified: battery voltage matched the bench supply, coolant temp matched room temperature). See "What Was Wrong" below for the bugs that were blocking it.

## Architecture

Dual-core design on the RP2040:
- **Core 0** (`ecu_state_machine.c`): K-Line init, KWP2000 packet send/receive, heartbeat keep-alive. Runs the ECU connection state machine, and the handler injection sequence (`me7_handler.c`) — every step there waits for the ECU's own reply before the next request goes out, so the sequence is paced by the ECU rather than by fixed sleeps.
- **Core 1** (`host.c`): owns both frontends and the single drain of core 0's message queue. The queue is SPSC, so exactly one place may pop it; each message is then fanned out to every frontend. `console.c` prints it, `host_link.c` frames it for the RPi 5.
- **Command layer** (`command.c`): the single place that turns "do a thing to the ECU" into core 0 messages (handler loading, the `start-logging` sequence, raw frames). It knows nothing about how a command arrived or where its output goes — a frontend supplies a `CommandHost` (`report` + `pump` callbacks). The RPi 5 host link becomes a second frontend against this same interface.
- **Inter-core comms** (`ring_buffer.c`): lock-free SPSC ring buffer, 128 messages of 128 bytes each. Exactly one core pushes and one pops a given buffer; `__dmb()` barriers order the payload copy against the index update. Message types live in `messages.h`.
- **Logging** (`log.c`): core 0 must never `printf` — stdio_usb is not multicore-safe and can block for milliseconds mid-transaction. Core 0 calls `klog()`, which queues the line for core 1 to print and *drops* it if the queue is full (reporting the gap), so the K-line path never waits on the console.

## Hardware

| Pin/Setting | Value |
|-------------|-------|
| K-Line RX | GPIO 18 (PIO0) |
| K-Line TX | GPIO 15 (PIO1) |
| Baud rate | 10,400 bps for init, 57,600 during logging |
| UART format | 8N1 via PIO (not hardware UART) |
| Host interface | USB serial (stdio_usb) — dedicated USB peripheral, not a UART |
| RPi 5 link TX | GPIO 0 (uart0) |
| RPi 5 link RX | GPIO 1 (uart0) |
| RPi 5 link baud | 921,600 |

## Source Files

```
src/
  main.c                    - Entry point, ring buffer init, launches core1
  platform/
    uart_pio.c / .h         - PIO UART init and byte-level K-line I/O
    uart_rx.pio             - PIO state machine for 8N1 TX/RX
  kline/
    kline.c / .h            - 5-baud slow init (0x88 wakeup), sync/key handshake
    kwp2000.c / .h          - Packet framing, checksum, send/receive, DTC parsing
  ecu/
    ecu_state_machine.c / .h - Core 0 state machine (IDLE -> CONNECTING -> CONNECTED)
    me7_handler.c / .h      - Handler injection sequence + logged variable list
    logger.c / .h           - Free-running sampler (core 0)
  host/
    host.c / .h             - Core 1 entry: drains core 0's queue, fans out to frontends
    command.c / .h          - Frontend-agnostic command layer (CommandId -> messages)
    console.c / .h          - USB text console frontend
    host_link.c / .h        - RPi 5 binary link frontend (uart0)
    proto.c / .h            - COBS + CRC16 framing, and its self-test
  ipc/
    ring_buffer.c / .h      - Inter-core message queue (SPSC, lock-free)
    messages.h              - Inter-core message type enum
    log.c / .h              - klog(): core 0 logging via the message queue
```

`platform/uart_pio.*` is the bit-banged K-line UART. Phase 5 adds a real hardware
UART for the RPi 5 link — do not confuse the two.

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

### Connect only ever worked once per Pico boot (fixed 2026-07-26)

`wakeup_programming_mode()` bit-bangs the 5-baud `0x88` with `gpio_put()`, but the first
connect finishes by calling `uart_pio_init_tx()`, which hands that pin to the PIO. After
that `gpio_put()` on it is silently ignored, so every later wakeup transmitted **nothing**
— the ECU never saw `0x88`, never sent a sync byte, and looked dead. Every apparent
"the ECU needs a power cycle" was really this: reflashing reset the pin, which is why
recovery always seemed to require touching hardware.

`kline_init_connection()` now calls `uart_pio_release_tx_pin()` first, taking the pin back
to SIO before the wakeup. Reconnecting mid-boot works.

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
read-log                 - Sample the logged variables once (bare 0xB7 -> 0xF7)
stream-on[:MS]           - Start free-running sampling (default 100ms, 0 = full rate)
stream-off               - Stop free-running sampling
set-vars:HEX             - Replace the logged variable list (3-byte addresses)
proto-test               - Run the host-link framing self-test (no ECU needed)
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

`start-logging` runs, in order: `diag-session` -> `load-handler` -> redirect `0xE228`
-> init trigger (`0x3E`) -> set variables (`0xB7` + list), all on core 0 (`me7_handler.c`).
Keep the heartbeat short — the KWP session times out in ~3-5s of silence.

**A dropped session is recoverable** (corrected 2026-07-26): `connect` again and re-run
`start-logging`. The old claim that it needed an **ECU power cycle** was a misdiagnosis of
the TX-pin bug below — connect only ever worked once per Pico boot, so any reconnect
attempt failed and the ECU looked dead. Verified by recovering a wedged handler state with
the ECU continuously powered on its bench supply (a `DEADBEEF` written to `0x387800`
before the recovery was still there afterwards, proving its RAM was never cleared).

## Free-Running Sampler (`ecu/logger.c`)

`stream-on` starts core 0 issuing bare 0xB7 reads at its own pace and pushing each result
as `MSG_SAMPLE` — the host consumes what arrives instead of asking per sample, which is
what keeps the rate independent of host latency. Sample traffic doubles as the keep-alive,
so the heartbeat only fires when sampling is slower than the heartbeat interval or stopped.

**Measured on the bench (10400 baud, 5 variables):** ~20.6 samples/s at full rate, ~48ms
per sample. Only ~11ms of that is wire time; the rest is the ECU's own P2 turnaround, so a
faster K-line rate buys less than it looks like it should. `stream-on:200` holds a metronomic
200ms and reports rate on stop.

**Auto-recovery.** A handler that has lost its redirect answers 0xB7 with SNS (`7F B7 11`).
The logger recognises that signature and reinstalls, bounded to 3 attempts; if reinstalling
does not fix it, it asks the state machine for a fresh session (reconnect + reinstall) and
gives up cleanly if that fails too. Reinstalling here is safe precisely *because* 0xB7
returned SNS — the redirect is gone, so writes route through the ECU's own dispatcher (see
the reinstall hazard above). Verified by restoring the BootRom pointer mid-stream: sampling
resumed after a 4.4s gap with unbroken sequence numbers.

## Host Link (`host/proto.c`, `host/host_link.c`)

The RPi 5 link is uart0 on **GP0 (TX) / GP1 (RX) at 921600**, 8N1. Both boards are 3.3V,
so TX/RX cross-connect plus a common ground needs no level shifting. These pins are free:
the K-line is on GP15/GP18 via PIO, and the USB console uses the RP2040's dedicated USB
peripheral — **USB serial does not consume a UART**.

```
frame   = COBS(payload) 0x00
payload = [type][seq][data...][crc16:2]      CRC16-CCITT, poly 0x1021, init 0xFFFF
```

COBS removes 0x00 from the body, so a single zero byte delimits frames unambiguously. A
receiver that joins mid-stream or loses bytes only has to scan to the next zero to
resynchronise — no length field to misread, no escapes to get lost in. Corrupt frames fail
the CRC and are dropped.

| Type | Direction | Payload |
|------|-----------|---------|
| `SAMPLE 0x10` | to host | `[seq:2 BE][t_ms:4 BE][raw variable bytes]` |
| `EVENT 0x11` | to host | `[code][detail]` — see event codes below |
| `LOG 0x12` | to host | ASCII log line |
| `RESPONSE 0x13` | to host | raw KWP2000 response payload |
| `CMD 0x20` | from host | `[CommandId][args]` — scalars are 4 bytes BE |
| `ACK 0x21` | to host | `[CommandId][CommandStatus]` — receipt for a command frame **only** |

Event codes (`proto.h`): `0x01` link lost, `0x02` operation ok, `0x03` operation failed.
Operation outcomes are events rather than `ACK`s because `ACK` means "your command frame
arrived". Carrying both meanings on one type was genuinely ambiguous — a NACK was
indistinguishable from a receipt for `CommandId` 1 — which left a host unable to
sequence a startup.

**The other end of this link is `../pi-dash/`** — a Node bridge (`pi-dash/server/`)
that decodes these frames, applies `.ecu` scaling and feeds a PIXI dashboard. Its
`proto.ts` is a port of `src/host/proto.c` and is held to it by vectors generated from
this very C file (`pi-dash/server/test/vectorgen.c` links it). **If you change the framing
here, regenerate those vectors or the bridge will silently disagree.** See
`pi-dash/CLAUDE.md`.

Commands arrive as `CommandId` values and go through the same command layer as typed
console commands, which is what phases 2 and 3 were for.

**Verification status:** the framing is proven in software by `proto-test` (52 checks:
round trips including all-zero and no-zero payloads, the maximum payload, single-bit
corruption, mid-stream join, oversized-frame recovery, back-to-back frames). The harness
itself was mutation-tested — swapping the CRC byte order on encode produced 12 failures,
confirming it can actually fail. **The UART round-trip is untested**: nothing is wired to
GP0/GP1 yet, so transmit goes into the void and receive has never seen a byte. Verify with
a loopback jumper between GP0 and GP1, or against the RPi directly, before trusting it.

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

All of this runs on **core 0** (`me7_handler.c`), one step at a time, each waiting for the
ECU's reply. There are no fixed delays anywhere in the sequence — the reference JS
(`me7log`'s `loadHandler`) drives it the same way. A full install takes ~1.5s of K-line
time instead of the ~9s the old sleep-driven version needed, and every chunk's response is
actually checked: 4 consecutive failures aborts the load instead of grinding through 60
more chunks of timeouts.

The PRJ variant (`handler.bin` at `0x387ACC` + `fill-distributor-table`) was an older,
unused path and has been **deleted** (2026-07-26). It never worked: `fill_distributor_table`
wrote the handler pointer in the wrong byte order (`00 38 7A CC` instead of little-endian
`CC 7A 38 00`). Recorded here in case the approach is ever revisited.

### Do not reinstall while the redirect is live

Running `start-logging` again on an ECU that already has the handler installed **breaks
`0x23` and `0x3D` for the rest of that session**. The write chunks travel through the
handler's own service table; by chunk 12 the rewrite has undone the copy of the original
table that the init trigger made, so those services start returning SNS and cannot be used
to undo it. `0xB7` keeps working, so logging survives, and nothing recovers the rest
*within the session* — `0x3E` does not make the handler re-copy the table.

**Recovery is cheap:** let the session drop (or `disconnect`), `connect` again, then
`start-logging`. The fresh session restores normal dispatch and the reinstall then runs
against a working `0x3D`. No power cycle of anything is needed — verified with the ECU
continuously powered.

To avoid it in the first place, either reinstall only after a reconnect, or put the
original pointer back at `0xE228` first (`cmd:3D00E22804D0270602`) so writes route through
the ECU's own dispatcher.

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
   ECU instability. **The claim that `setTimingParams` (`83030001001400`) wedges the
   session was also wrong** — see below; it is now sent on every install and is the
   single biggest win in this project's sampling rate.

## Sample Rate: send the timing parameters

**AccessTimingParameter (0x83) is worth more than everything else combined.** ISO 14230
defaults are P2min 25ms (before the ECU answers) and P3min 55ms (before the tester may ask
again); those dominate the budget entirely. Zeroing them takes a 5-byte sample from
**20.6 to 49.8 samples/s** with nothing else changed.

Frame: `83 03 00 01 00 14 00` — subfunction `03` = set values, then P2min=0, P2max=25ms,
P3min=0, P3max=5000ms, P4min=0. Exactly what ME7Logger sends, and what the real capture in
`me7log/ecu_files/many_logs.txt` opens every session with.

**Position in the sequence is not negotiable:**

- **After** `10 86`. StartDiagnosticSession resets timing to defaults, so sending 0x83
  first and then opening the session silently undoes it — which is exactly what made it
  look useless the first time it was tried here.
- **Before** the `0xE228` redirect. The handler's service table has no 0x83, so afterwards
  it answers `7F 83 11`.

Measured on the bench, all at 10400 baud with timing parameters set:

| Sample | Variables | Rate | Period |
|--------|-----------|------|--------|
| 5 bytes | 5 | 49.8/s | 20ms |
| 10 bytes | 8 | 45.4/s | 22ms |
| 28 bytes | 20 | 24.9/s | 40ms |

Above ~10 bytes the marginal cost is ~1ms per byte, which is simply the 10400 baud wire
time (0.96ms/byte), on top of ~6ms of fixed overhead — so the second lever is the baud
rate.

## Sample Rate: switch the K-line to 57600

`10 86 64` is StartDiagnosticSession with **baud identifier 0x64 = 57600**. The ECU answers
at the old rate and switches afterwards, so the Pico follows it (`uart_set_baud`). With 20
variables this takes **24.9 -> 63.8 samples/s**, and 2854 consecutive samples were logged
with zero checksum failures, echo mismatches or timeouts.

**It only works immediately after the 5-baud handshake.** Sent a few seconds later — for
example typed through the console — it simply times out. `ecu_try_connect()` therefore
calls `me7_handler_open_fast_session()` the instant the init returns 0xEE, before anything
else can get in the way. A refusal is not fatal: the link stays at 10400 and everything
still works, just slower.

Two consequences worth knowing:

- **`kline_init_connection()` resets the rate to 10400 first.** The 5-baud wakeup is
  bit-banged and rate independent, but the sync and key bytes that follow are not, so a
  reconnect after a fast session would otherwise read garbage.
- **The install must not send a second `10 86`.** That is another
  StartDiagnosticSession: it would drop the rate back to 10400 *and* reset the timing
  parameters. `me7_handler_install()` skips its own session step when the fast session is
  already open.

Measured with 20 variables / 28-byte samples:

| K-line rate | Timing parameters | Rate |
|-------------|-------------------|------|
| 10400 | defaults | ~10/s |
| 10400 | zeroed | 24.9/s |
| **57600** | **zeroed** | **63.8/s** |

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
MSG_INSTALL_HANDLER  = 0x0D  // Run the full injection sequence on core0
MSG_LOAD_HANDLER     = 0x0E  // Write the handler blob only
MSG_SAMPLE           = 0x0F  // [seq:2 BE][t_ms:4 BE][raw values] from the logger
MSG_START_STREAM     = 0x10  // 4 bytes BE sample interval in ms (0 = full rate)
MSG_STOP_STREAM      = 0x11
MSG_SET_LOG_VARS     = 0x12  // Flat list of 3-byte ECU addresses
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
