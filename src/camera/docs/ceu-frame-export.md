# CEU stopped-frame export

Wait for the final UART line whose fields include `export stable=yes`, `slot`,
`sequence`, `generation`, `address`, `bytes=614400`, and `crc32`. The continuous
capture state machine has stopped at a frame boundary and holds that exact
generation under a consumer lease, so the reported bytes remain stable while
CPU0 continues to emit heartbeats.

Do not reuse an address or CRC from another boot or firmware build. Record the
complete export line so the raw file remains associated with its slot and
generation.

Start J-Link Commander with the RA8P1 CPU0 target:

```powershell
JLink.exe -device R7KA8P1KF_CPU0 -if SWD -speed 4000 -autoconnect 1
```

At the J-Link prompt, halt CPU0 and export exactly `0x96000` (614,400) bytes
from the address on the stable export line:

```text
halt
savebin frame.raw <UART-export-address> 0x96000
go
exit
```

Convert and validate the dump from the `workspace/camera` directory:

```powershell
python .\tools\rgb565_to_image.py .\frame.raw .\frame.ppm --width 640 --height 480
```

The utility must report `bytes=614400`, `width=640`, and `height=480`. Its
CRC32 must equal the `crc32` value from the same UART slot/generation line.
The resulting PPM must be a coherent 640 x 480 image. A CRC from a different
generation is not valid evidence even if the static address is the same.

The OV5640 DVP byte stream is stored as big-endian RGB565 words, which is the
converter default. Use `--byte-order little` only as a diagnostic comparison.

## FSP 6.5.0 data-synchronous contract

The generated CEU transport uses 1280 one-byte samples per line, matching
the logical 640-pixel, 1280-byte RGB565 line. HD/VD mismatch and missing-HD
(NHD) interrupts remain disabled because FSP maps active capture geometry into
blanking-inclusive checks while the OV5640 legitimately has longer HTS/VTS.

`ceu_cdssr=0` is expected in this mode. RA8P1 `CDSSR` supplies a byte count
only for Data Enable Fetch mode, not the Data Synchronous Fetch mode used here.
Frame-end, absence of fatal events, CEU idle state, callback-buffer identity,
fixed geometry, generation ownership, and CRC form the completion contract.
