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
  `me.tthoma24.brscan.ica`, category `loadspike`) at `main` /
  `ICD_ScannerMain` entry and in every registered callback, so a re-run shows
  whether icdd launches our executable and calls us.

## Task 1c update — rename, bundle parity, and a re-test that separates A vs B

The earlier interpretation ("zero of our `os_log` lines ⇒ a signing/load gate")
was wrong, and this task corrects it. What we established locally:

- **Renamed** the identifier `ai.jiffylabs` → `me.tthoma24` everywhere: bundle id
  `me.tthoma24.brscan.ica-loadspike`, `os_log` subsystem `me.tthoma24.brscan.ica`,
  and the predicates in this runbook.
- **How icdd loads a module — confirmed by inspecting Apple's shipping module,
  not source.** `nm -gU` on
  `/System/Library/Image Capture/Devices/AirScanScanner.app/Contents/MacOS/AirScanScanner`
  exports **no** `ICD_` symbols; it only *imports* `_ICD_ScannerMain` and
  `_gICDScannerCallbackFunctions` from the framework, and its `Info.plist` is a
  plain `CFBundlePackageType APPL` app. icdd's own strings show it launches
  modules as **processes** (`launchDeviceModule`,
  `launchDeviceModuleForBrowseID:fromClientPID:runningPID:`,
  `launchedTaskWithLaunchPath:arguments:`, `openApplicationAtURL:configuration:`)
  — it does **not** `dlsym` an entry point out of the module. Our bundle already
  matches this contract: `main()` fills `gICDScannerCallbackFunctions` and calls
  `ICD_ScannerMain`, and our imports are identical to Apple's. **So "entry points
  not exported under the names icdd looks up" is not the gap — there is no such
  lookup.**
- **Ad-hoc signing is not a blanket exec/load gate.** Running our ad-hoc bundle's
  binary directly launches it, loads `ICADevices.framework`, enters
  `ICD_ScannerMain`, and emits our `os_log` lines. Ad-hoc code executes on this
  macOS; the only open A-question is whether icdd's *launch context* additionally
  enforces library validation on the child — which only the live re-test settles.
- **Bundle-parity fixes** (close plausible match/load B-gaps, learned from Apple's
  keys, values our own): added `CFBundleSupportedPlatforms = [MacOSX]` and a
  `Contents/PkgInfo` (`APPL????`) to match Apple's module for the LaunchServices
  path icdd uses.

**Why "zero of our `os_log` lines" does NOT prove A.** icdd launches a module
only *after* a Bonjour browse resolves a device and matches it to that module
(the `launchDeviceModuleForBrowseID:` path). So no os_log lines means only "the
module process was never started" — which is equally:

- **(A) signing/library-validation gate:** icdd *tried* to launch our module and
  the launch was denied, or
- **(B) match/bundle gap:** icdd *never tried* — no `_scanner._tcp.` device
  matched our module, so there was nothing to launch.

Our subsystem is silent in both. The discriminator is in the **icdd** log (did a
launch get *attempted*?) plus AMFI (was a launch *denied*?), streamed together in
§3 below so one re-test tells A from B.

Both schema shapes are learned from the **public plist interface** of the
shipping module, not from Apple source; all values are synthetic.

These changes are still **unverified** — they need the `sudo` install + Image
Capture UI re-test below. A device appearing is success for this spike; the scan
path is out of scope.

## Task 1d update — the match/binding gap is FIXABLE (missing TXT match key)

Task 1d answered the crux Plan 2 question with the earlier re-test's evidence
already in hand: our `me.tthoma24.brscan.ica` subsystem is silent AND no AMFI /
codesign line names our bundle — i.e. **no launch was even *attempted***. That
is scenario **B (match/binding gap)**, not A (signing). The question left was
whether B is fixable or structural — does icdd dispatch a raw `_scanner._tcp.`
device to a *third-party* module at all?

**Finding: FIXABLE. icdd does dispatch `_scanner._tcp.` to third-party modules;
our match dict was just missing the TXT criterion icdd binds on.** The evidence,
from Apple's public plugin interface and the device's own broadcast:

