# Plan 2 — Image Capture (ICA) device module (design)

This document is the design and task plan for Plan 2: making the Brother
scanner appear and scan inside **Image Capture** and **Printers & Scanners**
through a native macOS Image Capture Architecture (ICA) device module. The
module is a background-only `.app` bundle that lives in
`/Library/Image Capture/Devices/`, links `ICADevices.framework`, matches the
device over Bonjour, and drives scans through the existing `libbrscan` core
(`RunScan` / `Params`). No implementation code is proposed here — this is a
design, a task breakdown, and a set of open decisions for the human to rule on
before any code is written.

The scan button is out of scope: Plan 1b already handles it through Brother's
own SNMP registration and the `brscan-scand` daemon. Plan 2 is only about
*host-initiated* scanning from Apple's scan UI.

> **Clean-room note.** This design was written without reading Brother or Apple
> source. It describes only the public shape of `ICADevices.framework` and the
> Image Capture bundle layout, and it flags every point that needs empirical
> confirmation against the macOS SDK headers or a live Image Capture host. The
> device identity used throughout is the synthetic name `BRW00AABBCCDDEE` (the
> `.local` Bonjour name a Brother device derives from its MAC address); no real
> device identity appears here.

## Scope

In scope:

- A background-only ICA device module bundle that Image Capture / Printers &
  Scanners loads, matches to the device, and uses to run a scan.
- Discovery / matching of the device's `_scanner._tcp` Bonjour service, and
  binding a matched service to a `TcpTransport` on port 54921.
- Mapping Apple's scanner parameters (resolution, color mode, source, scan
  area, paper size, brightness/contrast) onto `brscan::Params`, and handing
  decoded pages back to the framework in the buffer format it expects.
- Standard paper-size enumeration within the device maximum, exposed as the
  size picker in the scan UI.
- A CMake build target that produces and installs the bundle, plus the
  signing/notarization story.

Out of scope:

- The scan button (Plan 1b already covers it).
- The `brscan-scand` daemon, the CLI, and the Plan 1e config app — Plan 2 adds
  a new front end over `libbrscan` and changes none of them.
- The eSCL / AirSane route (Plan 3). Plan 2 and Plan 3 are alternative ways to
  reach the same Image Capture visibility; the "Open decisions" section weighs
  them against each other.

## What an ICA device module is (and what is uncertain)

Image Capture on macOS separates the **client** API from the **module**
(driver) API:

- **Client side — `ImageCaptureCore.framework`.** This is what Image Capture,
  Printers & Scanners, and third-party apps consume: `ICDeviceBrowser`,
  `ICScannerDevice`, `ICScannerFunctionalUnitFlatbed` /
  `…DocumentFeeder`, `ICScannerBandData`. Plan 2 does **not** build against
  this; it is the API our module serves, not the API we call.
- **Module side — `ICADevices.framework`.** A device module is a bundle that
  exports a fixed set of C callback entry points (historically named
  `ICD_Scanner…`) which the Image Capture host process calls to open a
  session, negotiate parameters, run the scan, and stream image data back. The
  framework supplies helper functions for building the object/property tree the
  host reads and for posting notifications back to the host.

The module bundle is a background app: Brother's and other vendors' current
macOS scanner drivers still ship exactly this way — an `.app` in
`/Library/Image Capture/Devices/` marked `LSBackgroundOnly` / `LSUIElement`,
with an `Info.plist` that declares which devices and transports it matches.
That the mechanism is still shipped by shipping vendor drivers on current macOS
is the single most reassuring feasibility signal: the loader is not dead.

**What is genuinely uncertain and needs empirical confirmation** (this is a
legacy, sparsely documented framework — treat every item as a spike, not an
assumption):

1. **The exact callback set and the module API-version constant.** The precise
   names, signatures, and required-vs-optional status of the `ICD_Scanner…`
   entry points must be read from the installed
   `ICADevices.framework/Headers` in the macOS SDK before any code is written.
   The list below is the *expected shape* from public knowledge, not a
   confirmed contract.
2. **Whether a third-party module still loads on the target macOS at all**, and
   under what signing conditions (see "Signing and TCC"). This is the
   make-or-break question and is the reason the first milestone is a load
   spike.
3. **Whether Bonjour matching is the host's job or the module's** — i.e.
   whether declaring a service type in `Info.plist` is enough for the host to
   discover and hand us a matched device, or whether the module must browse
   `_scanner._tcp` itself. Design for both; confirm empirically.
4. **The band-data delivery contract** — whether the host pulls image data
   (a read-file-data callback) or the module pushes bands via notifications,
   and the exact buffer descriptor (bits per pixel, bytes per row, color
   space) the host expects for color, gray, and bitonal.
