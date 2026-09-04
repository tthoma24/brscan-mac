# Scan button (brscan-scand)

The printer's physical **Scan** button can send a scan to this Mac. You press
Scan on the device, pick this Mac and a destination (File, Image, OCR, or
E-mail), and the scan is pulled and handled here — no software running on the
Mac's screen at the time.

`brscan-scand` is the background agent that makes this work. It registers this
Mac with the printer, waits for a button press, pulls the scan, and performs
the destination's action.

## What each destination does

The destination you choose on the printer selects both the scan settings (from
your config) and what happens to the finished scan:

| Destination | Action |
|---|---|
| **File** | Save the scan to your save directory. |
| **Image** | Save it, then open it (in its default app -- usually Preview -- or an app you configure). |
| **OCR** | Recognize the text with the macOS Vision framework and save a searchable PDF — the image with a selectable, copyable text layer over it. |
| **E-mail** | Save it, then open a new Mail message with the scan attached, ready for you to review and send. It is never sent automatically. |

## Output format

Each destination also has its own output file format, configured with
`<dest>.format` in the config file (`<dest>` is `file`, `image`, `ocr`, or
`email`): `pdf`, `tiff`, `jpeg`, `png`, or `native` (the default). `native`
writes one file per page in the format the destination's scan mode implies
-- JPEG for color, PGM for gray/truegray, PBM for black & white -- the same
behavior as before this setting existed. Any other format instead writes a
single JPEG/PNG per page, or one combined PDF/TIFF holding every page.

**OCR always yields a searchable PDF.** Because OCR's whole point is text
you can select and search, its output format defaults to a PDF with a
Vision-recognized text layer over each page, even if you leave
`ocr.format` unset (or set to `native`) -- there's no native-file option
for OCR specifically. If you set `ocr.format` to `tiff`/`jpeg`/`png`
explicitly, that's still honored, just without a text layer (Vision only
lays text into a PDF page).

For a PDF or TIFF, `<dest>.separation` controls how many pages land in each
file: `combine` (the default) puts every page from one button press into a
single file; `every:N` starts a new file every N pages instead (for
example `every:1` with a 2-page scan produces two separate PDFs, each
named with a `-docNNN` suffix). Separation doesn't apply to jpeg/png/native
output, which always writes one file per page regardless.

`<dest>.tiff_compression` (`lzw`, `g3`, or `g4`; default `lzw`) only
matters for `<dest>.format = tiff`. `g3`/`g4` are 1-bit fax codecs that
only apply to a black & white page; a color or gray page requested with
`g3`/`g4` falls back to `lzw` instead of being thresholded to bilevel.

See `config/brscan-scand.conf.example` for the commented key list and
defaults.

## How it works

The Scan button uses Brother's own registration-and-notification mechanism,
which this project reverse-engineered clean-room from packet captures (see
[PROTOCOL.md](PROTOCOL.md) and `PROVENANCE.md`). In outline:

1. **Register.** The daemon sends the printer an SNMP Set (UDP 161) for each
   destination, telling it this Mac's name, address, and UDP listening port.
   Registrations expire, so the daemon re-sends them every few minutes.
2. **Notify.** When you press Scan and pick this Mac, the printer sends a UDP
   notification to the daemon on port 54925, naming the destination you chose.
3. **Acknowledge.** The daemon echoes the notification back to confirm receipt.
   It ignores any notification that doesn't come from the configured printer.
4. **Pull and act.** The daemon opens the scan connection (TCP 54921), pulls
   the image, saves it, and runs the destination's action.

## Config command

