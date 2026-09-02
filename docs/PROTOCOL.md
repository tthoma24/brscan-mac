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

## Modes not yet captured

The vendor vocabulary includes `TEXT`, `ERRDIF` (error diffusion), and `C256`,
but only `CGRAY` and `GRAY64` have been observed on the wire. True 1-bit
black-and-white and dithered modes await a supplementary capture. See the issue
tracker.
