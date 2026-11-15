# SDHI0 Storage Foundation

## Implemented Contract

The camera firmware now includes the board microSD path rather than only an offline journal codec:

- FSP 6.5.0 `SDHI0 -> rm_block_media_sdmmc -> DMAC0` in 4-bit mode.
- Board pins `P111` DAT3, `PD01-PD03` DAT2-DAT0, `PD04` CMD, `PD05` CLK and `PD07` card detect.
- `PD06` remains unassigned for the reserved GPT2 PWM.
- FatFs R0.15, one FAT volume, 512-byte sectors, short names, no heap and no RTOS lock.
- A 32-byte-aligned media path with an aligned one-sector bounce buffer for unaligned FatFs buffers.
- A bounded 250 ms transfer wait with completion, error and card-removal event priority.
- An eight-record RAM queue that rejects the newest record when full and never blocks vision while
  AI inference or capture handoff is timing-critical.

`FIELD.BIN` is the authoritative append-only journal. Every record has a fixed 32-byte big-endian
header, schema, type, sequence, monotonic timestamp, payload length, header CRC and payload CRC.
Startup scans it one record per service step and truncates only an incomplete or corrupt tail.
`FIELD.CSV` is a human-readable projection and is written only after the binary record has been
synced. A power loss may leave CSV behind the binary journal, but cannot make CSV the authority.

Card removal marks storage unavailable, preserves the bounded RAM queue and leaves camera, LCD,
AI and networking active. Reinsertion remounts the filesystem and continues from the recovered
binary tail. Three consecutive media/filesystem failures enter a diagnosable degraded state; a
physical remove/reinsert cycle permits a new mount attempt.

## Capability And Diagnostics

The application advertises `RA8P_CAPABILITY_SD_STORAGE` only while the card is both mounted and
writable. The low-rate `[sd-hb]` line reports insertion, mount/write state, sector count, queue
depth/high-water, committed sequence, byte counts, recovery counts, transfer failures/timeouts and
the last FSP error. The startup line must report `dmac=DMAC0`.

The generated configuration is guarded by `tests/check_fsp_generated_contract.ps1`. It verifies the
SDHI0 stack, callbacks, DMAC0 activation source, all seven pin assignments and that `PD06` remains
free, in addition to the existing CEU geometry contract.

## Remaining Hardware Gate

Code and build verification do not complete the SD feature. Board acceptance still requires a
FAT32 microSD card and the combined firmware:

1. Confirm `[sd-hb] inserted=yes mounted=yes writable=yes`.
2. Confirm `FIELD.BIN` and `FIELD.CSV` are created and grow with telemetry.
3. Remove the card while preview and networking run; storage must degrade without a black screen.
4. Reinsert it and confirm remount plus continued records.
5. Cold-start with the existing files and confirm corrupt/incomplete-tail counters remain zero.
6. Regress camera preview, LCD color/orientation and USER freeze/inference/resume for three rounds.

Until this gate passes, the board is still considered to run the last SD-free accepted firmware.

## Verification

```powershell
powershell -ExecutionPolicy Bypass -File workspace\camera\tests\run_host_tests.ps1
powershell -ExecutionPolicy Bypass -File workspace\camera\tests\check_fsp_generated_contract.ps1
```

FatFs is used under its upstream license in `src/fatfs/LICENSE.txt`. The binary journal/config
goldens are independently checked with IEEE CRC-32 values rather than copying the product encoder.
