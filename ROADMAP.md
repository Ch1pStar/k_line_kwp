# Roadmap: running our own code, and switchable maps

Written 2026-07-27. Goal, in the user's order of preference:

1. **A confirmed way to write our own code, load it into ECU RAM and have it execute.**
   This is the stated starting point, and everything else depends on it.
2. **Switchable tuning maps**, ideally at runtime or at least without much hassle.
3. **A fallback way to get code/calibration in**, if runtime switching proves impractical:
   reflashing over K-line, or a one-time soldered connection to the board.

**Bench status: the ECU is not connected to the Pico** (moved, 2026-07-27). The Pico is
still here. Everything marked *desk* below can proceed without hardware; everything marked
*bench* is blocked until the ECU is back.

---

## Stage 1 — prove our own code runs

We are much closer than the docs suggested. `misc/logger_handler/` is a complete Keil
uVision project for prj's 236-byte handler, and **it already builds**: the build log reports
`Program Size: code=236`, and converting its Intel HEX output reproduces the exact
`handler.bin` that used to live in this repo (md5 `f6f7f375…`, recoverable with
`git show 649d93e^:handler.bin`). That build is already specialised for *this* ECU — see
"About the deleted handler.bin" in `CLAUDE.md`.

So the toolchain works and the source is right. What is unproven is only the last step:
that the thing it produces executes on the ECU.

**1.1 (desk) Fix the binary output.** Keil is emitting a 991-byte `Objects/*.bin` that is
not the code image; the H86 is correct. Either fix the uVision "create binary" step or add
a small hex→bin conversion to the build. The check is byte-equality against the known-good
236 bytes. There is a working converter in this session's scratch, ~20 lines of Python, no
dependencies — worth committing into `logger_handler/` as `tools/hex2bin.py`.

**1.2 (desk) Write the loader for this handler.** It is *not* a drop-in replacement for
`handler_setzi.bin`, and the old `load_handler_prj` got it wrong. This variant needs:

- the 48-entry service table written to `0x387A00` (192 bytes), every entry the far pointer
  to the handler entry point — encoded `[offset_lo][offset_hi][page_lo][page_hi]`, so
  `0x387ACC` is `CC 3A E1 00`, **not** `00 38 7A CC`;
- the handler code at `0x387ACC`;
- prj's reference build additionally needs a config block at `0x387AC0`; our local build has
  those constants compiled in, so it does not.

**1.3 (bench) Load it and confirm it answers.** Success is unambiguous and documented in
`logger_handler/README.md`: with the handler installed, sending `0xBE` with no arguments
returns `7F BE 13` (IMLOIF) rather than the `7F BE 11` (SNS) an unmodified ECU gives. That
single byte — `13` vs `11` — is the proof that our code, not the ECU's, produced the reply.

**1.4 (bench) Prove it is *ours*.** Rebuild with one deliberate change — e.g. return a
different NRC, or a fixed marker byte — and confirm the wire changes to match. Only this
step rules out "we loaded something and the ECU happened to respond".

After 1.4 we have a verified edit-build-load-run loop into ECU RAM. That is the foundation
for everything below, and it is worth stopping there and consolidating.

---

## Stage 2 — where the maps actually live

*(desk, needs no ECU)*

Calibration data is in **flash**, not RAM: DPP0 and DPP1 map `0x810000` and `0x814000`, and
the flash dump `misc/me7logger-configs/me7logger/8N0906018BP.bin` covers `0x800000`–`0x8FFFFF`
with `file offset = address - 0x800000`. The RAM segment is only 32 KB in total, so a full
alternate calibration cannot simply live there.

Work to do, all against the dump and `side-projects/me7.5_decomp/` (Ghidra):

- Identify the maps worth switching (boost target, fuel, ignition) and their flash addresses.
- Determine **how the code reaches them**. This is the decisive question. If lookups use a
  base address baked into each instruction, switching means patching every site. If any
  lookup goes through a pointer or a base register, that pointer is a single switch point.
- Find the free flash to hold a second map set. 1 MB is large; ME7 images usually have room.

Until this is understood, any plan for switching is speculation.

---

## Stage 3 — the switching mechanism

Three candidates, in increasing order of what they demand:

**(a) Patched flash tune with two map sets and a selector.** This is how multi-map is
normally done on ME7: both sets ship in the calibration, and patched ASW code picks between
them from some input the ECU already reads (a switch on an unused input, a specific pedal
or cruise-stalk pattern, a sensor threshold). Robust, survives power cycles, no live link
needed. Requires Stage 2 plus flashing.

**(b) Maps in RAM, pointers repointed.** Copy a map into the 32 KB RAM and patch the lookup
to read from there. Attractive because RAM is writable over K-line at 57600 with the tooling
we already have, so values could change live. Constrained by the ~950 bytes free above the
handler unless we relocate things, and it depends entirely on Stage 2 finding an indirection
to hook. **Best suited to live tuning of a few values, not to switching whole map sets.**

**(c) Rewrite calibration flash on demand.** Simplest conceptually, worst in practice: flash
wear, seconds of write time, and a bricked ECU if interrupted. Not a runtime switch.

Realistically (a) is the answer for "switch maps", and (b) is the answer for "adjust values
while the engine runs". They are different features and worth keeping separate in our heads.

---

## Stage 4 — getting calibration in

**K-line reflashing.** ME7 supports it: `0x27` SecurityAccess, `0x34` RequestDownload,
`0x36` TransferData, `0x37` RequestTransferExit — see `KWP2000_COMMAND_REFERENCE.md`. The
obstacle is SecurityAccess: a seed/key exchange whose algorithm we do not have. Worth
checking whether the algorithm is derivable from the flash dump in Ghidra, since the ECU
must contain the key routine itself. **This is the path that needs no so