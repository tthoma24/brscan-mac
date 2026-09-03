# Brother network scan protocol (MFC-J6920DW)

This document describes the raw scan protocol the MFC-J6920DW speaks on TCP
port 54921, as observed from packet captures of traffic between a Mac running
the vendor driver and the printer. It is derived entirely from black-box
network observation of hardware we own, cross-checked against public
open-source implementations. No Brother software was consulted. See
[PROVENANCE.md](../PROVENANCE.md).

## Transport

- TCP, port 54921. The scanner sends `+OK 200\r\n` unsolicited on connect
  (ready), or `-NG 401` (busy).
- The vendor driver keeps one connection open and runs many scan sessions over
  it. A fresh connection per scan also works: the device greets on every
  connect.

## Commands (host to scanner)

Every command is `0x1b <letter> 0x0a [body] 0x80`, with one exception (Reset,
below). The body, when present, is one or more `KEY=VALUE` lines separated by
`0x0a`.

| Command | Bytes | Body | Meaning |
|---|---|---|---|
| Query | `1b 51` | (none) | Session init, sent once after connect |
| Info | `1b 49` | `R=x,y` `M=mode` `D=dup` | Negotiate; scanner replies with an offer |
| Select flatbed | `1b 53` | `FB` | Select the flatbed source |
| Select ADF | `1b 44` | `ADF` | Select the document feeder source |
| Execute | `1b 58` | scan parameters (below) | Start the scan; image data follows |
| Reset | `1b 52` (bare, no LF, no terminator) | n/a | Seen once mid-stream in the capture; purpose unconfirmed |

Reset is the one exception to the standard framing: it is a bare two-byte
sequence, `1b 52`, with no trailing `0x0a` and no `0x80` terminator. It
appeared exactly once in the capture, mid-stream rather than at connection
teardown, and it is not part of the normal scan flow (`ESC Q`, `ESC S`/`ESC
D`, `ESC I`, `ESC X`); its exact role is unconfirmed.

Source is selected explicitly: `ESC S FB` for the flatbed, or `ESC S FB`
followed by `ESC D ADF` for the document feeder. It is not inferred from paper
presence; a flatbed scan runs on the glass even with paper in the feeder.

### Execute (`ESC X`) parameters

```
B=50            brightness (0-100, 50 = neutral)
N=50            contrast (0-100, 50 = neutral)
M=CGRAY         mode: CGRAY = color, GRAY64 = grayscale
C=JPEG          compression: JPEG for color, NONE for grayscale
J=MID           JPEG quality (color only)
R=300,300       resolution dpi, x and y
A=x0,y0,x1,y1   scan area in pixels at the scan resolution
D=SIN           SIN = simplex, DUP = duplex (document feeder only)
P=0 E=1 G=0     page/edge/gamma flags (constant in all captures)
```

Note the two distinct uses of "D": the `ESC D` command selects the ADF source,
while `D=` inside `ESC X` selects simplex or duplex.

### Black & White, Error Diffusion, and True Gray (`C=RLENGTH`)

