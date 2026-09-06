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

The constant below is from `reference/streams/s0_in.bin` (the same capture
cited above), re-examined for its ADF regions (tests 7 and 8: an ADF simplex
and an ADF duplex scan). See `reference/protocol-notes-adf-multipage.md` for
the full byte-level decode.

| Constant | Meaning | Source |
|---|---|---|
| 10-byte end-of-page marker `82 07 00 <pidx> 00 84 00 00 00 00`, followed by either `80 80` (job-final, forming the 12-byte terminator already documented for the single-page case) or the next page's block header | Delimits pages within one multi-page ADF job (one `ESC X` streams the whole stack) | Capture, `reference/streams/s0_in.bin` tests 7 (ADF simplex, 3 pages) and 8 (ADF duplex, 4 pages): every inter-page boundary in both regions parses cleanly under this shape. Only color/JPEG multi-page is capture-confirmed; gray and RLENGTH multi-page framing is assumed identical (block framing, not payload-specific) but not independently captured |
| Block-header `pidx` at byte[3] (`64 07 00 <pidx> 00 84 ...`) is the 1-based page index; a **duplex** color feed interleaves the two sides' chunks and tags each with its `pidx` | Lets a reader de-interleave a duplex job by routing each chunk to a per-`pidx` accumulator | Capture, `reference/streams/s0_in.bin` test 8 (ADF duplex): headers alternate `pidx 2,1,2,1,…` up to `EOP(pidx=1)`, then `3,2,3,2,…` up to `EOP(pidx=2)`, etc.; de-multiplexing by `pidx` yields exactly 4 independent baseline JPEGs, all decoding cleanly at 2560×3252. Reading the interleaved stream one page at a time instead concatenates two JPEGs (two `ff d8` SOIs) and fails to decode. Only color/JPEG duplex interleaving is captured; gray/RLENGTH duplex is uncaptured and its interleaving (if any) unconfirmed. The interleaved capture itself stays git-ignored (scanned content); the committed regression uses synthetic interleaved JPEGs |

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

The constants below are from `reference/brscan-button-options.pcap`, our own
capture of the button config command pushed on the scan connection (TCP
54921) right after `ESC K`, for a range of LCD panel settings (Task 1d.1),
taken 2026-09-04. No Brother software consulted. This capture, and every
byte string decoded from it, carries no device identity (no real Bonjour
name, MAC, or IP) and no scan image content, so the payload strings below
are committed verbatim as test fixtures in `tests/button_config_test.cpp`,
unlike the button-notification fixtures above.

