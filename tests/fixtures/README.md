# Protocol fixtures

Byte streams captured from network traffic between a Mac running the vendor
driver and the printer, both owned. They are the oracle for the encoder and
decoder tests. No Brother software is included. See
[../../docs/PROTOCOL.md](../../docs/PROTOCOL.md) and
[../../PROVENANCE.md](../../PROVENANCE.md).

Extracted from `reference/brother-scan.pcap` (git-ignored, kept local).

## requests/

Host-to-scanner command bytes. Each `.bin` is the `ESC X` execute command
(`0x1b 0x58 ... 0x80`) for one scenario, unless noted. `all-commands.bin` is
the full host-to-scanner byte stream for the session (every command, in order).

| File | Scenario |
|---|---|
| `all-commands.bin` | Every command sent during the capture session, in order |
| `color-300-a3.bin` | Color, 300 dpi, full A3 |
| `color-100-a3.bin` | Color, 100 dpi, full A3 |
| `gray-300-a3.bin` | Grayscale, 300 dpi, full A3 |
| `color-300-crop.bin` | Color, 300 dpi, cropped region (area encoding) |
| `color-150.bin` `color-200.bin` `color-400.bin` `color-600.bin` `color-1200.bin` | Color at each resolution |
| `adf-color-300-simplex.bin` | ADF, color, 300 dpi, simplex |
| `adf-color-300-duplex.bin` | ADF, color, 300 dpi, duplex |
| `adf-nopaper-attempt.bin` | Full command sequence of an ADF scan attempted with no paper (ends at `ESC D ADF`) |

## responses/

Scanner-to-host bytes, trimmed.

| File | Contents |
|---|---|
| `color-scan-head.bin` | Start of a color scan response: `+OK 200` greeting, the `ESC Q` capability reply, the `ESC I` offer, the first data block header, and the JPEG start |

## To extend during response decoding (Task 6)

More response fixtures are carved from the pcap as the block framing is
decoded: a grayscale (raw) block, the multi-page ADF page delimiters, the
host-cancel stream tail, and the ADF no-paper status. Each is trimmed to the
first blocks plus the terminal status.
