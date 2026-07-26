# KWP2000 (ISO 14230) Command Reference

## Project Overview

### K-Line KWP Interface (`k_line_kwp`)

A **Raspberry Pi Pico-based K-Line/KWP2000 ECU interface** targeting Bosch ME7.5 ECUs. Dual-core architecture: Core 0 handles ECU communication, Core 1 runs a USB serial dashboard. Uses PIO-based UART at 10,400 baud with 5-baud slow init (sends 0x88 for programming mode wakeup).

**Commands currently implemented:**

| SID  | Name                   | Usage in code                          |
|------|------------------------|----------------------------------------|
| 0x10 | StartDiagnosticSession | Param 0x86 (special session)           |
| 0x14 | ClearDTCs              | Params 0xFF, 0x00                      |
| 0x18 | ReadDTCByStatus        | Params 0x00, 0xFF, 0x00                |
| 0x1A | ECU Identification     | Param 0x9B                             |
| 0x3D | WriteMemoryByAddress   | Writes handler binaries to ECU RAM     |
| 0x3E | TesterPresent          | Heartbeat / keep-alive                 |

### Logger Handler (`logger_handler`)

A **236-byte C166 assembly exploit** that injects a custom fast-logging diagnostic handler into ME7 ECU RAM. It hijacks the service distributor table to add a custom service **0xBE** (response on **0xFE**), enabling high-speed multi-variable datalogging (~50 Hz single variable, ~80 variables per request at <5% CPU).

---

## Response ID Convention

- **Positive response** = Request Service ID + 0x40 (e.g., request 0x10 → positive response 0x50)
- **Negative response** = `0x7F [rejectedServiceID] [NRC]`

---

## Complete Service ID Table

### Diagnostic & Session Management

| Request | Response | Service                    | Description                                                                 |
|---------|----------|----------------------------|-----------------------------------------------------------------------------|
| 0x10    | 0x50     | StartDiagnosticSession     | 0x01=default, 0x02=programming, 0x03=extended, 0x86=manufacturer-specific   |
| 0x11    | 0x51     | ECUReset                   | 0x01=hard, 0x02=keyOffOn, 0x03=soft                                        |
| 0x14    | 0x54     | ClearDiagnosticInformation | Clears stored DTCs                                                         |
| 0x17    | 0x57     | ReadStatusOfDTC            | Read status of specific DTC (legacy)                                       |
| 0x18    | 0x58     | ReadDTCByStatus            | Read DTCs filtered by status mask                                          |
| 0x20    | 0x60     | StopDiagnosticSession      | Return to default session                                                  |
| 0x27    | 0x67     | SecurityAccess             | Odd sub-fn=requestSeed, even=sendKey                                       |
| 0x28    | 0x68     | CommunicationControl       | Enable/disable normal message TX/RX                                        |
| 0x3E    | 0x7E     | TesterPresent              | Keep-alive / heartbeat                                                     |

### Data Read/Write

| Request | Response | Service                              | Description                                          |
|---------|----------|--------------------------------------|------------------------------------------------------|
| 0x1A    | 0x5A     | ReadECUIdentification                | Read ECU info by 1-byte local ID                     |
| 0x21    | 0x61     | ReadDataByLocalIdentifier            | Read by 1-byte record number                         |
| 0x22    | 0x62     | ReadDataByIdentifier                 | Read by 2-byte DID (e.g. VIN=0xF190)                 |
| 0x23    | 0x63     | ReadMemoryByAddress                  | Read raw memory (address + size)                     |
| 0x26    | 0x66     | SetDataRates                         | Configure periodic data TX rates                     |
| 0x2A    | 0x6A     | ReadDataByPeriodicIdentifier         | Request periodic data transmission                   |
| 0x2C    | 0x6C     | DynamicallyDefineLocalIdentifier     | Create dynamic DID from memory addresses             |
| 0x2E    | 0x6E     | WriteDataByIdentifier                | Write data by 2-byte DID                             |
| 0x2F    | 0x6F     | InputOutputControlByIdentifier       | Actuator tests, sensor overrides                     |
| 0x30    | 0x70     | InputOutputControlByLocalIdentifier  | I/O control by 1-byte local ID                       |
| 0x3B    | 0x7B     | WriteDataByLocalIdentifier           | Write by local identifier                            |
| 0x3D    | 0x7D     | WriteMemoryByAddress                 | Write raw data to ECU memory                         |

### Upload / Download (Flashing)

