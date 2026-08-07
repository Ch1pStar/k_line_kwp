# k_line_kwp

Raspberry Pi Pico (RP2040) firmware that talks KWP2000 (ISO 14230) to a Bosch
ME7.5 ECU over the K-line, injects a fast-logging handler into ECU RAM, and
streams multi-variable samples to a host. It is the ECU-facing half of a digital
dash — the other half (bridge + PixiJS dashboard) is [`pi-dash`](../pi-dash).

```
ECU ──K-line 10400/57600──> Pico ──uart0 921600──> RPi 5 (pi-dash)
  ME7.5, 1.8T                KWP2000 + injected      COBS-framed binary link
                             0xB7 log handler        + USB serial console
```

The Pico is deliberately a dumb pipe: it frames raw ECU bytes and knows nothing
about scaling or meaning. Decoding lives on the host, which has the `.ecu` file.

## What works

Fast logging runs end to end on the bench: `connect` → `start-logging` →
`stream-on` streams **20 variables at ~64 samples/s** (57600 baud, zeroed KWP
timing parameters, extended-length frames), costing the ECU only ~3.7 points of
its own CPU. The logger auto-recovers from a lost handler and a dropped session
without any power cycling. The Pico↔Pi UART link is framed and self-tested in
software (`proto-test`) but has never carried a byte over real wire.

## Design

Dual-core: **core 0** owns the K-line — 5-baud init, KWP2000 framing, the
handler injection sequence, the free-running sampler and the heartbeat — and
never touches stdio. **Core 1** owns the frontends: the USB serial console and
the binary host link, fed from core 0 through a lock-free SPSC ring buffer.

```
src/
  platform/   PIO bit-banged K-line UART (8N1 at 10400/57600)
  kline/      5-baud init, KWP2000 framing, DTC parsing
  ecu/        connection state machine, handler injection, sampler
  host/       command layer, USB console, RPi link (COBS + CRC16)
  ipc/        SPSC ring buffer, message types, core-0-safe logging
```

## Hardware

| Signal | Pin |
|--------|-----|
| K-line RX / TX | GPIO 18 / GPIO 15 (PIO) |
| RPi 5 link TX / RX | GPIO 0 / GPIO 1 (uart0, 921600) |
| Console | USB serial |

## Build and use

```bash
./build.sh        # needs Pico SDK at $PICO_SDK_PATH (default /opt/pico-sdk)
# flash build_linux/k_line_kwp.uf2, open the USB console, then:
connect
start-logging
stream-on:33      # ~30 Hz; stream-on for full rate
```

`set-vars:HEX` replaces the logged variable list (up to 32 entries), `read-dtcs`
/ `clear-dtcs` / `ecu-id` cover diagnostics, and `dump:ADDR:LEN` reads raw ECU
memory. The full command list is in [CLAUDE.md](CLAUDE.md).

## Read before touching anything

**[CLAUDE.md](CLAUDE.md)** is the engineering log and is load-bearing: the
handler injection flow and its far-pointer encoding, the strict ordering of the
57600 switch and timing parameters, the do-not-reinstall-while-redirected trap,
and the history of misdiagnoses that older notes still contradict.
[ROADMAP.md](ROADMAP.md) covers what's next (running our own code, switchable
maps); [KWP2000_COMMAND_REFERENCE.md](KWP2000_COMMAND_REFERENCE.md) is the full
ISO 14230 service table. Changing the host-link framing in `src/host/proto.c`
requires regenerating pi-dash's test vectors, or the two ends silently disagree.
