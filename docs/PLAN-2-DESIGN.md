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
- **Recommendation.** Buffer-then-feed (1) for Plan 2.
- **Why.** `RunScan` is synchronous and job-shaped: it does greeting → session
  init → source select → negotiate → execute → full page readout, returning a
  `std::vector<ScanResult>` (one per ADF page). Reusing it whole means Plan 2
  inherits the entire tested, live-validated scan path unchanged. The cost is
  memory: a full multi-page ADF color job is buffered before delivery. That is
  acceptable for a first release; streaming is a later optimization, and item 4
  (the band-delivery contract) has to be pinned down before streaming is even
  designable. The module must run `RunScan` off the host's calling thread and
  post progress/completion back through the framework's notification path so the
  scan UI stays responsive and cancellable.

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
Image Capture's scanner modules speak the TWAIN-derived `ICAP_*` capability
vocabulary, so `GetParameters` now emits, under a top-level `functionalUnits`
dict:

- `availableFunctionalUnitTypes` → array of `ICScannerFunctionalUnitType`
  numbers (`0` flatbed, `3` documentFeeder); `selectedFunctionalUnitType` → `0`.
- one capability sub-dict **per unit**, keyed by the stringified unit type
  (`"0"`, `"3"`) — capabilities are scoped per unit because the flatbed and
  feeder differ (the flatbed adds the platten "default" size and the
  flatbed-only sizes and has the larger extent).

Each capability is a TWAIN-style container
`{ "type": "TWON_ENUMERATION" | "TWON_ONEVALUE", "value": <array | scalar>,
"current": <v>, "default": <v> }` under these keys:

| ICAP capability key | Shape | Value | `brscan::Params` on the way back in |
|---|---|---|---|
| `ICAP_XRESOLUTION`, `ICAP_YRESOLUTION` | `TWON_ENUMERATION` | DPI `{100,150,200,300,400,600}`, current/default 300 | `x_dpi`, `y_dpi` (clamped to the `ESC I` `Offer` max) |
| `ICAP_BITDEPTH` | `TWON_ENUMERATION` | `ICScannerBitDepth` `{1, 8}`, default 8 | `mode` (with the scan-mode/pixel type: 8-bit RGB → `kColor`, 8-bit gray → `kGray`, 1-bit → `kBlackWhite`) |
| `ICAP_PHYSICALWIDTH`, `ICAP_PHYSICALHEIGHT` | `TWON_ONEVALUE` | the unit's max extent in `ICAP_UNITS` (pixels at 300 dpi, from `daemon/paper_size.cpp`) | bounds `area` |
| `ICAP_SUPPORTEDSIZES` | `TWON_ENUMERATION` | array of `ICScannerDocumentType` values (see paper mapping) | `area` (via the size picker) |
| `ICAP_UNITS` | `TWON_ONEVALUE` | `ICScannerMeasurementUnit` pixels = `5` | keeps host geometry in the module's pixel space |

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

| Echoed key | `brscan::Params` field | Notes |
|---|---|---|
| `ICAP_XRESOLUTION` (number) | `x_dpi`, `y_dpi` | Same value both axes; clamped to the offered max. Default 300. |
| `scan mode` (string) | `mode` | Best-effort case-insensitive map to a pixel type (black/bw/bitonal/text → `kBlackWhite`, gray → `kGray`, else `kColor`); exact string vocabulary is device-in-the-loop. |
| `selectedFunctionalUnitType` (number) | `source` | Flatbed `0` → `Source::kFlatbed`; feeder `3` → `Source::kAdf`. |
| `duplex` (bool) | `duplex` | Only honoured for `kAdf`. |
| `userScanArea` (dict: `offsetX`,`offsetY`,`width`,`height`) | `area` (`Area{x0,y0,x1,y1}`, pixels — `ICAP_UNITS`) | `x0=offsetX`, `y0=offsetY`, `x1=offsetX+width`, `y1=offsetY+height`, via the pure, unit-tested `scan_translate.h::CornersFromUserScanArea`. A degenerate/absent rect means `{0,0,0,0}` = "full offered area," which `RunScan` honours. |
| `ICAP_UNITS`, `document folder`/`name`/`extension`/`format`, `ColorSyncMode` | — | Logged for the live trace; the destination strings do not affect the in-memory band hand-back. |
| — (no ICA control) | `ScanMode::kTrueGray`, `kErrorDiffusion` | Not exposed; the panel has no natural control for them. |

Reverse direction (device → host), per page: `RunScan` fills a `ScanResult`
with `format`, `width`, `height`, and native `data`; decision G turns that into
the host's band buffer via `ICDAddBandInfoToNotificationDictionary`. Each
`kICANotificationTypeScannerPageDone` now also carries
`kICANotificationICAObjectKey` — a per-page image `ICAObject` minted as a child
of the device object with `ICDNewObject` — which the public headers say the page
notification must reference. The `Offer` the device returns from `ESC I` gives
the true maximum resolution and pixel dimensions, which the module should use to
bound the parameter ranges it advertises to the host so the UI never offers an
impossible value.

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
