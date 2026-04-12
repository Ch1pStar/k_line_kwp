# K-Line KWP2000 ECU Interface

## What This Project Is

A Raspberry Pi Pico (RP2040) firmware that communicates with Bosch ME7.5 automotive ECUs over the K-Line bus using KWP2000 (ISO 14230). The end goal is injecting a custom fast-logging handler into ECU RAM to enable high-speed multi-variable datalogging.

A companion project at `../../misc/logger_handler/` contains the 236-byte C166 assembly handler binary (`fastlogging_ramhandler`) that gets injected into ECU RAM. It adds a custom diagnostic service 0xBE (response 0xFE) for fast multi-variable reads.

## Architecture

Dual-core design on the RP2040:
- **Core 0** (`ecu_state_machine.c`): K-Line init, KWP2000 packet send/receive, heartbeat keep-alive. Runs the ECU connection state machine.
- **Core 1** (`dashboard.c`): USB serial console UI, command parsing, handler binary loading. User-facing interface.
- **Inter-core comms** (`ring_buffer.c`): Lock-free(ish) ring buffer with critical sections. 256 messages, 256 bytes each. Message types defined in `ecu_state_machine.h`.

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
  dashboard.c / dashboard.h - Core 1 console commands, handler loading
  ring_buffer.c / ring_buffer.h - Inter-core message queue
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
read-dtcs                - Read fault codes (SID 0x18, params 0x00 0xFF 0x00)
clear-dtcs               - Clear fault codes (SID 0x14, params 0xFF 0x00)
diag-session             - Start diagnostic session (SID 0x10, param 0x86)
load-handler             - Write handler_setzi.bin to ECU RAM at 0x387A00
fill-distributor-table   - Fill 48-entry service table at 0x387A00 with handler addr
cmd:XXXX                 - Send raw hex KWP2000 command (e.g., cmd:1A9B for ECU ID)
```

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

## Handler Injection Flow

1. `load-handler` writes `handler_setzi.bin` to ECU RAM at **0x387A00** in 8-byte chunks via SID 0x3D
2. `load-handler` (PRJ variant) writes `handler.bin` to **0x387ACC**, then calls `fill-distributor-table`
3. `fill-distributor-table` writes 48 x 4-byte entries at **0x387A00**, each pointing to **0x387ACC** (the handler code)
4. Once injected, sending SID **0xBE** invokes the custom logging handler; response comes on **0xFE**

## Handler Data Array Format (for 0xBE requests)

Each variable entry is 4 bytes:
```
[length: 1 byte] [address: 3 bytes big-endian]
```
Terminated by a `0x00` byte.

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
```

## Key Constants

| Constant | Value |
|----------|-------|
| SERIAL_BAUD | 10,400 |
| PIO_RX_PIN | 18 |
| PIO_TX_PIN | 15 |
| MAX_DATA_SIZE | 255 |
| MAX_RESPONSE_SIZE | 80 |
| RING_BUFFER_SIZE | 256 |
| MAX_MESSAGE_SIZE | 256 |
| DEFAULT_HEARTBEAT_INTERVAL_MS | 5000 |

## ECU Target

- **Platform**: Bosch ME7.5 (Infineon C167CR, 16-bit, segmented memory)
- **Example part number**: 8D0907551K
- **RAM segment for handler**: #038h (addresses like 0x387A00)
- **Init address byte 0xEE**: Programming mode (expected)
- **Init address byte 0xCC**: KWP2000 mode

## Full KWP2000 Service Reference

See `KWP2000_COMMAND_REFERENCE.md` for the complete ISO 14230 service ID table, negative response codes, and flash reprogramming sequence.
