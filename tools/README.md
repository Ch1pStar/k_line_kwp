# Hardware driver scripts (WSL + Windows COM port)

The Pico is on a Windows COM port (COM5 on this bench), but development happens in
WSL. These PowerShell scripts are invoked from WSL via `powershell.exe` interop so
Claude (or you) can drive the board without leaving WSL.

## ecu.ps1 — send console commands, capture output

```bash
cd /mnt/c && powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File '\\wsl.localhost\Debian\<abs-path>\tools\ecu.ps1' \
  -Port COM5 -Commands 'connect;start-logging;read-log;read-log' -Settle 25
```

- `-Commands` is a **semicolon-separated** list (not a PS array — `-File` flattens arrays).
- Drains until the line is idle (~2.8s of silence) or `-Settle` seconds elapse. The
  2.8s idle floor is deliberate: 5-baud `connect` has a ~2.4s silent stretch and a
  shorter floor makes the driver think connect finished early.
- Requires the COM port to be free (close any serial terminal first — Windows locks it
  exclusively; "Access denied" means something else has it open).

## flash.ps1 — build already done, flash the uf2

```bash
cd /mnt/c && powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File '\\wsl.localhost\Debian\<abs-path>\tools\flash.ps1' -Port COM5
```

Touches the port at 1200 baud to drop the Pico into BOOTSEL, copies
`build_linux/k_line_kwp.uf2` to the RPI-RP2 drive, waits for the COM port to re-enumerate.
The copy often reports an I/O error because the board resets mid-write — that's normal.

## Typical loop

```bash
PICO_SDK_PATH=/mnt/c/projects/devbox/pico-sdk ./build.sh   # or: make -C build_linux
# then flash.ps1, then ecu.ps1
```

Note `build.sh` defaults `PICO_SDK_PATH` to `/opt/pico-sdk` which does not exist here;
the SDK is at `/mnt/c/projects/devbox/pico-sdk` (v2.1.1).