| Request | Response | Service               | Description                                    |
|---------|----------|-----------------------|------------------------------------------------|
| 0x34    | 0x74     | RequestDownload       | Request to transfer data TO ECU                |
| 0x35    | 0x75     | RequestUpload         | Request to transfer data FROM ECU              |
| 0x36    | 0x76     | TransferData          | Transfer a data block (with sequence counter)  |
| 0x37    | 0x77     | RequestTransferExit   | End transfer, optional CRC verification        |
| 0x38    | 0x78     | StartRoutineByAddress | Start routine by memory address                |

### Routine Control

| Request | Response | Service                                | Description                                      |
|---------|----------|----------------------------------------|--------------------------------------------------|
| 0x31    | 0x71     | StartRoutineByLocalIdentifier          | Start ECU routine (erase flash, checksum, etc.)  |
| 0x32    | 0x72     | StopRoutineByLocalIdentifier           | Stop a running routine                           |
| 0x33    | 0x73     | RequestRoutineResultsByLocalIdentifier | Get routine results                              |

### Custom / Manufacturer-Specific

| Request   | Response | Service                          | Description                                        |
|-----------|----------|----------------------------------|----------------------------------------------------|
| 0xB7      | 0xF7     | Custom Fast Logging (`handler_setzi.bin`) | The handler we actually inject and use. Set variables, then bare 0xB7 to sample. |
| 0xBE      | 0xFE     | Custom Fast Logging (`handler.bin`, 236B) | The other handler variant. NOT the one wired to `start-logging`. |
| 0xA0-0xBF | —        | Manufacturer-specific range      | OEM-defined services                               |
| 0xC0-0xFE | —        | System-supplier-specific range   | Supplier-defined services                          |

> **Which SID?** The 582-byte `handler_setzi.bin` (the one `load-handler`/`start-logging`
> write) responds on **0xB7 / 0xF7**, not 0xBE. The 0xBE number comes from the
> 236-byte `handler.bin` variant and its `logger_handler` README, which describes a
> *different* binary. Confirmed empirically: at offset 204 of `handler_setzi.bin` the
> instruction is `47 F8 B7` = `CMPB RL4, #0B7h`. Sending 0xBE to the setzi handler
> just returns SNS.

---

## Negative Response Codes (NRC)

Returned as `0x7F [SID] [NRC]`

| NRC  | Name                                    | Description                                        |
|------|-----------------------------------------|----------------------------------------------------|
| 0x10 | generalReject                           | General/unspecified rejection                      |
| 0x11 | serviceNotSupported                     | Service ID not supported by ECU                    |
| 0x12 | subFunctionNotSupported                 | Sub-function not supported                         |
| 0x13 | incorrectMessageLengthOrInvalidFormat   | Wrong message length or format                     |
| 0x14 | responseTooLong                         | Response would exceed transport buffer             |
| 0x21 | busyRepeatRequest                       | ECU busy, try again                                |
| 0x22 | conditionsNotCorrect                    | Preconditions not met (e.g., engine running)       |
| 0x24 | requestSequenceError                    | Wrong order of requests                            |
| 0x25 | noResponseFromSubnetComponent           | Sub-component did not respond                      |
| 0x26 | failurePreventsExecution                | Hardware failure prevents execution                |
| 0x31 | requestOutOfRange                       | Parameter value out of range                       |
| 0x33 | securityAccessDenied                    | Security access required but not granted           |
| 0x35 | invalidKey                              | Wrong security key sent                            |
| 0x36 | exceededNumberOfAttempts                | Too many failed security attempts (lockout)        |
| 0x37 | requiredTimeDelayNotExpired             | Security lockout timer still active                |
| 0x40-0x4F | downloadNotAccepted / uploadNotAccepted | Various download/upload rejection reasons      |
| 0x70 | uploadDownloadNotAccepted               | Transfer not accepted                              |
| 0x71 | transferDataSuspended                   | Transfer suspended                                 |
| 0x72 | generalProgrammingFailure               | Flash programming failed                           |
| 0x73 | wrongBlockSequenceCounter               | Block counter mismatch during TransferData         |
| 0x78 | requestCorrectlyReceivedResponsePending | ECU needs more time, will respond later            |
| 0x7E | subFunctionNotSupportedInActiveSession  | Sub-function valid but not in current session      |
| 0x7F | serviceNotSupportedInActiveSession      | Service valid but not in current session           |

---

## Typical Flash Reprogramming Sequence