Captured from Brother iPrint&Scan rather than Image Capture (issue #4), over
the same protocol and connection flow above. Three additional modes all use a
per-row run-length payload instead of JPEG or raw:

| Mode | `M=` | `C=` | Payload |
|---|---|---|---|
| Black & White | `TEXT` | `RLENGTH` | 1-bit, packed 8px/byte |
| Error Diffusion (dithered gray) | `ERRDIF` | `RLENGTH` | 1-bit, packed 8px/byte |
| True Gray | `GRAY256` | `RLENGTH` | 8-bit, 1 byte/pixel |

iPrint&Scan's ESC X and ESC I for these three modes carry a different param
set than the CGRAY/GRAY64 flow above: ESC I adds `S=NORMAL_SCAN`; ESC X adds
`S=NORMAL_SCAN` and `L=0`, and uses `E=0` in place of `E=1`. The field order
also differs (`R,M,C,J,B,N,A,D,S,P,E,G,L` vs. `B,N,M,C,J,R,A,D,P,E,G`); the
device accepts either order, so this codebase reproduces iPrint&Scan's order
byte-for-byte for these three modes rather than merging the two shapes.

#### Row block framing

The image arrives as one block per scanline (confirmed: a 3472x4913 capture
produced exactly 4913 row blocks). Each block is a 12-byte header:

```
<type> 07 00 01 00 84 00 00 00 00 <len:le16>
```

`type` at byte 0 and the little-endian payload length in the last two bytes
(this is the same 12-byte shape `DetectHeaderLength` in `scanner.cpp` already
recognized for a raw gray/JPEG block on current firmware -- see "Image data"
above -- just with the marker byte now dispatched on rather than assumed).
Confirmed `type` values:

- `0x42`: the payload is `<len>` bytes of RLENGTH-compressed row data (below).
- `0x40`: the payload is `<len>` bytes of that row's *uncompressed* samples
  (the same shape as a GRAY64/`C=NONE` block). Observed for the large
  majority of rows in a True Gray capture -- the device apparently falls
  back to sending a row raw rather than compressing it, at least for some
  content, so a reader must accept both types for these three modes, not
  just 0x42.
- `0x64`: a JPEG chunk (not used by these three modes; documented above).
- `0x82`: not a row -- an end-of-page/status marker (`0x82 07 00 01 00 84
  00 00 00 00 <status>`, 11 bytes, no length-prefixed payload) that follows
  the last row block. A reader that already knows the requested height
  (as this codebase's `RunScan` does) can simply stop after that many rows
  without needing to parse this marker.

#### RLENGTH row codec

RLENGTH is the classic Apple/TIFF PackBits algorithm (see
`libbrscan/decode_rlength.h` and PROVENANCE.md for how this was derived and
cross-checked). Each row's compressed payload is a sequence of runs, each
starting with a control byte `c`:

- `c` in `[0x00, 0x7f]`: **literal run**. Copy the next `c + 1` bytes from
  the payload to the output verbatim.
- `c == 0x80`: **no-op**. No output, and no value byte is consumed.
- `c` in `[0x81, 0xff]`: **repeat run**. The single next byte in the
  payload is repeated `257 - c` times in the output (2 to 128 repeats).

Decoding a row's whole compressed payload must consume it exactly and
produce exactly that row's fixed byte width: `ceil(width_px / 8)` for the
1-bit modes (TEXT/ERRDIF), or `width_px` for True Gray (GRAY256, 1
byte/pixel). Verified against this project's own capture: every one of the
4913 compressed rows in a 3472px-wide TEXT scan decodes to exactly 434
bytes; a companion Error Diffusion capture decodes 4896 of its 4897
compressed rows to the same 434 bytes, the sole exception being a single
row with a corrupted length field on the wire (a hardware/capture
anomaly -- the bytes that follow are unambiguously the *next* row's
header, arriving earlier than the corrupted length claimed -- not a flaw
in this algorithm). Decoded 1-bit rows use the standard fax/PBM
convention: bit value 1 = black, 0 = white, packed most-significant-bit
first (see `PixelFormat::kBitonal` in `types.h`).

The row width actually used by the device is the requested `A=` area's
width, not the ESC I offer's granted maximum -- in this project's capture
the offer reported a 3460px maximum but the request's `A=0,0,3472,4913`
was honored as-is, delivering 3472px-wide rows.

A typical scan is: `ESC Q` (once per connection), then per scan
`ESC S FB` (or `+ ESC D ADF`), `ESC I` to negotiate, and `ESC X` to run it.

## Responses (scanner to host)

1. Greeting: `+OK 200\r\n` on connect.
2. `ESC Q` reply: a binary capability block (starts `c1 00 ...`) carrying the
   device maxima and supported-size bitmaps. Not required for basic scanning.
   No fixed or self-describing length was found for it (55-56 bytes across
   samples); a client that doesn't need its contents can just read until the
   connection goes quiet rather than relying on a byte count.
3. `ESC S` / `ESC D` reply: a short ack with no fixed length observed --
   2 bytes (`80 00`) in one capture, as little as 1 byte (`80`) live against
   real hardware. Contents unconfirmed; safe to discard.
4. `ESC I` reply: `[1-byte status][2-byte little-endian length][ASCII CSV
   text][NUL]`. The status byte was `0x00` in every sample seen and its
   meaning is unconfirmed. The CSV is a comma-terminated offer of the granted
   parameters: `xdpi,ydpi,flag,?,xmaxpx,?,ymaxpx,`. For A3 at 100 dpi the
   observed offer is `100,100,2,292,1153,427,1684,` (1153 x 1684 px = 11.53 x
   16.84 in). The capability probe (`R=19200,19200`) returns the device
   maximum, observed as `2400,1200` and `2400,2400` depending on source.
5. Image data: one or more blocks, each a short binary header followed by
   payload. The header is 12 or 13 bytes depending on firmware (a live probe
   against real hardware found 12 bytes, missing a constant leading `0x00`
   byte that an earlier vendor-driver capture had); either way its last two
   bytes are a little-endian length/width field, with two constant anchor
   bytes earlier in it (see the response codec for the exact offsets and how
   it detects which shape is on the wire).

   - **Color** (`M=CGRAY`, `C=JPEG`): the payload is one baseline JPEG stream
     (`ff d8 ... ff d9`), but for anything over 65524 bytes (`0xfff4`) it
     arrives split into multiple blocks: every block's header gives that
     block's own payload length, pinned at the `0xfff4` sentinel while more
     blocks follow, and the true (shorter) length on the final block. A
     reader must strip each embedded header and concatenate the block
     payloads to reassemble the real JPEG -- reading the stream as if it
     were one contiguous run and just searching for the EOI marker corrupts
     it, since an embedded header can land in the middle of the
     entropy-coded data.
   - **Grayscale** (`M=GRAY64`, `C=NONE`): the payload is raw 8-bit samples,
     `width * height` bytes, and is **not** chunked the way color is even
     past the same 65524-byte size -- confirmed by a live gray scan crossing
     that boundary with no header spliced in. The mid-stream headers seen for
     color appear to be an artifact of how the device's JPEG encoder flushes
     bounded output bursts, not a general network-layer framing.

   The end-of-page, end-of-job, and cancel status bytes beyond the block
   header are not decoded by this codebase; see "Cancellation" below for how
   a cancelled scan is detected instead (a read timeout, not a status byte).

## Multi-page (ADF)

A single `ESC X` on the document feeder makes the device feed and stream
**every** page in the stack over the same connection, not just one: `ESC Q`,
source select, `ESC I`, and `ESC X` all run once per job, and the block/
payload readout that follows loops until the device signals the job is
done. This applies to a simplex feed (pages 1..N in order) and a duplex feed
(front/back pages, still delivered in order) alike; a flatbed or single-
sheet feeder scan is just the one-page degenerate case of the same framing.

Each page is an independent payload: for color, its own complete baseline
JPEG (own JFIF/DQT/SOF/DHT/EOI), chunked exactly like a single flatbed
scan's payload (see "Image data" above). After a page's payload comes a
10-byte end-of-page marker:

```
82 07 00 <pidx> 00 84 00 00 00 00
```

`pidx` is the 1-based index of the page that just ended. The 2 bytes
immediately following this marker decide what comes next:

- `80 80`: the job is done. Together with the marker above this forms the
  12-byte job-final terminator, `82 07 00 <pidx> 00 84 00 00 00 00 80 80`.
  A single-page (flatbed or one-sheet ADF) scan is simply this terminator
  with `pidx = 1`, following the one page it sent -- which is why a
  single-page reader that stops right there already works.
- Anything else: it's the next page's block header (`64 07` for color,
  `40 07`/`42 07` for the raw-gray/RLENGTH modes -- see "Image data" and
  "Row block framing" above), not a new marker. A reader must not consume
  or reinterpret these bytes as part of the marker; it must go straight
  back to parsing a block header with them still unread.

A reader must not route the 10-byte marker through the same header parser
that decodes `64`/`40`/`42`-type payload blocks: doing so consumes the 2
bytes right after the marker as part of a (bogus) 12/13-byte header,
desynchronizing the rest of the stream. Read the marker as a fixed 10-byte
count, then peek (without consuming) the next 2 bytes to decide whether to
stop or loop back for another page.

Only color/JPEG multi-page ADF framing has been confirmed against a real
capture (see PROVENANCE.md); the gray and RLENGTH (Black & White/Error
Diffusion/True Gray) modes are expected to use the same marker, since it's
block framing rather than anything payload-type-specific, but that has not
been independently verified on the wire.

## Resolution and size

- Host-facing resolutions observed: 100, 150, 200, 300, 400, 600, 1200 dpi.
  The device reports a maximum of 2400 over this path; the vendor apps cap the
  selectable list at 1200.
- The scan area is coordinate-based, so any paper size maps to an `A=` rectangle
  within the device maximum reported in the offer.

## Cancellation

- Host-initiated cancel (from the client) is a clean client-side action.
- Device-initiated cancel (pressing Stop on the printer) emits no in-band
  status: the device simply stops sending. A client must use a read timeout to
  detect the stalled stream rather than blocking indefinitely.

## Mode coverage

`CGRAY`, `GRAY64`, `TEXT`, `ERRDIF`, and `GRAY256` have all been observed on
the wire and are implemented (see "Black & White, Error Diffusion, and True
Gray" above for the latter three). No other modes are known to remain.