5. **The physical-units convention** for scan area and paper size (pixels at
   the chosen resolution vs. a fixed unit like 1/72" or millimeters).

The expected `ICD_Scanner…` callback shape, to be confirmed against the SDK
headers, covers roughly: open device / close device; get device status; read
the property (parameter) tree; get and set scan parameters; start scan; read or
receive image band/file data; and a scan-complete / abort path. The design
below is organized around those responsibilities rather than exact symbol
names.

## Architecture decisions

Each decision lists the options considered, the recommendation, and why.

### A. Front-end type: ICA device module vs. deferring to Plan 3

- **Options.** (1) Build the ICA device module (this plan). (2) Skip Plan 2 and
  reach Image Capture only through Plan 3's eSCL/AirSane path, where Apple's own
  `AirScanScanner` module does all the ICA work and `brscan` only supplies a
  SANE backend that AirSane republishes as eSCL.
- **Recommendation.** Build the module, but gate the whole plan behind a
  cheap load spike (Task 1). If the spike shows a third-party module will not
  load or sign on the target macOS, stop and take Plan 3 instead.
- **Why.** The module's unique value over Plan 3 is a self-contained native
  experience with no AirSane/SANE runtime dependency and no second Bonjour
  identity for the device. But it fights a legacy framework, so it is only
  worth it if the load spike is cheap and passes. Plan 3 already delivers
  Image Capture visibility through a *supported* Apple code path, so it is the
  safe fallback, not a consolation prize. See "Open decisions" for the honest
  go/no-go.

### B. Language and the C++ link seam

- **Options.** (1) Write the callback translation unit as Objective-C++
  (`.mm`) and link `libbrscan` (a C++ static library) directly. (2) Keep the
  module pure Objective-C and interpose a C-ABI shim (`brscan_c`) around
  `libbrscan`.
- **Recommendation.** Objective-C++ (`.mm`), linking `libbrscan` directly.
- **Why.** `ICADevices` entry points are plain C functions; a `.mm` file
  speaks C, Objective-C, and C++ at once, so it can host the `ICD_Scanner…`
  callbacks and call `brscan::RunScan` in the same file with no extra ABI
  layer. The CLI already links `libbrscan` from Objective-C++ (`.mm`) sources,
  so this seam is proven. A separate C shim would be pure overhead.

### C. Discovery and matching

- **Options.** (1) Declare `_scanner._tcp` matching in the bundle's
  `Info.plist` and let the Image Capture host browse Bonjour and hand the
  module a matched device with its resolved host/port. (2) Have the module
  browse `_scanner._tcp` itself with `NSNetServiceBrowser` and register the
  device with the framework.
- **Recommendation.** Prefer (1) — declarative `Info.plist` matching — and
  confirm during the discovery spike whether the host resolves the service to a
  host:port for us. Keep (2) as a fallback if the host only matches by device
  class and expects the module to enumerate.
- **Why.** Declarative matching is how vendor network-scanner modules are
  configured and keeps discovery logic out of our code. The module then only
  has to turn a matched service into a `TcpTransport(host, 54921)`. Confirming
  which side owns discovery is item 3 in the uncertainty list.
- **Coexistence.**
  - *With the daemon.* The daemon (`brscan-scand`) only holds a UDP button
    listener and opens TCP 54921 on demand when a scan actually runs; the ICA
    module likewise opens 54921 only during a scan. The device serves one scan
    session at a time, so the two never contend unless a button scan and a host
    scan literally overlap — an acceptable, user-visible "device busy"
    (`Status::kBusy`, the `-NG 401` greeting) rather than a design flaw.
  - *With Plan 3 (AirSane/eSCL).* If the user also runs AirSane, the same
    physical device appears twice in Image Capture: once as the native
    `_scanner._tcp` module and once as AirSane's `_uscan._tcp` eSCL identity.
    That duplicate is confusing; the human should pick one path per machine
    (see "Open decisions"). They can coexist technically, but shouldn't be
    advertised simultaneously.

### D. Session and threading model

- **Options.** (1) Run `RunScan` to completion on a background thread, buffer
  every decoded page in memory, then feed the host bands/file data from the
  buffer. (2) Refactor `libbrscan` to stream bands incrementally as the device
  sends them, matching ICA's band model natively.
- **Recommendation (corrected, Task 16).** Buffer-then-feed (1) for the page
  model, but run the whole thing **synchronously on icdd's callback thread**
  inside `ICD_ScannerStart`, not on a background thread.
- **Why.** `RunScan` is synchronous and job-shaped: it does greeting → session
  init → source select → negotiate → execute → full page readout, returning a
  `std::vector<ScanResult>` (one per ADF page). Reusing it whole means Plan 2
  inherits the entire tested, live-validated scan path unchanged. The cost is
  memory: a full multi-page ADF color job is buffered before delivery. That is
  acceptable for a first release; streaming is a later optimization, and item 4
  (the band-delivery contract) has to be pinned down before streaming is even
  designable.
- **Threading — reversed in Task 16 (the original recommendation was wrong for
  this API).** The initial design ran `RunScan` on a background `std::thread`
  and posted notifications from it, on the theory that blocking icdd's callback
  thread would freeze the scan UI. Device-in-the-loop testing proved the
  opposite: ICA notification delivery/reply is bound to the icdd connection
  runloop **on the callback thread**, so every `ICDSendNotification*` from the
  worker returned `noErr` yet nothing reached the host — the overview never
  rendered, the UI stuck on "Scanner is warming up", and the scan never
  completed (all one cause). The four reference modules (Apple `VirtualScanner`
  and its modern copy, `scansnaps1500m` shipping on Tahoe, and `SaneNetScanner`)
  all perform the entire scan synchronously inside `ICD_ScannerStart` on the
  callback thread, sending every notification inline, and return only after
  `ScannerScanDone`. Blocking `Start` for the whole scan is correct and expected
  by icdd. `Start` therefore now connects, runs `RunScan`, hands every page back
  (`ScanProgressStatus`/`ScannerPageDone`, or a file write), and sends
  `ScannerScanDone` all inline; the background thread, its join, and the per-job
  `ScanJob` snapshot are removed (SetParameters and Start alternate on the one
  thread, so the transfer settings are plain locals with no cross-thread race).
  Cancel is handled at a page boundary: a `ScanProgressStatus` reply of
  `userCanceledErr` stops further pages and finishes with a clean
  `ScannerScanDone` (there is no mid-`RunScan` cancel, since `RunScan` is
  monolithic).

### E. Paper-size enumeration

- **Options.** (1) Reuse `daemon/paper_size.h`'s captured-tuple table directly.
  (2) Add a small ICA-specific enumeration unit that reuses the *same* captured
  ground-truth data but exposes it in the shape the ICA size picker wants
  (named standard sizes within the device maximum, in the host's units).
- **Recommendation.** (2) — a new, hermetic enumeration unit that draws on the
  same captured tuples, ideally by lifting the paper table into a shared
  location `libbrscan` and the module can both link.
- **Why.** `daemon/paper_size.h` is scand-specific: it keys off the scan
  button's `P=` tokens and bakes in ADF centering decisions for a button flow.
  The ICA picker needs standard size *names* filtered to those at or under the
  device's maximum scannable area, expressed in the host's unit convention, and
  it must offer flatbed-only sizes only for the flatbed functional unit. That is
  a different projection of the same underlying captured areas, so it deserves
  its own small unit rather than overloading the button table. Keeping the
  captured tuples as the single source of truth (they are the ground truth per
  `PROVENANCE.md`) means both front ends agree on geometry. This unit is
  fully hermetically testable.

### F. Build seam

- **Options.** (1) A CMake target that builds the `.app` bundle (Info.plist,
  `LSBackgroundOnly`, the linked `.mm` + `libbrscan`) plus an install step to
  `/Library/Image Capture/Devices/` and a signing hook. (2) A separate Xcode
  project/target for the bundle.
- **Recommendation.** CMake target, consistent with the repo. Add a dedicated
  `install`/packaging step (the system directory needs elevated permissions, so
  installation is a scripted, opt-in step, not part of an ordinary build).
- **Why.** The whole repo builds under MacPorts CMake; `libbrscan`, the CLI,
  the daemon, and the tests are all CMake targets. Note the Plan 1e experience:
  the config app went **SwiftPM**, and the signable `.app` bundle was
  *deferred* — Plan 2 cannot defer bundling, because an ICA module *is* a
  bundle and must be signed to load. So Plan 2 is where the repo finally has to
  solve `.app` bundling + signing for real. CMake can emit an `.app` bundle and
  copy an `Info.plist`; the signing/notarization steps run as post-build script
  hooks (see "Signing and TCC"). Whether CMake's bundle support is ergonomic
  enough here, or an Xcode target is worth introducing solely for signing, is an
  open decision.

### G. Data hand-back and pixel formats

`RunScan` returns one `ScanResult` per page in the device's native encoding
(`libbrscan/include/brscan/scanner.h`):

- `PixelFormat::kRgb` — a baseline JPEG stream (color). Decode with
  `DecodeJpeg` to interleaved 24-bit RGB before hand-back.
- `PixelFormat::kGray` — raw 8-bit samples, one byte per pixel, ready as-is.
- `PixelFormat::kBitonal` — 1 bit per pixel, MSB-first, `1 = black`, rows
  padded to a byte (stride `(width + 7) / 8`). This already matches the
  standard fax/TIFF-G4 and PBM packing, so it maps to the host's 1-bit format
  without repacking — confirm the host's bit-polarity expectation during the
  spike.

- **Recommendation.** Decode to the framework's expected buffer in a small
  descriptor unit that, given a `ScanResult`, produces the host's buffer
  parameters — bits per pixel, bytes per row, color space (RGB / device-gray),
  and the pixel bytes. Color decodes through `DecodeJpeg`; gray and bitonal
  pass through. This descriptor computation is pure and hermetically testable
  even though the delivery call into the framework is not.

## Scan-parameter mapping (ICA ↔ `RunScan` / `Params`)

The module receives Apple's scanner parameters through the get/set-parameters
callbacks and translates them to a `brscan::Params` (see
`libbrscan/include/brscan/types.h`) for a normal `RunScan` call. `button_flow`
stays `false` throughout — this is host-initiated scanning, not the button
flow.

**Schema (Task 8 — the real TWAIN/ICAP contract).** Plan 2 Task 2 filled the
`ICD_ScannerGetParameters` dict with invented descriptive keys
(`supportedResolutions`, `paperSizes`, …); the live Scan test showed Image
Capture recognised none of them, rendered no controls, and never called `Start`.
Image Capture's scanner modules speak the TWAIN-derived `ICAP_*`/`CAP_*`
capability vocabulary, and expect it under a single top-level **`device`** dict.
Task 8 first emitted a top-level `functionalUnits` dict with per-unit capability
sub-dicts keyed `"0"`/`"3"`; Image Capture rendered **no** controls from that
shape and never called `Start`. Task 10 corrects it to the shape those modules
actually consume: `GetParameters` now emits

```
theDict["device"] = {
    <ICAP_*/CAP_* capability entries, FLAT, for the selected unit>,
    functionalUnits : {
        availableFunctionalUnitTypes : [0, 3],   // flatbed, documentFeeder.
        selectedFunctionalUnitType   : 0,        // flatbed.
    }
}
```

- The capability entries sit **flat** inside `device` and describe the SELECTED
  functional unit (flatbed). The nested `functionalUnits` dict holds **only**
  `availableFunctionalUnitTypes` + `selectedFunctionalUnitType` — no per-unit
  capability sub-dicts. Advertising both unit types is what makes the source
  picker appear; switching the flat capability set when the host reselects a unit
  is a follow-up (the flatbed and feeder differ — the flatbed adds the platten
  "default" size and the flatbed-only sizes and has the larger extent).

Each capability is a TWAIN-style container
`{ "type": "TWON_ENUMERATION" | "TWON_ONEVALUE", "value": <array | scalar>,
"current": <v>, "default": <v> }` under these keys:

| ICAP/CAP capability key | Shape | Value | `brscan::Params` on the way back in |
|---|---|---|---|
| `ICAP_XRESOLUTION`, `ICAP_YRESOLUTION` | `TWON_ENUMERATION` | DPI `{100,150,200,300,400,600}`, current/default 300 | `x_dpi`, `y_dpi` (clamped to the `ESC I` `Offer` max) |
| `ICAP_BITDEPTH` | `TWON_ENUMERATION` | `ICScannerBitDepth` `{1, 8}`, default 8 | `mode` (paired with `ICAP_PIXELTYPE`) |
| `ICAP_PIXELTYPE` | `TWON_ENUMERATION` | `ICScannerPixelDataType` `{0 BW, 1 Gray, 2 RGB}`, current/default RGB (2) | `mode` (drives the colour-mode picker: RGB → `kColor`, Gray → `kGray`, BW → `kBlackWhite`) |
| `ICAP_PHYSICALWIDTH`, `ICAP_PHYSICALHEIGHT` | `TWON_ONEVALUE` | the platen's real extent in **inches** (a `double`): width = widest sheet (A3, 3472 px@300 ÷ 300 = 11.57 in), height = longest sheet (Ledger, 5053 px@300 ÷ 300 = 16.84 in), from `daemon/paper_size.cpp` | bounds `area`; drives the overview platen the host draws |
| `ICAP_SUPPORTEDSIZES` | `TWON_ENUMERATION` | array of `ICScannerDocumentType` values (see paper mapping) | `area` (via the size picker) |
| `ICAP_UNITS` | `TWON_ENUMERATION` | `ICScannerMeasurementUnit` `{0 inches, 1 cm, 5 pixels}`, current/default **inches (0)** | the host renders the platen from the inch physical extents and echoes the scan rectangle in this unit; `PixelsFromMeasure` converts it back to the module's pixel space |
| `CAP_FEEDERENABLED` | `TWON_ONEVALUE` | `0` (flatbed selected) | source picker; feeder selection maps to `Source::kAdf` |
| `CAP_DUPLEX` | `TWON_ONEVALUE` | `0` (`TWDX_NONE`) | duplex control (feeder only) |

Paper-token → `ICScannerDocumentType` (raw values are the **non-contiguous**
ImageCaptureCore enum, verified against the SDK header
`ICScannerFunctionalUnits.h`; the mapping lives in
`scan_translate.h::DocumentTypeForPaperToken`): LETTER → `USLetter` (3), LEGAL →
`USLegal` (4), A4 → `A4` (1), LEDGER → `USLedger` (9), A3 → `A3` (11), A5 → `A5`
(5), EXECUTIVE → `USExecutive` (10); the flatbed also advertises `Default` (0,
platten). PHOTO and BCARD have no clean standard case, so they are omitted from
`ICAP_SUPPORTEDSIZES` and offered as a custom scan area only. Brightness /
contrast are **not** scalar client capabilities (they are `vendorFeatures` /
`ICScannerFeatureRange`), so they are deferred this task: the invented
`brightness`/`contrast`/`*Range` keys are dropped from the advertisement and
`TranslateScanParams` simply defaults them to 50. A later task can add them as
`ICScannerFeatureRange` if the panel needs them.

**`SetParameters` echo → `ScanRequest` → `Params`.** The host echoes its
selection back through `ICD_ScannerSetParameters`; the module reads the real
keys, each presence-aware (a missing key falls back to the design default):

Task 11 live trace corrected the on-the-wire shape: the host does **not** send
flat top-level keys. It sends ONE top-level `userScanArea` dictionary whose
entries are TWAIN `{type, value[, current]}` sub-dicts. `ReadScanRequest`
(`ica-module/module_main.mm`) does the CoreFoundation traversal — read each
entry's `value`, falling back to `current` — into a plain `IcapScanSelection`,
and the pure, unit-tested `scan_translate.h::ScanRequestFromIcap` maps that to a
`ScanRequest` (which `TranslateScanParams` then turns into `Params`). The parse
prefers the nested `userScanArea`, falls back to the top-level dict, and probes a
`device` wrapper, so it survives either shape.

| Nested `userScanArea` entry | `brscan::Params` field | Notes |
|---|---|---|
| `ICAP_XRESOLUTION.value` (falls back to `ICAP_YRESOLUTION.value`) | `x_dpi`, `y_dpi` | Same value both axes; clamped to the offered max. Default 300. |
| `ICAP_PIXELTYPE.value` (0/1/2) | `mode` | `0 → kBlackWhite`, `1 → kGray`, `2 → kColor`. (The old `"scan mode"` string heuristic is retired — the host sends the numeric pixel type.) |
| `selectedFunctionalUnitType.value` | `source` | Flatbed `0` → `Source::kFlatbed`; feeder `3` → `Source::kAdf`. Probed nested then top-level. |
| `duplex` (bool) | `duplex` | Only honoured for `kAdf`. |
| scan-rectangle offset + extent (in `ICAP_UNITS`) | `area` (`Area{x0,y0,x1,y1}`) | The host reports `offsetX`/`offsetY`/`width`/`height` as top-level numbers in `ICAP_UNITS` (read as **doubles**, since inches carry fractions like 8.5). `ScanRequestFromIcap` converts each to pixels at the request's dpi via the pure `PixelsFromMeasure` (inches × dpi; cm × dpi ÷ 2.54; pixels pass through; anything else → full area), then `CornersFromUserScanArea` builds `x1=x0+w`, `y1=y0+h`. A US Letter selection in inches at 300 dpi → `2550×3300`. A degenerate/absent rect or unconvertible unit means `{0,0,0,0}` = "full offered area." `ReadScanRequest` still logs the FULL `userScanArea` dict for a re-test. |
| `ICAP_BITDEPTH.value`, `ICAP_UNITS.value` | — | Captured for the trace; `ICAP_UNITS` selects the offset/extent conversion, bit depth follows from the mode. |
| — (no ICA control) | `ScanMode::kTrueGray`, `kErrorDiffusion` | Not exposed; the panel has no natural control for them. |

#### Authoritative per-scan notification sequence (Task 15)

The module diverged from Apple's canonical module→host notification protocol
(pinned from `ICADevices.framework` `ICAApplication.h` + `ICD_ScannerCalls.h`
plus reference modules, interface facts only). The corrected sequence, in order (all posted inline on the callback thread by
the synchronous `Start`, Task 16):

1. **Warm-up — removed (Task 16).** Task 15 opened each scan with two
   `kICANotificationTypeDeviceStatusInfo` warm-up notifications
   (`kICANotificationSubTypeWarmUpStarted` / `…WarmUpDone`). The shipping Tahoe
   fork sends none, and ours — posted from the background worker thread — is what
   left the UI stuck on "Scanner is warming up"; `PostWarmUp` and its calls are
   removed. The scan now opens directly with the first progress notification.
2. **`kICANotificationTypeScanProgressStatus`** (per pass) — overview pixels via
   `ICDAddImageInfoToNotificationDictionary`; sent with
   `ICDSendNotificationAndWaitForReply` (a cancel is `replyCode == userCanceledErr`,
   which ends the scan at that page boundary with a clean `ScannerScanDone`).
3. **`kICANotificationTypeScannerPageDone`** — plain `ICDSendNotification`; on
   file transfer it carries the exact written path under
   `kICANotificationScannerDocumentNameKey`.
4. **`kICANotificationTypeScannerScanDone`** — plain `ICDSendNotification`.

**The notification object is the DEVICE object, not the scan trigger (prime
fix).** Every scanner notification references
`ScannerObjectInfo::icaObject` (the framework-assigned device object, marked
"Apple" in `ICD_ScannerCalls.h`) under `kICANotificationICAObjectKey`. Earlier
builds passed `ICD_ScannerStartPB::object` (the trigger, observed as `0x02000001`
in logs); the host keys its scan session on the **device** object, so
notifications against the trigger left Image Capture on "Scanning document"
forever and never rendered the overview. The device object is captured in
`OpenSession`/`Start` from `deviceObjectInfo->icaObject` and passed to the
synchronous scan (Task 16); `Start` logs `deviceObjectInfo->icaObject` vs
`pb->object` so a re-test confirms which is right.

**Overview image-info args must match the buffer exactly.**
`ICDAddImageInfoToNotificationDictionary(dict, width, height, bytesPerRow,
dataStartRow, dataNumberOfRows, dataSize, dataBuffer)` copies `dataSize` bytes and
describes the row stride with `bytesPerRow`; a silent mismatch renders nothing.
`PostPage` now sets `bytesPerRow = DescribeBuffer` stride, `dataSize = stride ×
height` (`expected_byte_count`), `dataStartRow = 0`, `dataNumberOfRows = height`,
guards that the real buffer is at least `dataSize` bytes, and logs every arg
(width/height/stride/dataSize/bpp/components/colorspace/bufferBytes).

Reverse direction (device → host), per page: `RunScan` fills a `ScanResult`
with `format`, `width`, `height`, and native `data`; decision G turns that into
one full-height image chunk. **Task 14 corrected the in-memory (overview) packer**:
the chunk is packed with `ICDAddImageInfoToNotificationDictionary` — the
**IMAGE-info** packer that populates the `kICANotificationImage*` keys the host's
overview/preview accumulator consumes — **not** `ICDAddBandInfoToNotification­Dictionary`.
This is Defect A: Task 11 used the band packer, and the host accepted the
notification (`sendProgressBand=0`, `ScannerScanDone err=0`) but never rendered
the overview because its preview accumulator reads the Image-info keys, not the
Band-info keys. Convention confirmed against a working SANE-backed ICA scanner
module's `ScanProgressStatus` hand-back (interface facts only, no source copied).
The chunk is carried in a `kICANotificationTypeScanProgressStatus` notification
(**not** the page-done), and **every** scanner notification (progress /
`ScannerPageDone` / `ScannerScanDone`) references the framework-assigned **device**
object (`ScannerObjectInfo::icaObject`) under `kICANotificationICAObjectKey` —
**not** `ICD_ScannerStartPB::object`, corrected in Task 15 (see the authoritative
sequence above). `PostPage` logs the full notification key set so a
device-in-the-loop re-test can confirm the host consumes the Image-info keys.
The Task-7 build did neither — it packed the band into a `ScannerPageDone` and
tried to mint a per-page object with `ICDNewObject`, which returns `unimpErr`
(-4) in the scanner-module runtime (it is the legacy generic object API and is
not serviced for scanner modules). No object is minted on the in-memory band
path; the scanner-specific `ICDScannerNewObjectInfoCreated` +
`kICANotificationTypeObjectAdded` + `kICANotificationScannerDocumentNameKey`
belong to the alternative file-based transfer mode. The `Offer` the device
returns from `ESC I` gives the true maximum resolution and pixel dimensions,
which bound the parameter ranges advertised to the host so the UI never offers
an impossible value.

### Two transfer modes: file-based vs overview/memory (Task 12)

The host chooses the transfer mode **per scan**, and the module must honour the
one it asks for or the client hangs. The two modes are distinguished by the keys
the SetParameters request carries (confirmed from the live trace and Apple's
public `VirtualScanner` sample module, interface facts only):

- **Overview / preview → in-memory image.** The request carries
  `"scan mode" = overview`, `progressNotificationWithData = 1`, and **no**
  destination. The module pushes the decoded page as one full-height chunk via
  `ICDAddImageInfoToNotificationDictionary` (the Task-14 Defect-A fix + the
  Task-15 device-object/arg fixes; see the authoritative sequence above) in a
  `kICANotificationTypeScanProgressStatus`, then `ScannerPageDone`, then
  `ScannerScanDone` — all against the device object, all inline on the callback
  thread (Task 16; no warm-up pair).

- **Final scan → file-based transfer.** The request carries a security-scoped
  destination folder under `ICSecurityScopedWrappedURL` (a modern icdd key not
  present in any public SDK header — observed only in our own logs) **and/or** a
  plain `"document folder"` path, plus `"document name"`, `"document extension"`,
  and `"document format"` (a UTI). The host is asking the module to **encode the
  page, write it into that folder, and post a file/object completion**. Pushing
  bands here (the pre-Task-12 defect) leaves the host waiting for a file it never
  gets → the "Scanning document" hang.

  The module now: resolves + `startAccessingSecurityScopedResource` on the
  destination URL; encodes the decoded page with native macOS **ImageIO**
  (`CGImageDestinationCreateWithURL` / `AddImage` / `Finalize`) — kRgb via
  `DecodeJpeg` → RGB `CGImage`, kGray/kBitonal via a matching `CGImage`
  (bitonal uses a `{1,0}` decode array so `1 = black` renders as ink) — to the
  requested format (TIFF/JPEG/PNG; default TIFF); writes
  `<document name>.<extension>` into the folder (multi-page/ADF appends a
  ` N` index; single-page flatbed is the must-pass, multi-page a follow-up);
  `stopAccessingSecurityScopedResource`; then posts a
  `kICANotificationTypeScannerPageDone` carrying **the exact written absolute
  path** (`<document folder>/<document name>.<document extension>`) under
  `kICANotificationScannerDocumentNameKey` — the same path the file was written
  to, logged verbatim (Task 15) — and finally one
  `kICANotificationTypeScannerScanDone`, both against the device object. Encoding
  uses ImageIO directly in the module — it does **not** pull in
  `daemon/output_writer` (different scope).

  For a **local** client (a `"document folder"` is present, as with Image
  Capture saving to `~/Documents`) VirtualScanner writes the file and posts
  PageDone + ScanDone without minting an object; the
  `ICDScannerNewObjectInfoCreated` + `kICANotificationTypeObjectAdded` object
  announce is its **remote/network** path (`documentFolderPath == NULL`), so the
  module does not use it here (also side-stepping the `ICDNewObject` `unimpErr`
  seen in Task 7). The pure filename/UTI policy (default TIFF, derive a missing
  extension from the UTI and vice-versa, strip a duplicated extension, index
  multi-page names) lives in the hermetically tested `ica-module/file_transfer`
  unit; the CoreFoundation / ImageIO / security-scoped-URL glue is in
  `module_main.mm` and is device-side, not unit-testable.

## Testing strategy

The split is stark: the translation logic is hermetic, and everything that
touches the framework or the device is manual. Lean hard on the first.

**Hermetic (GoogleTest, no framework, no device — lands in CI):**

- Parameter mapping: Apple parameters → `brscan::Params` (every mode, source,
  duplex, resolution clamping to the offer, area conversion, brightness/contrast
  scaling).
- Paper-size enumeration: names within the device maximum, flatbed-only vs.
  feeder sizes, unit conversion, and agreement with the captured ground-truth
  tuples.
- Buffer descriptor: `ScanResult` → (bits per pixel, bytes per row, color
  space, pixel bytes) for RGB (post-`DecodeJpeg`), gray, and bitonal, including
  the bitonal stride and polarity.

These need no `ICADevices` linkage: they are pure functions extracted
deliberately so the framework-bound glue stays thin. The `libbrscan` scan path
itself is already covered by the existing suite and live validation, so Plan 2
adds no new coverage there.

**Inherently manual (a runbook, not CI):**

- Does the module bundle load on the target macOS, and under what signing?
  (The load spike — Task 1.)
- Does the device appear in Image Capture and in Printers & Scanners under the
  synthetic name?
- Does discovery match the real `_scanner._tcp` service and bind the right
  host:port?
- Does a scan complete end-to-end for flatbed and for a multi-page ADF job, in
  color / gray / black-and-white?
- Cancellation mid-scan, "device busy" when a button scan overlaps, and TCC
  prompt behavior.

Plan 2 should ship a short live runbook (in the shape of
`docs/RUNBOOK-plan-1d-live.md`) enumerating these manual checks, because they
cannot be automated and must be repeatable across macOS versions.

## Signing and TCC

This is the highest-risk area after "does it load at all."

- A module in `/Library/Image Capture/Devices/` is loaded into an Apple host
  process. On Apple Silicon, that host may enforce library validation, which
  would reject a module not signed by the same team / not Developer-ID signed
  and notarized. Vendor drivers in that directory are Developer-ID signed and
  notarized — that is the likely bar for a distributable module.
- For local development, ad-hoc signing (or a self-signed cert with the right
  local trust) is the first thing to try in the load spike. If ad-hoc loads,
  iteration is cheap; if only Developer-ID + notarization loads, every load test
  costs a notarization round trip, which materially changes the plan's effort.
- TCC: any user-facing prompts (e.g. for the scan destination folder) are
  attributed to the Image Capture host process, not to our bundle, so the
  module itself likely needs no privacy entitlements — but this must be
  confirmed on the target OS. Installing to the system directory needs an admin
  step, handled by the packaging script, not the build.

## Task breakdown

Ordered so the two make-or-break unknowns (does it load; does the pure logic
work) are answered first, before any effort is sunk into the framework glue.

1. **Load spike — a stub module that just appears.** A minimal background-only
   bundle with an `Info.plist` that declares the device match, no real scan
   logic, installed and signed the cheapest way that works. Success = the
   synthetic device shows up in Image Capture / Printers & Scanners on the
   target macOS. This is the go/no-go gate for the whole plan and the
   recommended first milestone. *Verifiable manually, immediately.*
   **Status:** the spike bundle, build seam, and runbook are implemented
   (`ica-module/`, `docs/RUNBOOK-plan-2-loadspike.md`). It builds, links
   `ICADevices`, and ad-hoc signs; the load/appearance result is a privileged,
   manual check the runbook drives — not yet run against hardware.
   **Task 1d (match/binding gap — resolved as fixable):** the earlier re-test
   showed our module was never *launched* and no AMFI line named it — a
   match/binding gap, not a signing gate. The cause: our `_scanner._tcp.` match
   dict carried only `device type = scanner`, but icdd binds a browsed device
   via `compareBonjourDeviceModuleDictionary:withBonjourTXTRecord:`, which needs
   an `ICABonjourTXTRecordKey` sub-dict to test against the device's TXT record.
   `ICABonjourTXTRecordKey` is Apple's public match interface — Apple's own
   `AirScanScanner.app` uses exactly this key to match its service type on
   Bonjour TXT pairs — so a non-Apple module for `_scanner._tcp.` applies the
   same interface, keyed on the TXT pairs the device itself broadcasts. Fixed
   `ica-module/DeviceMatchingInfo.plist` to pair `device type = scanner` with
   `ICABonjourTXTRecordKey = { mdl; mfg; }` and `device events = ( scan )`,
   keyed on the device's own advertised `mdl=MFC-J6920DW` / `mfg=Brother` TXT
   (captured black-box with `dns-sd -L`). This is **not structural** — Plan 2
   stays viable past the match gap. The signing/library-validation gate is the *next* gate and is
   still unverified: binding the device is what finally lets icdd *attempt* the
   launch, so the runbook's §3 A-vs-B re-test now yields a real launch attempt
   (success = our `main: … executable launched` log line; AMFI denial = the
   signing wall → Plan 3).
2. **Hermetic parameter-mapping unit + tests.** Apple parameters → `Params`,
   with resolution clamping and area/brightness/contrast conversion. Pure C++,
   GoogleTest, no framework. *Lands green in CI.*
3. **Hermetic paper-enumeration unit + tests.** Standard sizes within the
   device maximum, flatbed vs. feeder, drawing on the captured tuples (lift the
   shared table as needed). *Lands green in CI.*
4. **Hermetic buffer-descriptor unit + tests.** `ScanResult` → host buffer
   parameters for RGB/gray/bitonal. *Lands green in CI.*
5. **Discovery / matching.** Confirm empirically whether the host resolves the
   matched `_scanner._tcp` service; wire a matched device to
   `TcpTransport(host, 54921)`. *Manual verification against the device.*
6. **Session lifecycle callbacks.** Open/close, status, and the get/set-
   parameters callbacks wired to the mapping unit and to the advertised ranges
   derived from the `ESC I` offer. *Partly manual.*
7. **Scan execution.** Run `RunScan` on a background thread, buffer pages, feed
   the host bands/file data via the descriptor unit; wire cancel and completion.
   *Manual, end-to-end.*
8. **Build + install + signing.** CMake bundle target, install script to
   `/Library/Image Capture/Devices/`, signing/notarization hook.
9. **Live runbook.** The manual verification checklist for load, appearance,
   discovery, scan, cancel, busy, and TCC across macOS versions.

Tasks 2–4 are independent of 1 and of each other and can proceed in parallel;
they are worth doing even if the human wants more confidence before the spike,
because they are useful to Plan 3's SANE backend geometry too. Tasks 5–9 should
not start until Task 1 passes.

## Open decisions for the human

1. **Go / no-go vs. Plan 3 — the honest call.** Plan 3 (the SANE ethernet
   transport → AirSane → eSCL) *already* makes the scanner appear and scan in
   Image Capture, through Apple's own supported `AirScanScanner` module, and it
   is the user's already-running path. Plan 2's only advantage is a
   self-contained native module with no AirSane dependency and no duplicate
   Bonjour identity — bought at the price of a legacy, under-documented
   framework and a hard signing requirement. **Recommendation:** do the Task 1
   load spike (cheap, a day or two) as a pure feasibility probe; if a
   third-party module will not load or demands notarization on every iteration,
   stop and standardize on Plan 3. Plan 2 is worth finishing only if the spike
   is clean *and* the human specifically wants to drop the AirSane runtime
   dependency. Absent that preference, Plan 3 is the better use of effort.
2. **First-milestone target.** Recommended: the Task 1 load spike (device
   merely *appears*), because it de-risks everything else. Alternative if the
   human wants CI-visible progress first: land the hermetic units (Tasks 2–4),
   which are useful regardless of the go/no-go and independent of the framework.
3. **Build seam.** CMake bundle target (recommended, matches the repo) vs. a
   dedicated Xcode target introduced solely to get ergonomic signing /
   notarization. Tied to whether the human wants Plan 2 to be the place the repo
   finally solves `.app` bundling that Plan 1e deferred.
4. **Signing approach for iteration and distribution.** Confirm the minimum
   signing that loads on the target macOS — ad-hoc for local dev vs.
   Developer-ID + notarization required even to load. This directly sets the
   plan's effort, because notarization-per-load is a slow loop. Needs the Task 1
   spike to answer.
5. **Coexistence policy with AirSane/eSCL.** If both Plan 2 and Plan 3 ship,
   the device appears twice in Image Capture. Decide whether the two are ever
   advertised simultaneously or the machine picks one path, and document the
   choice.
6. **Which SDK/macOS versions are in scope.** `ICADevices` behavior and the
   load/signing rules can differ across macOS releases; name the target
   versions so the spike and the runbook test the right ones.
