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

The constants below are from `reference/brscan-modes.pcap`, a supplementary
capture of Brother iPrint&Scan (issue #4: Black & White, Error Diffusion, and
True Gray modes), taken 2026-09-02, plus one open-source cross-reference for
the RLENGTH algorithm itself (see below). Streams are in
`reference/streams/modes_*.bin`; see `reference/protocol-notes-modes.md`.

| Constant | Meaning | Source |
|---|---|---|
| `M=TEXT` / `M=ERRDIF` / `M=GRAY256` | Black & White / Error Diffusion / True Gray mode | Capture |
| `C=RLENGTH` | Per-row run-length compression | Capture |
| `S=NORMAL_SCAN` `L=0` `E=0` | iPrint&Scan's param set for the RLENGTH modes (differs from Image Capture's `E=1`, no `S=`/`L=`) | Capture |
| 12-byte row block header, `<type> 07 00 01 00 84 00 00 00 00 <len:le16>` | Per-row block framing (one block per scanline); `type` 0x40 = raw row, 0x42 = RLENGTH-compressed row (0x64 = JPEG chunk, already documented above) | Capture (`modes_text_in.bin`/`modes_errdif_in.bin`/`modes_gray256_in.bin`): every block in all three streams parses cleanly under this shape |
| RLENGTH row codec (PackBits: literal/repeat/no-op control byte) | Decompresses each row's payload to its fixed byte width | Primarily reverse-engineered from the capture: every row in `modes_text_in.bin` decodes to exactly 434 bytes under this algorithm, and 4896/4897 rows in `modes_errdif_in.bin` do likewise (see docs/PROTOCOL.md for the one anomalous row). Corroborated against `~/src/brscan`'s (`dmikushin/brscan`) `libbrscandec/brother_scandec.c`, `FUN_001063f3`'s `nInDataComp == 3` branch, present in that repo's checked-out `master` branch at commit `652e028` -- the literal/no-op/repeat control-byte roles there match what the capture already required |
| `0x82 ... 0x80` end-of-page/status marker | Terminal block after the last row | Capture: observed verbatim at the end of `modes_text_in.bin`, `modes_errdif_in.bin`, and `modes_gray256_in.bin` |

`dmikushin/brscan` is on the permitted list above (#3). `brother_scandec.c` in
that project is itself a from-scratch reimplementation of Brother's
proprietary `libbrscandec.so`, decompiled and rewritten by that project's
author (not by this one) and published under the GPLv2; this project adapts
its documented RLENGTH/PackBits algorithm under that license (see
`libbrscan/decode_rlength.h`), consistent with the clean-room policy above,
which names that project as a permitted source. No Brother binary, driver
package, or decompiler output was run or read by this project directly.

The constant below is from `reference/brscan-button.pcap`, our own capture of
the Scan-button registration/notification traffic between a Mac running
Brother's driver and the printer (issue #3), taken 2026-09-02. See
`reference/protocol-notes-button.md` for the decoded structure; the raw SNMP
Set packet is `reference/streams/button_snmp_set.bin` (git-ignored -- it
carries the real capturing Mac's LAN IP and computer name, so it and every
fixture derived from it stay out of the repository; committed tests in
`tests/snmp_register_test.cpp` use synthetic values instead, e.g.
`HOST=192.0.2.10:54925`, `USER="Test Mac"`).

| Constant | Meaning | Source |
|---|---|---|
| SNMPv1 Set, community `internal`, OID `1.3.6.1.4.1.2435.2.3.9.2.11.1.1.0` (BER `2b 06 01 04 01 93 03 02 03 09 02 0b 01 01 00`), OctetString value `TYPE=BR;BUTTON=SCAN;DURATION=<sec>;CC=1;HOST=<ip>:<port>;USER="<name>";FUNC=<FILE\|IMAGE\|OCR\|EMAIL>;APPNUM=<n>;` (APPNUM: FILE 5, IMAGE 1, EMAIL 2, OCR 3) | Registers this Mac as a Scan-button destination | Capture, `reference/streams/button_snmp_set.bin`; BER tag/length framing (SEQUENCE, community, `[3]` SET-REQUEST, varbind list) cross-checked byte-for-byte against that capture and independently against `/opt/local/bin/snmpset`'s (net-snmp 5.9.4) own encoding of the same OID/community/value over a loopback UDP capture -- both agree with `daemon/snmp_register.cpp`'s `BuildSnmpSetRegister` field-by-field aside from request-id length/value, which the protocol allows to vary |
| Button notification, UDP printer -> Mac:54925: 4-byte header `02 00 <len> 30` then ASCII `TYPE=BR;BUTTON=SCAN;USER="<name>";FUNC=<FILE\|IMAGE\|OCR\|EMAIL>;HOST=<ip>:54925;APPNUM=<n>;P1=0;P2=0;P3=0;P4=0;REGID=<id>;SEQ=<n>;` -- header byte 2 (`<len>`) equals `payload.size() + 5` (4 header bytes + 1), not the payload's raw byte length, confirmed exactly against the real capture's byte 2 (`0x7e` = 126, payload 121 bytes). ACK, Mac -> printer:54925: byte-for-byte echo of the received datagram (header included) | Scan-button press notification and its acknowledgement | Capture, `reference/streams/button_notify_hex.txt` (git-ignored -- carries the real capturing Mac's LAN IP and computer name; the identical notification and ACK lines confirm the echo behavior); the length-byte formula and full field layout were verified by parsing that real capture with `daemon/button_listener.cpp`'s `ParseNotification` outside the committed test tree. Committed tests in `tests/button_listener_test.cpp` use synthetic values instead (e.g. `USER="Test Mac"`, `HOST=192.0.2.10:54925`) |
