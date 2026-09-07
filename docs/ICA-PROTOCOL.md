<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Image Capture (ICA) device-module interface

This documents the macOS Image Capture *device-module* interface the Plan 2 ICA
module (`ica-module/`) implements, reconstructed clean-room. Apple never
published narrative docs for the third-party ICA scanner device-module protocol;
this file records what was established from the public SDK headers, the device's
own black-box behavior (live `os_log` traces), and — for interface facts only —
the sequence used by Apple's `VirtualScanner` sample and two shipping
open-source modules. See [Provenance](#provenance-and-clean-room) below and
`PROVENANCE.md`. No vendor or Apple source was copied.

A module is a background-only `.app` in `/Library/Image Capture/Devices/` that
`icdd` (the Image Capture discovery daemon) launches and drives through the C
callback table in `ICADevices.framework/Headers/ICD_ScannerCalls.h`
(`gICDScannerCallbackFunctions`, entered via `ICD_ScannerMain`). It links
`ICADevices.framework`. Ad-hoc signing suffices for local development;
distribution needs Developer-ID + notarization.

## Discovery and binding

`icdd` browses Bonjour and binds a device to a module by matching the module's
`Contents/Resources/DeviceMatchingInfo.plist` against the device's advertised
TXT record, via icdd's `compareBonjourDeviceModuleDictionary:withBonjourTXTRecord:`.
The match dict must live under a top-level `BonjourNetwork` dict keyed by the
Bonjour service type, and must carry an `ICABonjourTXTRecordKey` sub-dict of
TXT key/value pairs to compare (a bare `device type = scanner` does not bind).

- Service type: `_scanner._tcp.` (the Brother raw-scan protocol, port 54921).
- `ICABonjourTXTRecordKey`: `{ mdl = MFC-J6920DW; mfg = Brother; }` — the
  device's own publicly-advertised TXT values (captured with `dns-sd -L`).
- Plus `device type = scanner` and `device events = ( scan )`, and a top-level
  `Version = 1.0`.

A `DeviceInfo.plist` is also required in `Resources/`. Symptom of a missing TXT
match dict: the device is browsed but never bound (Image Capture shows 0
devices; the module is never launched).

## Callback lifecycle

Order observed live: `ICD_ScannerOpenTCPIPDevice` (fill the device
`ScannerObjectInfo`: `objectType = kICADevice`, `objectSubtype =
kICADeviceScanner`, a stable `uniqueID`, and `privateData` = per-connection
context) → `ICD_ScannerGetObjectInfo` (object-tree walk) → `ICD_ScannerOpenSession`
→ `ICD_ScannerGetParameters` → `ICD_ScannerPeriodicTask` (polled) → then, per
scan, `ICD_ScannerSetParameters` → `ICD_ScannerStart`. Teardown:
`ICD_ScannerCloseSession` / `ICD_ScannerCloseDevice` / `ICD_ScannerCleanup`.

- Returning a *zeroed* device object from `OpenTCPIPDevice` yields
  `-21345` (`ICReturnConnectionFailedToOpen`); filling it clears that.
- **Object tree:** `ICD_ScannerGetObjectInfo` is walked for index 0, 1, 2 …
  terminating on `kICAIndexOutOfRangeErr`. The device object must report a
  finite tree: exactly one child at index 0 (a scan object, `kICAFile` /
  `kICAFileImage`), end-of-list at index ≥ 1; any non-device parent is a leaf.
  Reporting the device as empty (error for every index) makes icdd re-drive the
  callback in an unbounded loop.
- **Polled callbacks** (`GetObjectInfo`, `GetPropertyData`, `Status`,
  `PeriodicTask`) must not `os_log` per call, or they flood the unified log.
- `icdd` does not call `ICD_ScannerGetPropertyData` for capability rendering.

## Capability schema (`ICD_ScannerGetParameters`)

`icdd` hands `theDict` **empty** and the module fills it — the module dictates
the schema (there is no pre-seed). The keys are the **TWAIN capability
vocabulary** (`ICAP_*`), not any `kICAScanner…` constants (which do not exist on
the module side). Everything lives under a single top-level `device` dict:

```
theDict["device"] = {
  // capability entries for the SELECTED functional unit, FLAT in this dict:
  "ICAP_XRESOLUTION":  { type, value, current, default },
  "ICAP_YRESOLUTION":  { ... },
  "ICAP_BITDEPTH":     { ... },   // values are the bit count (1, 8)
  "ICAP_PIXELTYPE":    { ... },   // 0=BW, 1=gray, 2=RGB
  "ICAP_PHYSICALWIDTH":  { ... }, // TWON_ONEVALUE, INCHES (see Geometry)
  "ICAP_PHYSICALHEIGHT": { ... },
  "ICAP_SUPPORTEDSIZES": { ... }, // array of ICScannerDocumentType (per-unit set)
  "ICAP_UNITS":        { ... },   // measurement unit; inches=0 … pixels=5
  "CAP_FEEDERENABLED": { ... },   // TWON_ONEVALUE; tracks the selected unit
  "CAP_DUPLEX":        { ... },   // feeder: TWON_ONEVALUE nonzero; flatbed: 0
  "CAP_DUPLEXENABLED": { ... },   // feeder only: TWON_ONEVALUE 0 (host-writable)
  "CAP_AUTOFEED":      { ... },   // feeder only: TWON_ONEVALUE 1
  "functionalUnits": {            // ONLY these two keys
    "availableFunctionalUnitTypes": [ 0, 3 ],
    "selectedFunctionalUnitType": 0
  }
}
```

Each capability entry is a TWAIN container: `{ "type": "TWON_ENUMERATION" |
"TWON_ONEVALUE", "value": <array | scalar>, "current": <v>, "default": <v> }`
(a numeric range adds `min`/`max`/`stepSize`). A top-level `functionalUnits`
key (instead of the `device` wrapper) renders no controls — that wrapper is
mandatory.

Enum values (from `ImageCaptureCore/ICScannerFunctionalUnits.h`):
`ICScannerFunctionalUnitType` flatbed=0, positiveTransparency=1,
negativeTransparency=2, documentFeeder=3; `ICScannerPixelDataType` BW=0, gray=1,
RGB=2; `ICScannerMeasurementUnit` inches=0, cm=1, picas=2, points=3, twips=4,
pixels=5; `ICScannerDocumentType` USLetter=3, USLegal=4, A5=5, USLedger=9,
USExecutive=10, A3=11 (values are non-contiguous — read from the header).

Client mapping (`icdd` translates the module dict into the typed
`ICScannerDevice` / `ICScannerFunctionalUnit` object graph — there is no
guaranteed 1:1 key identity): `ICAP_XRESOLUTION` → `supportedResolutions` /
`resolution`; `ICAP_BITDEPTH` → `bitDepth`; `ICAP_SUPPORTEDSIZES` →
`supportedDocumentTypes`; `ICAP_UNITS` → `measurementUnit`.

### Per-unit supported sizes (`ICAP_SUPPORTEDSIZES`)

The size set is scoped to the selected unit (`scan_parameters.mm::BuildUnit`).
The Brother ADF feeds 148–297 mm wide × 148–431.8 mm long (A5 up through
A3/Ledger), so:

- **Feeder** — the full document set that fits that envelope: `USLetter (3)`,
  `USLegal (4)`, `A4 (1)`, `USLedger (9)`, `A3 (11)`, `A5 (5)`,
  `USExecutive (10)`, `JISB5 (2)`, `JISB4 (38)`. No platten `Default`; no
  `4R`/`BusinessCard` (both under the 148 mm ADF minimum).
- **Flatbed** — that set plus the platten `Default (0)`, `4R (62)` (4×6 photo,
  from the `PHOTO` token) and `BusinessCard (53)` (from the `BCARD` token).

`DocumentTypeForPaperToken` maps `PHOTO → 4R (62)` and `BCARD → BusinessCard
(53)`; those two tokens are advertised on the flatbed only. `JISB5`/`JISB4`
carry no `daemon/paper_size.cpp` geometry (the host supplies the scan rectangle
per document type) and are advertised on both units. (A5/Executive/JIS B4 on the
*feeder* are inferred from the spec envelope, not from a live capture.)

## Scan request (`ICD_ScannerSetParameters`)

The host passes one top-level `userScanArea` dict of TWAIN `{type,value}`
entries plus scan-area and destination fields. Observed keys:
`ICAP_XRESOLUTION`/`YRESOLUTION`, `ICAP_PIXELTYPE`, `ICAP_BITDEPTH`,
`ICAP_UNITS`, `offsetX`/`offsetY`/`width`/`height` (in `ICAP_UNITS`),
`ColorSyncMode`, `"scan mode"` (`overview` for a preview pass),
`useOverviewImageAsFinalImage`, `progressNotificationWithData`.

Read the scan area in the request's own `ICAP_UNITS` and convert to pixels at
the chosen DPI (inches × dpi; cm × dpi / 2.54; pixels pass through). The host
switches its unit to whatever the module advertises for the platen.

### Scan-area registration (ADF centering)

The host always sends a **0-based** rectangle (`offsetX = 0`), e.g. Letter at
300 dpi comes in as `(0, 0, 2550, 3300)`. Where that rectangle registers on the
device differs by source:

- **Flatbed** — **corner-registers** at `x0 = 0`. The 0-based rectangle is
  correct as sent, so the module passes it straight through.
- **ADF (document feeder)** — **center-registers** the page across the feeder's
  full sensor width (`xmax = 3472 px @300 dpi`, the ADF width baked into
  `daemon/paper_size.cpp`'s `kPaperTable`; A3 fills it, `0 … 3472`). The device
  scans exactly the horizontal rectangle it is handed *within that sensor*, so a
  0-based rectangle scans `0 … width`: the left `(3472 − width)/2` px is blank
  sensor margin and the page's right edge is cut off.

So for an ADF scan the module re-centers the requested width horizontally before
building `brscan::Params` (`scan_translate.cpp::TranslateScanParams`, via
`CenteredAdfX0`): `sensor = 3472 × dpi / 300`, `x0 = max(0, (sensor − width)/2)`,
`x1 = x0 + width` (clamped to `x0 = 0` when `width ≥ sensor`); `y0`/`y1` are left
alone. This reproduces the captured center-registration offsets (Letter → 480 ≈
478, A4 → 512 ≈ 513, Legal → 480 ≈ 478, Ledger → 104 ≈ 103, A3 → 0). The
full-area default (`{0,0,0,0}`, "full offered area") is left untouched, and the
flatbed path is unchanged.

### Source selection (flatbed vs feeder)

The host does **not** always echo `selectedFunctionalUnitType` in the scan
request. When the user picks the Document Feeder and scans, the request carries
`CAP_FEEDERENABLED == 1` but **no** functional unit — so deriving the source from
the functional unit alone silently ran every ADF scan as flatbed. Resolve the
source by precedence (`scan_translate.cpp::ScanRequestFromIcap`, logged as
`source=<signal>`):

1. an explicit `functionalUnit` / `selectedFunctionalUnitType` in the request;
2. else `CAP_FEEDERENABLED == 1` → the document feeder (`brscan::Source::kAdf`);
3. else the unit the module tracked from an earlier unit-switch `SetParameters`
   (`DeviceContext.selectedFunctionalUnit`);
4. else flatbed.

### Duplex (2-sided)

Duplex is a document-feeder capability only. The SDK models it on
`ICScannerFunctionalUnitDocumentFeeder` as `supportsDuplexScanning` (readonly)
and `duplexScanningEnabled` (readwrite) — `ICScannerFunctionalUnits.h` lines
802–811 — which map onto the TWAIN-derived dict as `CAP_DUPLEX` (duplexer type;
`TWDX_NONE=0`, `TWDX_1PASSDUPLEX=1`, `TWDX_2PASSDUPLEX=2`) and `CAP_DUPLEXENABLED`
(the enable toggle).

**The container shape is load-bearing.** Advertising these as
`TWON_ENUMERATION [0,1]` with current 0 renders **no** control: an enumeration
resolves to `value[current] = 0 = TWDX_NONE`, so the framework sets
`supportsDuplexScanning = NO`. A 2-sided toggle appears only when they are
`TWON_ONEVALUE`. So the **feeder** advertises `CAP_DUPLEX = OneValue(2)` — the
NONZERO value is what maps to `supportsDuplexScanning = YES` (2 =
`TWDX_2PASSDUPLEX`; 1-vs-2 is immaterial to the flag) — and
`CAP_DUPLEXENABLED = OneValue(0)`, the read/write toggle the host flips to 1
(→ `duplexScanningEnabled`), plus `CAP_AUTOFEED = OneValue(1)`. The **flatbed**
has no duplexer, so it keeps `CAP_DUPLEX = OneValue(0)` (no control).

On the scan request the host echoes `CAP_DUPLEXENABLED != 0`; the read side
(`ScanRequestFromIcap`) treats any of `duplex` (bool), `CAP_DUPLEX != 0`, or
`CAP_DUPLEXENABLED != 0` as 2-sided. Duplex is honoured only when the resolved
source is the feeder.

### Transfer mode (host-selected, per scan)

`ICScannerTransferMode` fileBased=0 (default), memoryBased=1. The mode is
signalled by the presence of destination keys, not a flag:

- **File-based** (final scan): the request carries `document folder`,
  `document name`, `document extension`, `document format` (a UTI), and
  `ICSecurityScopedWrappedURL`. The module encodes the page, writes it to
  `<document folder>/<document name>.<document extension>`, and reports that
  exact path. `ICSecurityScopedWrappedURL`'s value is an
  `NSSecurityScopedURLWrapper` (in no public header); unwrap it to the real
  security-scoped `NSURL` (its `-url` selector) and bracket the write with
  `startAccessingSecurityScopedResource` / `stop…`.
- **Memory/overview** (preview): no destination keys; the module hands the
  pixels back inline (see below). `"scan mode" = overview`.

## Notification / completion sequence

The module posts, per scan, in order (via `ICDSendNotification` /
`ICDSendNotificationAndWaitForReply`, `ICASendNotificationPB`):

1. `kICANotificationTypeDeviceStatusInfo` + `kICANotificationSubTypeKey` =
   `kICANotificationSubTypeWarmUpStarted`, then a second with
   `…WarmUpDone`.
2. `kICANotificationTypeScanProgressStatus` — **one per decoded band, streamed
   LIVE** (Task 18b). `brscan::RunScan`'s 4-argument streaming overload fires an
   `on_band` callback as rows decode off the wire (16-row bands); each band is
   packed with `ICDAddImageInfoToNotificationDictionary` using the **full page**
   `width`/`height` and the band's `dataStartRow` / `dataNumberOfRows` /
   `dataSize`, with `bytesPerRow` = the `DescribeBuffer` stride. The host
   accumulates the bands into its overview/preview image, so the preview fills
   top-to-bottom with a moving progress bar **during** the scan (not just at the
   end — the earlier whole-image single-chunk send rendered only on completion).
   Sent with `ICDSendNotificationAndWaitForReply`; a cancel is
   `replyCode == userCanceledErr`, which makes `on_band` return `false` so
   `RunScan` stops reading promptly and returns `Status::kCancelled` (a true
   mid-scan cancel). The image-info args (`width`/`height`/`bytesPerRow`/
   `dataSize`) must exactly match the band buffer or the host renders nothing,
   silently — `DescribeBand` (`ica-module/buffer_descriptor.h`) computes them and
   enforces the per-band stride guard (`band.size == stride * num_rows`).
   `ICDAddBandInfoToNotificationDictionary` (the BAND-info packer) is NOT used:
   the host's preview accumulator consumes the Image-info keys, not the Band-info
   keys (former "Defect A").
3. `kICANotificationTypeScannerPageDone` — **file transfer only.** For the
   **memory/overview** path the streamed bands are the complete delivery, so
   there is no whole-image send and no per-page `ScannerPageDone`. For **file
   transfer**, after `RunScan` returns the whole page (still accumulated in the
   `out` vector while bands streamed) is encoded to the destination and this is
   posted. Keys: `kICANotificationICAObjectKey` = the **device** object
   (`deviceObjectInfo->icaObject`, NOT the `ICD_ScannerStartPB::object`
   trigger), `kICANotificationTypeKey`, and
   `kICANotificationScannerDocumentNameKey` = the exact destination path. Sent
   with plain `ICDSendNotification`.
4. `kICANotificationTypeScannerScanDone` — one per scan, ends the job. Keys:
   `kICANotificationICAObjectKey` (device object) + `kICANotificationTypeKey`.
   Plain `ICDSendNotification`. A clean host cancel (`RunScan` returned
   `kCancelled`) also ends with a clean `ScannerScanDone` (no file written, no
   leaked scoped URL / transport).

`kICANotificationICAObjectKey` must be the **device object's** `icaObject` on
every notification so the host correlates them to the session; using the Start
trigger object instead is a likely cause of a scan that never completes in the
UI.

### Object registration

`ICDScannerNewObjectInfoCreated(deviceObjectInfo, 0, &newObj)`
(`ICD_ScannerCalls.h`) registers a scanned object — but Apple's sample calls it
**only** for the in-memory / remote-client case (`document folder == NULL`).
For **local file transfer** no object is registered; the file write +
`ScannerPageDone`(path) + `ScannerScanDone` is what completes the job. The
generic `ICDNewObject` (`ICADevice.h`, deprecated 10.7) returns `unimpErr` (-4)
for a scanner/TCP-IP module — a dead end; do not use it.

## Scan execution over `libbrscan`

Host-initiated scanning is the normal driver flow: build a `brscan::Params`
from the request (`button_flow = false`; the driver flow's job-final terminator
is `0x80 0x80`, unlike the button flow's single `0x80`), open a
`brscan::TcpTransport(ip, 54921)`, and run `brscan::RunScan`'s 4-argument
streaming overload with an `on_band` callback (Task 18b): bands stream to the
host live for the progress bar (see the notification sequence above), while the
overload still accumulates whole pages into `out`. The bands carry already-
decoded, host-ready pixels; the whole pages in `out` keep `kRgb` as native JPEG
(decoded via `brscan::DecodeJpeg` only for the file-transfer encode) and
`kGray`/`kBitonal` as raw samples. `DescribeBuffer` / `DescribeBand`
(`ica-module/buffer_descriptor.h`) compute bpp/stride/size/color space. The
device allows one connection at a time, and a bounded connect retry handles a
lingering-socket race.

**Scan execution runs SYNCHRONOUSLY on icdd's callback thread, not a background
thread (Task 16).** `ICD_ScannerStart` performs the entire scan inline —
transport connect, `RunScan`, per-page notifications, and the final
`ScannerScanDone` — and returns only after `ScannerScanDone`. ICA notification
delivery/reply is bound to the icdd connection runloop on the callback thread, so
notifications posted from any other thread return `noErr` yet never reach the
host: an earlier background-thread implementation is exactly why the overview
never rendered, the UI stuck on "Scanner is warming up", and the scan never
completed. Blocking `Start` for the whole scan is correct and expected by icdd
(matching Apple `VirtualScanner` and the other reference modules). There is no
worker thread, no join, and no cross-thread state (the transfer settings are
locals in `Start`, since `SetParameters` and `Start` alternate on the one
thread). Cancel is now **mid-scan** (Task 18b): a `ScanProgressStatus` reply of
`userCanceledErr` makes the `on_band` callback return `false`, so `RunScan`
stops reading promptly and returns `Status::kCancelled`; the module then writes
no file and finishes with a clean `ScannerScanDone`. This supersedes the earlier
page-boundary-only cancel (the old whole-page hand-back could only observe a
cancel between pages).

## Geometry (platen extent) — OPEN

`ICAP_PHYSICALWIDTH`/`ICAP_PHYSICALHEIGHT` are advertised in **inches**
(`TWON_ONEVALUE` doubles); physical size in pixels is dpi-dependent and
mis-scales the host's platen canvas. The MFC-J6920DW flatbed is A3-capable, so
A3 and Ledger are valid flatbed sizes. **Known issue:** the platen still renders
larger than it should relative to a Letter selection; the current advertisement
uses a phantom union (A3 width × Ledger height) rather than one real glass
rectangle. Tracked for a follow-up.

## Re-test loop (device-in-the-loop)

`icdd` launches the module as a child process that survives `killall icdd`, so a
re-test must also reap it. After building:

```
sudo ica-module/install-loadspike.sh install
sudo killall BrscanICALoadSpike 2>/dev/null      # reap the stale child
sudo rm -f "/Library/Image Capture/Devices/deviceInfoCacheV2.plist"
killall icdd 2>/dev/null
# verify the INSTALLED binary carries your change:
strings "/Library/Image Capture/Devices/BrscanICALoadSpike.app/Contents/MacOS/BrscanICALoadSpike" | grep "<a string you added>"
log stream --predicate 'subsystem == "me.tthoma24.brscan.ica"' --style compact --info --debug
```

Then open Image Capture and select `Brother MFC-J6920DW`.

## Provenance and clean-room

The interface facts here (callback signatures, `kICANotification*` /
`ICAP_*` / enum constant names, the `theDict["device"]` shape, and the
notification order) come from:

- The public SDK headers in `ICADevices.framework` and
  `ImageCaptureCore.framework` (Apple SDK; reference only).
- The device's own black-box behavior, captured in this module's `os_log`
  traces and `dns-sd`.
- The required *sequence* and which key goes on which notification, cross-checked
  against Apple's `VirtualScanner` sample (Apple Sample Code License —
  proprietary, GPL-incompatible), `chrspeich/SaneNetScanner` (no license
  declared — all-rights-reserved), and `craiglinton/scansnaps1500m`
  (Apple-`VirtualScanner`-derived). These were read for **interface facts
  only**; no source bodies were reproduced or adapted, and none enter this
  repository. See `PROVENANCE.md`.
