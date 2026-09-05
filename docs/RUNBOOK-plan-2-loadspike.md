# Load-spike runbook — Plan 2 Task 1 (does an ICA module still load?)

This is the go/no-go gate for Plan 2. Before any scan-path code is written, we
answer one question empirically: **does a third-party Image Capture (ICA) device
module still load on this macOS, and what signing does it need?** If a
third-party module will not load, or only loads after Developer-ID notarization
on every iteration, Plan 2 stops and the project standardizes on Plan 3
(AirSane/eSCL) instead. See `docs/PLAN-2-DESIGN.md`, Task 1 and "Open decisions".

The module under test (`ica-module/`) is deliberately inert: a background-only
`.app` that registers no-op `ICD_Scanner…` callbacks and calls the framework's
`ICD_ScannerMain`. It runs no scan. Its only job is to appear.

Device identity is the synthetic Bonjour name `BRW00AABBCCDDEE` throughout.

## What was confirmed against the SDK (clean-room)

Read from `MacOSX.sdk/System/Library/Frameworks/ICADevices.framework/Headers`
(`ICD_ScannerCalls.h`, `ICADevices.h`) — public SDK only, no vendor source:

- The framework exports `int ICD_ScannerMain(int, const char**)` and the global
  callback table `ICD_scanner_callback_functions gICDScannerCallbackFunctions`.
  A module fills in the entry points it implements, then calls `ICD_ScannerMain`,
  which runs the module's service loop. Both symbols are present in the SDK's
  `ICADevices.tbd`, so the module links.
- The host is `icdd` (the "ImageCapture Discovery Daemon",
  `/System/Library/Image Capture/Support/icdd`, a per-user LaunchAgent
  `com.apple.icdd`). Its binary strings name the load paths
  `/Library/Image Capture/Devices/` and `/System/Library/Image Capture/Devices/`,
  the matching selectors `addMatchingBonjourDevices:fromBundleWithPath:…` and
  `baseDictionaryForBonjourServiceType:andTXTRecords:devices:`, the key
  `ICABonjourServiceTypeKey`, the browsed service type `_scanner._tcp.`, and the
  warning `Missing DeviceMatchingInfo.plist in '<bundle>'!`.

**Two things the SDK does not settle, so the spike must:**

1. The exact schema of `Contents/Resources/DeviceMatchingInfo.plist`. No
   Bonjour-matching module ships on this macOS to copy — the only module in
   `/System/Library/Image Capture/Devices/` is Apple's own `AirScanScanner.app`,
   which uses its own eSCL discovery and a different `DeviceInfo.plist`. Our
   `DeviceMatchingInfo.plist` is reconstructed from icdd's symbols and is
   **unconfirmed**; the install test is what validates it.
2. Whether icdd will load a non-Apple module at all on this OS, and under what
   signature. This is the make-or-break question and can only be answered by
   installing into the privileged directory (below).

## 1. Build and sign (no privileges needed)

From the repo root:

```bash
cmake -S . -B build
cmake --build build --target brscan-ica-loadspike
```

That produces `build/BrscanICALoadSpike.app`, copies `DeviceMatchingInfo.plist`
into `Contents/Resources/`, and **ad-hoc** signs the bundle (`codesign --sign -`),
the cheapest signature to try first. Verify:

```bash
codesign --verify --strict --verbose=2 build/BrscanICALoadSpike.app
codesign -dvvv build/BrscanICALoadSpike.app    # Signature=adhoc, flags=0x2(adhoc)
```

Gatekeeper will reject an ad-hoc bundle (`spctl -a -t exec` → "rejected"). That
is expected and is a *distribution* check, not the *load* check icdd does; do
not read it as a load result.

## 2. Install into the system directory (privileged, manual)

ICA modules load only from `/Library/Image Capture/Devices/` or the system
directory — there is no user-writable location — so this step needs admin and is
kept out of the build:

```bash
sudo ica-module/install-loadspike.sh install
# then make icdd rescan:
killall icdd 2>/dev/null || launchctl kickstart -k gui/$(id -u)/com.apple.icdd
```

Remove it when done:

```bash
sudo ica-module/install-loadspike.sh uninstall
killall icdd 2>/dev/null || true
```

## 3. Check whether it loaded (as headless as possible)

icdd has no query CLI, so watch its log. In one terminal:

```bash
log stream --predicate 'process == "icdd"' --info --debug
```

Then trigger a rescan (kickstart icdd as above, or open **Image Capture**).
Read the stream for:

- our bundle path being scanned, and specifically whether the
  `Missing DeviceMatchingInfo.plist` warning fires (it should NOT — the file is
  present; if it does, our file is in the wrong place or unreadable),
- any code-signing / library-validation rejection naming
  `ai.jiffylabs.brscan.ica-loadspike`,
- a Bonjour browse of `_scanner._tcp.` and whether our declared device is added.

A backward look at what already happened:

```bash
log show --last 10m --predicate 'process == "icdd"' --info --debug
```

Also check for an AMFI / code-signing gate on load:

```bash
log show --last 10m --predicate \
  'subsystem == "com.apple.amfi" OR eventMessage CONTAINS "library validation"' --info
```

## 4. The manual UI check (the actual go/no-go)

The definitive pass is visual and cannot be automated:

1. With the module installed and icdd restarted, open **Image Capture** and
   **System Settings ▸ Printers & Scanners**.
2. **Go** = the synthetic device `BRW00AABBCCDDEE` appears in the source list
   (even if selecting it does nothing — this spike has no scan path).
3. **No-go** = it never appears. Correlate with the icdd log from step 3 to see
   why: `DeviceMatchingInfo.plist` schema rejected, signature/library-validation
   rejected, or the service simply never browsed.

Record which happened. A rejection is a finding, not a failure — it tells us the
real bar (schema fix, or Developer-ID + notarization) before Plan 2 continues.

## Signing: what we can and cannot verify here

- **Verified locally:** the bundle builds, links `ICADevices`, and takes a valid
  ad-hoc signature (`codesign --verify --strict` passes).
- **Needs a real device / privileged install to answer:** whether icdd loads an
  **ad-hoc** module, or enforces library validation and demands a
  **Developer-ID-signed + notarized** bundle even to load. Apple's own module in
  the system directory carries the `library-validation` flag and is Team-signed,
  which is a strong hint the bar is high — but only the install test on the
  target OS confirms it. This project has no Developer-ID certificate available
  in this environment, so the notarization path cannot be exercised here; if the
  ad-hoc install does not load, that is where the effort estimate changes (every
  load test becomes a notarization round trip) and Plan 3 becomes the better bet.

## What this runbook does NOT cover

Discovery correctness, parameter mapping, the scan itself, cancellation, and TCC
prompts are Plan 2 Tasks 5–9 and their own live runbook. This one stops at
"does the module load and the device appear".