1. **The bind test is a TXT-record comparison, and our match dict gave it
   nothing to compare.** icdd exports the selector
   `compareBonjourDeviceModuleDictionary:withBonjourTXTRecord:` and
   `baseDictionaryForBonjourServiceType:andTXTRecords:devices:` (strings in
   `/System/Library/Image Capture/Support/icdd`). The comparison needs an
   **`ICABonjourTXTRecordKey`** sub-dict of TXT key/value pairs to test against
   the device's advertised TXT record. `ICABonjourTXTRecordKey` is Apple's
   public match key — Apple's own `AirScanScanner.app` uses exactly it to match
   its service type on Bonjour TXT pairs. Our Task 1b/1c dict carried only
   `device type = scanner` — no `ICABonjourTXTRecordKey` — so the comparison had
   no TXT pair to test and the device never bound.
2. **The device advertises usable identity TXT keys.** Captured independently on
   the live network:

   ```
   dns-sd -L "Brother MFC-J6920DW" _scanner._tcp local
   # ... can be reached at <host>.local.:54921
   #   txtvers=1 ty=Brother MFC-J6920DW mfg=Brother mdl=MFC-J6920DW
   #   button=T feeder=T flatbed=T
   ```

   The device does **not** advertise eSCL `_uscan._tcp.` at all (`dns-sd -B
   _uscan._tcp local` is empty), so `_scanner._tcp.` is the only network scan
   path it exposes.

Whether icdd will actually dispatch a raw `_scanner._tcp.` device to a
third-party module is what the §3 re-test settles — Apple's `AirScanScanner`
demonstrates the match *interface* on its own service type, not this dispatch;
this fix supplies the missing TXT criterion so binding, and then a launch
attempt, can finally happen.