```
0x10 0x02           → Start programming session
0x27 0x01           → SecurityAccess: request seed
0x27 0x02 [key]     → SecurityAccess: send key
0x31 [routineID]    → StartRoutine: erase flash
0x33 [routineID]    → RequestRoutineResults: verify erase
0x34 [addr][size]   → RequestDownload
0x36 [seq][data]    → TransferData (repeated for each block)
0x37                → RequestTransferExit
0x31 [routineID]    → StartRoutine: checksum verify
0x11 0x01           → ECUReset: hard reset
```

---

## KWP2000 Packet Structure

```
[Length] [Service ID] [Data Bytes...] [Checksum]
```

**Checksum:** `(1 + dataLength) + serviceId + sum(all_data_bytes)`

**Half-duplex echo (important):** K-line is a single wire, so every byte the Pico
transmits appears back on its own RX. This is *not* the ECU echoing — it is electrical
loopback. The firmware reads and verifies each echoed byte **inline as it sends**
(`kwp2000.c: send_packet`); the response reader then starts at the ECU's first real
byte. The previous approach transmitted the whole frame and *then* tried to skip the
echo, which overflowed the PIO RX FIFO (~9 bytes) and silently dropped echo bytes on
any frame longer than ~9 bytes — desynchronising every field of the reply. That single
bug was what made the 0xB7 handler look like it was rejecting valid requests.

---

## K-Line Physical Layer

| Parameter        | Value                                              |
|------------------|----------------------------------------------------|
| Baud Rate        | 10,400 bps                                         |
| Format           | 8N1                                                |
| Max Payload      | 255 bytes (single frame)                           |
| 5-Baud Init      | Send ECU address (0x88) at 5 baud                  |
| Fast Init        | 25ms low / 25ms high wakeup pattern                |
| Sync Byte        | 0x55 (ECU response after wakeup)                   |
| Key Bytes        | Two bytes received after sync                      |
| Complement       | 0xFF - key_byte_2, sent back to ECU for handshake  |
| Address Response | 0xEE = programming mode, 0xCC = KWP2000 mode      |

---

## Handler Configuration (ME7 ECU RAM)

The **working** setup uses `handler_setzi.bin` loaded as one contiguous 582-byte blob
at `0x387A00` — its first 192 bytes are the service table (48 x 4-byte entries all
pointing at the handler entry `0x387AC6`) and the rest is code. It is **not** the
split table-at-0x387A00 / code-at-0x387ACC layout the older `handler.bin` PRJ path uses.

| Address    | Name          | Description                                              |
|------------|---------------|---------------------------------------------------------|
| 0x387A00   | newdist       | Injected service table + handler blob (582 bytes total) |
| 0x387AC6   | handler entry | Code entry point (what every table slot points to)      |
| 0xE228     | dist pointer  | Far ptr to the active service table. **This is what we redirect.** |
| 0xE1CE     | recbufptr     | Pointer to receive buffer                               |
| 0xE1CA     | reclen        | Length of incoming request                              |

Original value at `0xE228` on this ECU is `D0 27 06 02` (= far ptr `0x81A7D0`, the
BootRom table). A hard ECU reset restores this default.

### Redirecting the dispatcher (the step that was missing)

Loading the handler into RAM is not enough — the ECU still dispatches through its
original table until you repoint `0xE228`:

```
cmd:3D00E22804003AE100
     3D             WriteMemoryByAddress
     00E228         address 0x00E228
     04             4 bytes
     003AE100       C166 far-pointer encoding of 0x387A00
```

`0x387A00` encodes as `00 3A E1 00`: page = `0x387A00 >> 14 = 0xE1`, offset =
`0x387A00 & 0x3FFF = 0x3A00`, stored little-endian as `[offset_lo offset_hi page_lo page_hi]`.

Flash memory (e.g. the original table at `0x81A7D0`) is **not** readable via 0x23 —
it returns NRC 0x12. Only RAM reads are permitted, so the handler must copy the
original table itself on its first invocation (which is why the init step exists).

### 0xB7 Fast Logging protocol (`handler_setzi.bin`)

1. **Set variables:** `0xB7` + `0x03` + one 3-byte big-endian address per variable.
   Positive response `0xF7`. Example (5 vars): `B7 03 00F89A 380991 38099D 3809F6 380A32`.
2. **Sample:** bare `0xB7`. Response `F7` carries the packed values, one field per
   variable in request order, sizes per the ECU's variable definitions.

The leading `0x03` format byte's exact meaning is still under investigation. The
`[length][3-byte addr]` + `0x00` terminator array format below belongs to the *other*
(0xBE) handler variant.

### Data Array Format (0xBE handler variant only)

Each entry is 4 bytes:

```
Byte 0:     Data length to read (1-4 bytes)
Bytes 1-3:  24-bit big-endian address
Terminator: 0x00
```
