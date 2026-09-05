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

1. The exact schema of `Contents/Resources/DeviceMatchingInfo.plist`, and
   whether a `DeviceInfo.plist` is also required.
2. Whether icdd will load a non-Apple module at all on this OS, and under what
   signature. This is the make-or-break question and can only be answered by
   installing into the privileged directory (below).

## Task 1b update — plist schema corrected, DeviceInfo.plist added

The first spike installed and icdd read its `DeviceMatchingInfo.plist` without
the `Missing…` warning, but **Image Capture showed DEVICES = 0** — no device
was instantiated. Task 1b found two causes and fixed both:

- **`DeviceMatchingInfo.plist` had the wrong schema.** The first version was
  reconstructed from icdd's symbols (`ICABonjourServiceTypeKey`, a `bonjour`
  array of `serviceType`/`devices`/`deviceType`). Inspecting the one shipping
  example, `/System/Library/Image Capture/Devices/AirScanScanner.app`, shows the
  real shape: a top-level **`BonjourNetwork`** dict keyed by the Bonjour service
  type, each value an array of dicts carrying **`device type` = `scanner`**
  (note the space), plus a top-level **`Version` = `1.0`**. AirScan keys it on
  the eSCL types `_uscan._tcp.` / `_uscans._tcp.`; we key the same structure on
  the Brother raw type `_scanner._tcp.`. The file is rewritten to this schema.
- **No `DeviceInfo.plist` shipped at all.** AirScan carries one; we did not.
  Added `ica-module/DeviceInfo.plist` (`device info version` = `3.0`, a
  `devices` dict of `{ iconFile }` entries) and wired it into the bundle. It is
  not settled from the public interface whether icdd keys `devices` by the
  module name or the matched Bonjour device name, so we register both
  (`BrscanICALoadSpike` and the synthetic `BRW00AABBCCDDEE`) to a stock system
  icon.
- **Added `os_log` tracing** to `module_main.mm` (subsystem
  `ai.jiffylabs.brscan.ica`, category `loadspike`) at `main` /
  `ICD_ScannerMain` entry and in every registered callback, so a re-run shows
  whether icdd launches our executable and calls us — the key diagnostic that
  was missing.

Both schema shapes are learned from the **public plist interface** of the
shipping module, not from Apple source; all values are synthetic.

These changes are still **unverified** — they need the `sudo` install + Image
Capture UI re-test below. A device appearing is success for this spike; the scan
path is out of scope.

## 1. Build and sign (no privileges needed)

From the repo root:

```bash
cmake -S . -B build
cmake --build build --target brscan-ica-loadspike
```

That produces `build/BrscanICALoadSpike.app`, copies `DeviceMatchingInfo.plist`
and `DeviceInfo.plist` into `Contents/Resources/`, and **ad-hoc** signs the
bundle (`codesign --sign -`), the cheapest signature to try first. Verify:

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

### Our own tracing (Task 1b) — the key diagnostic

The module now logs under its own subsystem. In a second terminal, before the
rescan:

```bash
log stream --predicate 'subsystem == "ai.jiffylabs.brscan.ica"' --info --debug
```

Interpret it as follows:

- **No lines at all** after a rescan → icdd never launched our executable. That
  is a *load/signing gate*, not a plist bug — correlate with the AMFI / library-
  validation query below; the fix is a stronger signature, not the plists.
- **`main: BrscanICALoadSpike executable launched`** and
  **`main: callbacks registered, entering ICD_ScannerMain`** appear → icdd loads
  and runs our code. From here, a missing device is a match/DeviceInfo problem,
  not a load problem.
- **`callback: …` lines** (e.g. `ICD_ScannerOpenTCPIPDevice`,
  `ICD_ScannerGetObjectInfo`) → icdd matched the device and is driving it; the
  device should be present in Image Capture.
- `main: ICD_ScannerMain returned …` should NOT appear in normal operation — it
  means the service loop exited.

A backward look at our trace specifically:

```bash
log show --last 10m --predicate 'subsystem == "ai.jiffylabs.brscan.ica"' --info --debug
```

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