**The fix (applied):** `ica-module/DeviceMatchingInfo.plist`'s `_scanner._tcp.`
match dict now carries `ICABonjourTXTRecordKey = { mdl = MFC-J6920DW; mfg =
Brother; }` and `device events = ( scan )`, matching the proven schema. The two
values are the device's own publicly-broadcast TXT strings (from the `dns-sd`
capture above), captured black-box; `mdl`/`mfg` are device-class identity
(the project's public target model), while the synthetic per-unit Bonjour name
`BRW00AABBCCDDEE` stays in `DeviceInfo.plist`.

**Residual risk — the signing gate (A) is the NEXT gate, still unverified.**
Making the device *bind* is what lets icdd finally *attempt* to launch our
module. Only then can the library-validation / Developer-ID question actually
fire (Apple's own modules in the system directory are Team-signed and carry the
library-validation flag, so an ad-hoc third-party module may still be refused).
So a clean bind
does not by itself prove Plan 2 green; it unblocks the go/no-go by turning the
silent non-result into a real launch attempt whose success or AMFI-denial the
§3 stream will now show. If, after this fix, the device binds and icdd launches
our ad-hoc module (success log line below), Plan 2 is go; if the launch is
AMFI-denied, that is the signing wall and the fallback is Plan 3.

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

## 3. One re-test that tells A (signing gate) from B (match/bundle gap)

The point of this run is to see, in a single log stream, **whether icdd
*attempted* to launch our module and, if so, whether the launch was *denied*.**
That, not the presence/absence of our own `os_log` lines, is what separates A
from B.

### 3.0 Clear the stale device-info cache first (required)

icdd caches per-device presentation state at
`~/Library/Application Support/icdd/deviceInfoCacheV2.plist`, keyed by device
UUID. Earlier runs (wrong-schema plist, old bundle id) can leave entries that
suppress a fresh match, so clear it before the re-test:

```bash
rm -f ~/Library/Application\ Support/icdd/deviceInfoCacheV2.plist
killall icdd 2>/dev/null || launchctl kickstart -k gui/$(id -u)/com.apple.icdd
```

### 3.1 Stream everything the discriminator needs, in one terminal

Start this **before** the rescan. It merges the icdd log (launch attempts,
Bonjour browse, the `Missing DeviceMatchingInfo.plist` warning), AMFI / `amfid`
(library-validation and launch denials), and our own subsystem:

```bash
log stream --info --debug --predicate '
  process == "icdd"
  OR process == "amfid"
  OR sender == "AppleMobileFileIntegrity"
  OR subsystem == "me.tthoma24.brscan.ica"
  OR eventMessage CONTAINS[c] "library validation"
  OR eventMessage CONTAINS[c] "code signature"
  OR eventMessage CONTAINS[c] "was denied"'
```

Then trigger discovery: open **Image Capture** (and keep it open — icdd only
launches a module when a client is browsing) with the real `_scanner._tcp.`
device on the network. Optionally confirm the device advertises the type we
declare, in another terminal:

```bash
dns-sd -B _scanner._tcp local     # our module declares this type
dns-sd -B _uscan._tcp local       # eSCL/AirScan; Apple's module claims this
```

### 3.2 Read the stream — the exact lines that decide A vs B

- **A — signing / library-validation gate (→ no-go, go to Plan 3):** icdd logs a
  launch **attempt** for our bundle — a line naming `BrscanICALoadSpike.app` from
  `launchDeviceModule` / `launchDeviceModuleForBrowseID:` /
  `openApplicationAtURL:` — **and** it is followed by an AMFI / codesign denial:
  a line from `AppleMobileFileIntegrity` or `amfid`, or containing
  `library validation failed`, `code signature`, or `Launch … was denied`, that
  names `me.tthoma24.brscan.ica-loadspike` or the bundle path. Our own
  `me.tthoma24.brscan.ica` subsystem stays **silent** (the child was killed
  before `main`). This means ad-hoc is not enough to *load*; the bar is
  Developer-ID + notarization, which turns every iteration into a notarization
  round trip → **Plan 2 is not viable without an Apple-level signature; take
  Plan 3.**
- **B — match / bundle gap (→ fixable):** there is **no** launch attempt for our
  bundle in the icdd log and **no** AMFI/codesign line naming it. icdd browsed
  `_scanner._tcp.` but either resolved no device (then `dns-sd -B _scanner._tcp`
  is also empty → the Brother advertises only eSCL `_uscan._tcp.`, so declare
  that type instead) or resolved one without binding it to our module (a
  match-dict/`DeviceInfo` gap). The fix is in our plists/bundle, not the
  signature.
- **Success (spike passes):** our subsystem prints
  `main: BrscanICALoadSpike executable launched` then
  `main: callbacks registered, entering ICD_ScannerMain` — icdd launched and ran
  our code. `callback: …` lines mean it is driving the device; it should now
  appear in Image Capture. `main: ICD_ScannerMain returned …` should NOT appear
  (it means the service loop exited).

Watch for the `Missing DeviceMatchingInfo.plist in '…'!` warning: it should
**not** fire (the file is present); if it does, the bundle is in the wrong place
or unreadable — a third, trivially-fixable variant of B.

Backward look (same predicate) if you missed the live stream:

```bash
log show --last 10m --info --debug --predicate '
  process == "icdd" OR process == "amfid"
  OR sender == "AppleMobileFileIntegrity"
  OR subsystem == "me.tthoma24.brscan.ica"
  OR eventMessage CONTAINS[c] "library validation"'
```

## 4. The manual UI check (the actual go/no-go)

The definitive pass is visual and cannot be automated:

1. With the module installed and icdd restarted, open **Image Capture** and
   **System Settings ▸ Printers & Scanners**.
2. **Go** = the synthetic device `BRW00AABBCCDDEE` appears in the source list
   (even if selecting it does nothing — this spike has no scan path).
3. **No-go** = it never appears. Correlate with §3.2 to classify it: a denied
   launch of our bundle = **A** (signature/library-validation → Plan 3); no
   launch attempt at all = **B** (match/bundle gap → fixable in our plists).

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