| Constant | Meaning | Source |
|---|---|---|
| Config-command frame, printer -> Mac (TCP 54921), sent after `ESC K` (`1b 4b`): `30 <len> 00` then ASCII `\n`-separated `KEY=VALUE` lines (trailing `\n` included); `<len>` (byte 1) equals the payload's byte count (bytes after byte 2) | Pushes the LCD panel's current scan-button settings (destination, mode, dpi, paper, output format, panel toggles) ahead of the scan itself | Capture, `reference/brscan-button-options.pcap`; frame shape and every `KEY=VALUE` line confirmed field-by-field across the full range of panel settings captured (all four destinations; every paper size incl. photo/business-card; every output-type token; mode/duplex/duplex-edge/skip-blank/remove-background at all three levels/high-speed) |
| `F=` (FILE\|IMAGE\|OCR\|EMAIL), `D=` (SIN\|DUP), `E=` (LON\|SHO), `R=<n>`, `M=` (CGRAY\|TEXT), `P=` (LETTER\|LEGAL\|A4\|LEDGER\|A3\|A5\|EXECUTIVE\|PHOTO\|BCARD), `A=0`, `T=` (PDF(Image)\|MULTI-TIFF\|JPEG\|TXT\|HTML\|RTF), `W=` (0\|1), `G=` (0\|1, omitted entirely on OCR routes), `L=` (64\|128\|192, present only when `G=1`), `X=` (0\|1) | The config command's field vocabulary; decoded into `daemon/button_config.h`'s `ButtonConfig` (paper/output-type tokens kept raw -- mapping to a scan area / `OutputFormat` is a later task) | Capture, `reference/brscan-button-options.pcap`; see `docs/BUTTON.md`'s "Config command" section for the full field table |
| Per-`P=`-token scan area at 300 dpi (`{x0,0,x1,y1}`): `LETTER {478,2990,3253}`, `LEGAL {478,2990,4153}`, `A4 {513,2961,3461}`, `LEDGER {103,3367,5053}`, `A3 {0,3472,4913}`, `A5 {0,1712,2433}`, `EXECUTIVE {0,2128,3103}`, `PHOTO {0,1168,1753}`, `BCARD {0,1024,661}` (Task 1d.2) | The physical scan area the printer actually uses for each paper size the LCD panel offers, at the config command's captured 300 dpi; `daemon/paper_size.h`'s `AreaForPaper` stores these 9 tuples verbatim and scales them linearly to other dpi rather than deriving them from each paper's public nominal dimensions plus a margin formula | Black-box network observation, `reference/brscan-button-options.pcap` (same capture as the row above, re-examined for the scan-area bytes the printer sent for each paper size on the scan connection, TCP 54921). These tuples carry no device identity or scan-image content, so they're committed verbatim as test fixtures in `tests/paper_size_test.cpp` |
| Scan-button wire flow (Task 1d.3): `ESC K` opener `1b 4b 0a 80` (in place of `ESC Q`, with NO `ESC S`/`ESC D` source-select); `ESC I` carries `S=NORMAL_SCAN` for color too; `ESC X` uses the RLENGTH-style field order (`R,M,C,J,B,N,A,D,S=NORMAL_SCAN,P=0,E=0,G,L`) with `C=JPEG` for color (`C=RLENGTH` for BW) and `G`/`L` from remove-background. Example (color Letter@300 simplex): `R=300,300\nM=CGRAY\nC=JPEG\nJ=MID\nB=50\nN=50\nA=478,0,2990,3253\nD=SIN\nS=NORMAL_SCAN\nP=0\nE=0\nG=0\nL=0\n` | The host->device command bytes the scan-button session sends; encoded by `EncodeButtonQuery`/`EncodeInfo(button_flow=true)`/`EncodeExecute(button_flow)` and driven by `RunButtonScan` (`libbrscan/scanner.cpp`). From `ESC X` onward the block/payload readout is identical to `RunScan` (shared `RunReadout`) | Black-box network observation, `reference/brscan-button-options.pcap` (same capture as the config-command rows above, re-examined for the host->device `ESC K`/`ESC I`/`ESC X` bytes on TCP 54921); decoded in `reference/protocol-notes-button-options.md`. These command byte strings carry no device identity or scan-image content, so they're asserted as literal fixtures in `tests/command_test.cpp` and `tests/scanner_test.cpp` |

## Image Capture (ICA) host-interface facts

The Plan 2 Image Capture device module (`ica-module/`) talks to macOS's Image
Capture, not to the printer, so its constants are not Brother protocol bytes but
facts about **Apple's** ICA device-module interface. These were reconstructed
clean-room from three permitted kinds of source, and are documented in
[docs/ICA-PROTOCOL.md](docs/ICA-PROTOCOL.md):

1. The **public SDK headers** shipped with macOS —
   `ICADevices.framework/Headers/*` and `ImageCaptureCore.framework/Headers/*`
   (Apple SDK, reference only) — for callback signatures, constant/key names
   (`kICANotification*`, `ICAP_*`, `TWON_*`, `ICScanner*` enum values), and
   struct layouts.
2. **Black-box observation** of the running system — this module's own `os_log`
   traces of what `icdd` sends it, and `dns-sd` for the device's advertised
   Bonjour TXT record.
3. For the required **call sequence** only (which notification types, in what
   order, with which keys), cross-checking against Apple's `VirtualScanner`
   sample and two shipping open-source modules. These were read for interface
   facts, not copied.

