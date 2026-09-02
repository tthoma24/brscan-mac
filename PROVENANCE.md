# Provenance

This project reimplements Brother's network scan protocol from scratch. To
keep that claim auditable, every protocol constant in the source cites its
source here: a captured byte stream, a published open-source implementation,
or public protocol documentation.

## Clean-room policy

Permitted sources of protocol knowledge, in priority order:

1. Packet captures of traffic between a Mac running Brother's driver and the
   printer, both owned by the project author. This is black-box observation
   of network traffic, not decompilation of Brother's software.
2. Live probing of the printer.
3. Published open-source implementations: the SANE `brother_mfp` backend
   (merge request 751), `dmikushin/brscan`, and `v0lp3/mfc-j430w`.
4. The protocol notes at `brother-mfc.sourceforge.net`.

No Brother file enters this repository: not the driver package, not its
binaries, plists, color profiles, lookup tables, or icons. No disassembly or
decompiler output, and no notes transcribed from them, enter this repository.

## Constant sources

Recorded as protocol work lands. Each entry names a constant and its source.

All constants below come from `reference/brother-scan.pcap`, our own capture of
network traffic between a Mac running the vendor driver and the printer, taken
2026-09-02. The decoded structure is documented in
[docs/PROTOCOL.md](docs/PROTOCOL.md); the raw command streams are in
`tests/fixtures/`.

| Constant | Meaning | Source |
|---|---|---|
| `+OK 200` | Ready greeting on TCP 54921 | Live probe of the printer, 2026-09-01; capture |
| `-NG 401` | Busy greeting on TCP 54921 | brother-mfc.sourceforge.net/network.html |
| `0x1b 0x51` (ESC Q) | Session init | Capture, `tests/fixtures/requests/all-commands.bin` |
| `0x1b 0x49` (ESC I) | Negotiate; returns offer | Capture |
| `0x1b 0x53` (ESC S) + `FB` | Select flatbed source | Capture |
| `0x1b 0x44` (ESC D) + `ADF` | Select document feeder source | Capture, `adf-color-300-*.bin` |
| `0x1b 0x58` (ESC X) | Execute scan | Capture |
| `0x1b 0x52` (ESC R) | Reset/release at teardown | Capture |
| `0x80` | Command terminator | Capture |
| `M=CGRAY` / `M=GRAY64` | Color / grayscale mode | Capture |
| `C=JPEG` / `C=NONE` | Compression: color=JPEG, gray=raw | Capture |
| `D=SIN` / `D=DUP` | Simplex / duplex (ESC X param) | Capture, `adf-color-300-duplex.bin` |
| `A=x0,y0,x1,y1` | Scan area in pixels | Capture, `color-300-crop.bin` |
| `B=50` `N=50` `J=MID` `P=0` `E=1` `G=0` | Brightness, contrast, JPEG quality, page/edge/gamma flags | Capture |
| Offer `xdpi,ydpi,flag,?,xmaxpx,?,ymaxpx,` | ESC I reply format | Capture, `responses/color-scan-head.bin` |