Right after the host sends `ESC K` (`1b 4b`) on the scan connection (TCP
54921), the printer pushes one more frame before the scan data itself: a
snapshot of whatever the LCD panel currently has configured for the button
press about to happen (destination, mode, resolution, paper size, output
format, and the panel's toggle settings). This is separate from the UDP
button-press notification above -- it rides the TCP scan connection, not
UDP 54925 -- and separate from the `ESC I`/`ESC X` negotiation described in
[PROTOCOL.md](PROTOCOL.md).

The frame is:

```
byte 0    : 0x30
byte 1    : payload length (single unsigned byte; observed 59-78)
byte 2    : 0x00
bytes 3.. : payload -- 0x0a-separated KEY=VALUE lines, trailing 0x0a included
```

`byte[1]` equals the payload's byte count (everything after byte 2); a frame
where it doesn't match, or that doesn't start with `0x30`/have `0x00` at
byte 2, is rejected as malformed. There is no `0x80` terminator here --
unlike the scan-command frames in PROTOCOL.md, this is its own self-
contained frame, not part of the `ESC`-command stream.

`daemon/button_config.h`'s `ParseButtonConfig` decodes this frame into a
`ButtonConfig` struct. It's a faithful decode only: paper and output-type
tokens are kept as raw strings rather than mapped to a scan area or this
project's own output-format enum -- later tasks own those mappings, so this
parser stays decoupled and independently testable.

| Key | Field | Values | Notes |
|---|---|---|---|
| `F` | `func` | `FILE`, `IMAGE`, `OCR`, `EMAIL` | The button pressed. Always present; an empty or missing `F` makes the whole frame malformed. |
| `D` | `duplex` | `SIN` -> `false`, `DUP` -> `true` | |
| `E` | `duplex_edge` | `LON`, `SHO` | Raw token; only meaningful when `duplex` is true. |
| `R` | `dpi` | e.g. `200`, `300` | A single value here (`ESC I`/`ESC X` carry an `x,y` pair instead; see PROTOCOL.md). |
| `M` | `mode` | `CGRAY` (color), `TEXT` (black & white) | Raw token. |
| `P` | `paper` | `LETTER`, `LEGAL`, `A4`, `LEDGER`, `A3`, `A5`, `EXECUTIVE`, `PHOTO`, `BCARD` | Raw token; not mapped to a scan area by this parser. `daemon/paper_size.h`'s `AreaForPaper` holds the captured scan area for each of these 9 tokens, but nothing wires that lookup to this field yet -- that's a later task. |
| `A` | `area_flag` | `0` | Observed always 0 (auto-area); the real scan area is computed downstream. |
| `T` | `output_type` | `PDF(Image)`, `MULTI-TIFF`, `JPEG`, `TXT`, `HTML`, `RTF` | Raw token (parens and hyphen kept verbatim); not mapped to this project's `OutputFormat` by this parser. |
| `W` | `skip_blank` | `0`/`1` | |
| `G` | `remove_background` | `0`/`1` | OCR config commands omit this key entirely; its absence leaves `remove_background` at its `false` default. |
| `L` | `remove_background_level` | `64` (Low), `128` (Med), `192` (High) | Present only when `G=1`; defaults to `0`. |
| `X` | `high_speed` | `0`/`1` | |

Unknown keys are ignored (forward compatibility with panel settings not yet
documented here); a missing key leaves its field at the default above; an
unrecognized value for a string field is stored as-is rather than rejected.

## Setup

### 1. Build the daemon

Build the project as described in the [README](../README.md). This produces
`build/brscan-scand` alongside `build/brscan-cli`.

### 2. Install the binary

Copy the binary somewhere stable that the LaunchAgent can point at. The
example plist uses `/usr/local/bin`:

```bash
sudo cp build/brscan-scand /usr/local/bin/brscan-scand
```

If you'd rather not use `sudo`, put it anywhere you own (for example
`~/bin/brscan-scand`) and use that absolute path in the plist's
`ProgramArguments` in the next steps.

### 3. Write the config

Copy the example config and edit it. At minimum, set `printer_host`:

```bash
mkdir -p ~/.config
cp config/brscan-scand.conf.example ~/.config/brscan-scand.conf
```

Find your printer's name with `dns-sd -B _scanner._tcp` (see the README's
"Finding your scanner"), then set `printer_host` in the config. Every other
setting has a default; see the comments in the example file for the full list,
including per-destination mode, resolution, and source, and the Image and
E-mail action settings.

You can check your config before installing the agent by running the daemon in
the foreground:

```bash
build/brscan-scand --config ~/.config/brscan-scand.conf
```

It prints its startup settings and logs each registration and button press.
Press Ctrl-C to stop. Approve the network prompts described under
[Firewall](#firewall) the first time.

### 4. Install the LaunchAgent

`brscan-scand` runs as a **LaunchAgent** — an agent inside your GUI login
session — because the Image, E-mail, and OCR actions open apps and windows on
your desktop. Install it for just your user (no admin rights needed):

```bash
cp config/com.brscan.scand.plist.example ~/Library/LaunchAgents/com.brscan.scand.plist
```

Or install it for all users, which needs admin rights:

```bash
sudo cp config/com.brscan.scand.plist.example /Library/LaunchAgents/com.brscan.scand.plist
```

Open the copied plist and set the first `ProgramArguments` entry to the path
where you installed the binary in step 2, and replace `YOUR_USERNAME` in the
two log paths with your short user name (launchd doesn't expand `~`). Load it
only after your config from step 3 is in place: the agent is set to relaunch if
it exits, so with a missing or invalid `printer_host` it exits at startup and
launchd keeps restarting it (every few seconds) until the config is valid. The
log names the reason each time.

Then load it, using whichever path you copied the plist to in step 4:

```bash
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.brscan.scand.plist
```

The agent starts immediately and again at every login. Within a few seconds,
this Mac appears in the printer's Scan menu.

To stop and unload it:

```bash
launchctl bootout gui/$(id -u)/com.brscan.scand
```

After editing the plist or the config, unload and reload it (bootout, then
bootstrap) for the change to take effect.

## Firewall

The daemon needs three network paths, so approve it if macOS or a third-party
firewall prompts on first run:

- Outbound **UDP 161** (SNMP) — to register with the printer.
- Outbound **TCP 54921** — to pull each scan.
- Inbound **UDP 54925** — to receive button-press notifications.

Without the inbound path, registration still succeeds and this Mac appears in
the Scan menu, but button presses never arrive and nothing scans.

## Troubleshooting

- **This Mac doesn't appear in the printer's Scan menu.** Check that the daemon
  is running (`launchctl list | grep brscan`) and that `printer_host` is
  correct and reachable. Watch the log for `[register] ... -> sent`.
- **The Mac appears, but pressing Scan does nothing.** The notification isn't
  reaching the daemon — check the inbound UDP 54925 firewall path above, and
  that both devices are on the same network.
- **Nothing in the logs at all.** Confirm the plist's `ProgramArguments` path
  points at the actual binary, and check the `StandardErrorPath` you set (the
  example uses `~/Library/Logs/brscan-scand.log`).
- **A press is logged but the scan fails.** The log line names the reason (for
  example, the scanner is busy or the document feeder is empty). The daemon
  keeps running and handles the next press.