No Apple source, sample-code body, or proprietary text enters this repository;
no decompilation was performed. The `ICAP_*` / `TWON_*` names are the TWAIN
Working Group's capability vocabulary, which Apple's interface reuses.

| Constant / fact | Meaning | Source |
|---|---|---|
| `_scanner._tcp.` + `ICABonjourTXTRecordKey {mdl=MFC-J6920DW; mfg=Brother}` in `DeviceMatchingInfo.plist` | Bonjour match that binds the device to this module (via icdd's `compareBonjourDeviceModuleDictionary:withBonjourTXTRecord:`) | SDK header symbols; device's own `dns-sd -L` TXT; match interface shape from Apple's `AirScanScanner.app` plist (public plugin interface, not source) |
| `gICDScannerCallbackFunctions` / `ICD_Scanner*` table (`OpenTCPIPDevice`, `GetObjectInfo`, `OpenSession`, `GetParameters`, `SetParameters`, `Start`, …) | The callbacks icdd drives | `ICADevices.framework/Headers/ICD_ScannerCalls.h` |
| `theDict["device"]` capability schema: `ICAP_XRESOLUTION/YRESOLUTION/BITDEPTH/PIXELTYPE/PHYSICALWIDTH/PHYSICALHEIGHT/SUPPORTEDSIZES/UNITS` + `CAP_FEEDERENABLED/CAP_DUPLEX`, each a `{type:TWON_ENUMERATION\|TWON_ONEVALUE,value,current,default}` container, plus `functionalUnits{availableFunctionalUnitTypes,selectedFunctionalUnitType}` | What `ICD_ScannerGetParameters` must fill so Image Capture renders scan controls | SDK header constant names; the `device`-wrapper + flat-caps *shape* confirmed as an interface fact against `VirtualScanner`/`SaneNetScanner`/`scansnaps1500m` (facts only); the empty-incoming-`theDict` behavior observed live |
| `ICScannerFunctionalUnitType` flatbed=0/documentFeeder=3; `ICScannerPixelDataType` BW=0/gray=1/RGB=2; `ICScannerMeasurementUnit` inches=0…pixels=5; `ICScannerDocumentType` USLetter=3/USLegal=4/A5=5/USLedger=9/USExecutive=10/A3=11 | Enum values used in the capability dict / scan request | `ImageCaptureCore/Headers/ICScannerFunctionalUnits.h` |
| SetParameters `userScanArea` keys: `ICAP_*` selection, `offsetX/offsetY/width/height` (in `ICAP_UNITS`), `document folder/name/extension/format`, `ICSecurityScopedWrappedURL` (an `NSSecurityScopedURLWrapper`, unwrapped via `-url`), `progressNotificationWithData`, `"scan mode"=overview` | The scan request the host sends; drives file-vs-memory transfer and the scan rectangle | Live `os_log` of the incoming `theDict` (black-box); key names cross-checked against the reference modules |
| Notification sequence: `kICANotificationTypeDeviceStatusInfo`(`kICANotificationSubTypeWarmUpStarted`/`WarmUpDone`) → `kICANotificationTypeScanProgressStatus` (`ICDAddImageInfoToNotificationDictionary` whole image / `ICDAddBandInfoToNotificationDictionary` bands, `ICDSendNotificationAndWaitForReply`) → `kICANotificationTypeScannerPageDone` → `kICANotificationTypeScannerScanDone`, each carrying `kICANotificationICAObjectKey` = the device object (`deviceObjectInfo->icaObject`); file transfer adds `kICANotificationScannerDocumentNameKey` = the written path | How a scanned page / overview image and completion are handed back to the host | SDK header symbols (`ICAApplication.h`, `ICADevices.h`); required order + key placement confirmed as interface facts against `VirtualScanner`/`SaneNetScanner`/`scansnaps1500m` |
| `ICDNewObject` returns `unimpErr` (-4) for a scanner/TCP-IP module; object registration for the in-memory case is `ICDScannerNewObjectInfoCreated` (not used for local file transfer) | Object-registration behavior | `ICADevice.h` / `ICD_ScannerCalls.h` signatures + live return code (black-box) |
