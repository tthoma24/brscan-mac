<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ICA module runbook — Plan 2 ("BrScan Mac ICA")

This is the device-in-the-loop runbook for the Plan 2 Image Capture (ICA) device
module, `BrScan Mac ICA`. The module now scans end-to-end from Apple's own scan
UI: it appears in **Image Capture** and **System Settings ▸ Printers & Scanners**,
renders a live progressive overview, and completes flatbed and document-feeder
scans in color, grayscale, and black & white, saving to the chosen destination.

The module began as a go/no-go *load spike* ("does a third-party ICA module still
load on this macOS?"). That question is settled — the module loads, binds, and
scans — so this runbook is now a **regression test matrix**: a repeatable checklist
that a tester runs against the real device after each change to confirm nothing
broke. The load-spike history is preserved in the [History appendix](#history-appendix-the-load-spike)
for provenance.

Every check here is manual and needs the physical Brother MFC-J6920DW on the
network, because none of it can be automated. The hermetic parameter, paper-size,
and buffer-descriptor logic is covered by the GoogleTest suite in CI and is out of
scope for this runbook.

## Scope

This runbook covers the **ICA / Image Capture host-initiated** scan path only —
the path where the user drives a scan from Image Capture or Printers & Scanners
and the module serves it over `libbrscan`.

It does **not** cover the scan-button flow, the Plan 1e panel toggles
(skip-blank, high-speed, OCR sub-formats), or the config GUI `.app`. Those are a
separate surface with their own front end and their own docs:

- Scan-button flow — [BUTTON.md](BUTTON.md).
- Config GUI and panel toggles — [../gui/README.md](../gui/README.md).

For the module's clean-room interface reference (callback lifecycle, the
`theDict["device"]` capability schema, the notification sequence, ADF centering,
duplex, feeder-empty mapping), see [ICA-PROTOCOL.md](ICA-PROTOCOL.md). For the
design and task history, see [PLAN-2-DESIGN.md](PLAN-2-DESIGN.md).

## Re-test loop

Run this loop after every change, before touching the matrix. It builds the
bundle, verifies the signature, installs it, reaps the stale module child, clears
the device-info cache, restarts `icdd`, and opens the scan UI. Each step exists
because skipping it silently re-tests the *previous* build — the hard-won lesson
of the load spike.

### 1. Build and verify the signature

From the repo root:

```bash
cmake -S . -B build
cmake --build build --target brscan-ica
```

That produces `build/BrScan Mac ICA.app`, copies `DeviceMatchingInfo.plist` and
`DeviceInfo.plist` into `Contents/Resources/`, writes `Contents/PkgInfo`, and
**ad-hoc** signs the bundle. Verify the signature:

```bash
codesign --verify --deep --strict --verbose=2 "build/BrScan Mac ICA.app"
codesign -dvvv "build/BrScan Mac ICA.app"    # Signature=adhoc, flags=0x2(adhoc)
```

Ad-hoc signing is enough for a local install. Gatekeeper rejects an ad-hoc bundle
(`spctl -a -t exec` → "rejected"); that is a *distribution* check, not the *load*
check `icdd` performs, so ignore it here. See [DISTRIBUTION.md](DISTRIBUTION.md)
for the Developer-ID + notarization path.

### 2. Install into the system directory (privileged)

ICA modules load only from `/Library/Image Capture/Devices/` — there is no
user-writable location — so this step needs admin and is kept out of the build:

```bash
sudo ica-module/install.sh install
```

### 3. Reap the stale module child

`icdd` launches the module as a **child** process that keeps running after
`killall icdd`, so a freshly reinstalled bundle does not take effect until the old
child dies. Symptom: the log keeps showing the *previous* build's lines. Reap the
child, then prove the *installed* binary carries your change:

```bash
sudo killall "BrScan Mac ICA" 2>/dev/null || true
# Prove the installed copy (not just build/) has your change — grep an os_log
# format literal you added; it is embedded in the binary:
strings "/Library/Image Capture/Devices/BrScan Mac ICA.app/Contents/MacOS/BrScan Mac ICA" \
  | grep "<a string you just added>"
ps -Ao pid,lstart,comm | grep -i brscan   # no stale PID older than the reinstall
```

### 4. Clear the device-info cache

`icdd` caches per-device presentation state at
`~/Library/Application Support/icdd/deviceInfoCacheV2.plist`, keyed by device
UUID. Stale entries suppress a fresh match, so clear the cache before re-testing:

```bash
rm -f ~/Library/Application\ Support/icdd/deviceInfoCacheV2.plist
```

### 5. Kick icdd

```bash
killall icdd 2>/dev/null || launchctl kickstart -k gui/$(id -u)/com.apple.icdd
```

### 6. Stream the module log, then open the scan UI

Start the module's log stream in one terminal before you open the UI. This
compact predicate follows the module's own subsystem — the callback lifecycle,
the scan progress, and the completion:

```bash
log stream --predicate 'subsystem == "me.tthoma24.brscan.ica"' --style compact --info --debug
```

Then open **Image Capture** (and keep it open — `icdd` only launches the module
while a client is browsing) with the device on the network, and select
`Brother MFC-J6920DW`. A healthy launch prints the module entering
`ICD_ScannerMain` and then its per-scan callback lines.

If the device does **not** appear at all, switch to the fuller launch/AMFI
discriminator predicate in the [History appendix](#diagnosing-a-non-appearance)
to tell a signing denial from a match gap.

### Uninstall when done

```bash
sudo ica-module/install.sh uninstall
killall icdd 2>/dev/null || true
```

## Test matrix

Work top to bottom. Mark each row Pass or Fail and record what you saw in Notes;
a Fail with a note is the useful output of a session. The **PRs** in parentheses
are the change that landed each behavior, for provenance when a row regresses.

Legend for the `ICScannerDocumentType` values referenced below is in
[ICA-PROTOCOL.md](ICA-PROTOCOL.md#capability-schema-icd_scannergetparameters).

### A. Discovery and open

| Scenario | Preconditions | Steps | Expected result | Pass/Fail | Notes |
|---|---|---|---|---|---|
| **A1. Appears in Image Capture** | Module installed; re-test loop run; device on network | Open Image Capture | `Brother MFC-J6920DW` appears in the source list | ☐ | |
| **A2. Appears in Printers & Scanners** | As A1 | Open System Settings ▸ Printers & Scanners ▸ select the device ▸ **Scan** tab ▸ **Open Scanner** | The same ICA scan UI opens; the device is listed | ☐ | Same ICA path as Image Capture |
| **A3. Scan panel + pickers populate** | Device selected in either UI | Open the scan panel; inspect every control | Pickers populate: **Kind** (Flatbed / Document Feeder), **Resolution/DPI**, mode (Color / Gray / B&W), **Size**, **Format**; the feeder exposes a **Duplex / 2-sided** control (#67, #68, #76, #80) | ☐ | No control implies the capability schema regressed |

### B. Flatbed (platen)

| Scenario | Preconditions | Steps | Expected result | Pass/Fail | Notes |
|---|---|---|---|---|---|
| **B1. Live progressive overview + geometry** | Kind = Flatbed; original on the glass | Trigger **Overview** / preview | The overview fills **band-by-band top-to-bottom** (live progressive, not only on completion), with a moving progress bar; a **Letter** selection is correctly proportioned against the A3 platen (#77, #78, #75) | ☐ | Proportion live-confirmed in #75. Residual nuance: the platen extent is advertised as a union (A3 width × Ledger height), not one real glass rectangle — see [ICA-PROTOCOL Geometry](ICA-PROTOCOL.md#geometry-platen-extent--open). Confirm the Letter proportion still looks right |
| **B2. Low-resolution scan** | Kind = Flatbed | Set a low DPI (e.g. 100 or 150); Scan | Image completes at the selected DPI | ☐ | |
| **B3. High-resolution scan** | Kind = Flatbed | Set a high DPI (e.g. 600); Scan | Image completes at the selected DPI; pixel dimensions scale accordingly | ☐ | Clamped to the device `ESC I` offer max |
| **B4. Color mode** | Kind = Flatbed | Mode = Color; Scan | Correct 24-bit color image | ☐ | kRgb decoded via DecodeJpeg |
| **B5. Grayscale mode** | Kind = Flatbed | Mode = Gray; Scan | Correct 8-bit grayscale image | ☐ | |
| **B6. Black & white mode** | Kind = Flatbed | Mode = B&W; Scan | Correct 1-bit bitonal image; `1 = black` renders as ink | ☐ | |
| **B7. Flatbed size sweep** | Kind = Flatbed | Step through the size menu, scanning (or previewing) each: **Default (Auto)**, A3, US Ledger, A4, US Letter, US Legal, A5, A6, US Executive, JIS B4, JIS B5, Business Card, 4R, 3R, 5R (#82) | Each selection crops the scan area to the named size | ☐ | Tester has Letter/Legal/A4 media. For smaller sizes, place a smaller original and confirm the crop matches. Note per size whether real media was used or eyeballed |
| **B8. File / format save** | Kind = Flatbed; a save destination and format chosen | Scan to the chosen folder in the chosen format (TIFF/JPEG/PNG) | The scan **saves to the chosen destination** as `<name>.<ext>` in the chosen format (#70, #71) | ☐ | Uses the security-scoped destination URL |
| **B9. Cancel mid-scan** | Kind = Flatbed; a scan running | Press **Cancel** while the scan is in progress | The scan aborts promptly and cleanly; no file is written; the device is ready for the next scan (#66, #71) | ☐ | Mid-scan cancel via `userCanceledErr` on a band reply |

### C. ADF (document feeder)

| Scenario | Preconditions | Steps | Expected result | Pass/Fail | Notes |
|---|---|---|---|---|---|
| **C1. Feeder actually feeds** | Kind = Document Feeder; sheets loaded in the ADF | Scan | The **sheets feed through the ADF** (not the glass); output matches the fed pages (#79) | ☐ | Confirms source resolves to kAdf, not flatbed |
| **C2. Centered window — Letter** | Kind = Document Feeder; a **Letter** sheet in the ADF | Scan | The page is **centered**: no left padding, no right-edge cut-off (#81) | ☐ | Device center-registers; module re-centers the window |
| **C3. Centered window — Legal** | Kind = Document Feeder; a **Legal** sheet in the ADF | Scan | Centered as C2, no cut-off, on Legal | ☐ | |
| **C4. Duplex (2-sided)** | Kind = Document Feeder; a 2-sided original; Duplex enabled | Scan | Front and back pages come out, correct and in order (#80) | ☐ | Duplex honored only for the feeder |
| **C5. Feeder size set** | Kind = Document Feeder | Inspect the size menu | Offers: **Default (Auto)**, US Letter, US Legal, A4, US Ledger, A3, A5, US Executive, JIS B4, JIS B5 — and **no** A6/photos/Business Card (below the ADF minimum width) (#80, #82) | ☐ | Scan the sizes you have media for |
| **C6. Multi-page feed** | Kind = Document Feeder; several sheets loaded | Scan | **Multi-page output** — one page per fed sheet, in order | ☐ | File transfer appends a page index |
| **C7. Empty ADF — simplex** | Kind = Document Feeder; **no paper**; Duplex off | Press **Scan** | Image Capture promptly shows the **native alert "Scanner reported an error / Document feeder is empty."** (#84, #85) | ☐ | Instant via the `ESC D` ack (`0xc2`), not a timeout |
| **C8. Empty ADF — 2-sided** | Kind = Document Feeder; **no paper**; Duplex on | Press **Scan** | Same native "Document feeder is empty." alert, promptly (#84, #85) | ☐ | Must fire for duplex as well as simplex |

### D. Packaging and signing

| Scenario | Preconditions | Steps | Expected result | Pass/Fail | Notes |
|---|---|---|---|---|---|
| **D1. Installed bundle verifies + identifier** | Module installed | `codesign --verify --deep --strict --verbose=2 "/Library/Image Capture/Devices/BrScan Mac ICA.app"` then `codesign -dvvv` on it | Verify passes; the identifier is `me.tthoma24.brscan.ica`; signature is ad-hoc | ☐ | |
| **D2. Distribution path documented** | — | Read [DISTRIBUTION.md](DISTRIBUTION.md) | Ad-hoc suffices for the local install; Developer-ID + `notarytool` is the documented future step for redistribution | ☐ | No notarization needed for local testing |

## History appendix — the load spike

Before any scan-path code existed, Plan 2 opened with a single go/no-go question:
**does a third-party ICA device module still load on this macOS, and under what
signing?** If a third-party module would not load, or loaded only after
Developer-ID notarization on every iteration, Plan 2 stopped and the project fell
back to Plan 3 (AirSane/eSCL). The module under test was deliberately inert — a
background-only `.app` that registered no-op `ICD_Scanner…` callbacks and called
`ICD_ScannerMain`, running no scan. Its only job was to appear. That question is
now settled; the notes below record how, for provenance.

- **Task 1b — plist schema corrected.** The first spike installed and `icdd` read
  its `DeviceMatchingInfo.plist`, but Image Capture showed **0 devices**. The
  real schema, learned from Apple's shipping `AirScanScanner.app`, is a top-level
  `BonjourNetwork` dict keyed by the Bonjour service type, each value carrying
  `device type = scanner`, plus a top-level `Version = 1.0`; a `DeviceInfo.plist`
  is also required. Both were added, and `os_log` tracing (subsystem
  `me.tthoma24.brscan.ica`) was wired in.
- **Task 1c — rename and bundle parity.** The identifier was renamed
  `ai.jiffylabs` → `me.tthoma24` throughout (bundle id, `os_log` subsystem, log
  predicates). Inspecting Apple's module confirmed `icdd` launches modules as
  **processes** (it does not `dlsym` an entry point), and that ad-hoc code
  executes on this macOS. `CFBundleSupportedPlatforms` and `Contents/PkgInfo`
  were added for LaunchServices parity.
- **A vs B — the discriminator.** "Zero of our `os_log` lines" did not by itself
  prove a signing gate. `icdd` launches a module only after a Bonjour browse
  matches a device to it, so silence was equally **(A)** a signing /
  library-validation denial or **(B)** a match/bundle gap where no launch was even
  attempted. The two were told apart by streaming the `icdd` log (was a launch
  *attempted*?) alongside AMFI (was it *denied*?).
- **Task 1d — the match gap, fixable.** The evidence pointed at **B**: no launch
  attempt, no AMFI line. The cause was a missing TXT criterion — the match dict
  carried only `device type = scanner`. Adding `ICABonjourTXTRecordKey = { mdl =
  MFC-J6920DW; mfg = Brother; }` (the device's own advertised TXT, captured with
  `dns-sd -L`) and `device events = ( scan )` let `icdd` bind the device and
  attempt the launch — which then succeeded on the ad-hoc signature. Plan 2 was
  green, and the scan path (Tasks 2–25, PRs #62–#85) followed.

### Diagnosing a non-appearance

If the device stops appearing after a change, reuse the A-vs-B discriminator
stream — it merges the `icdd` launch log, AMFI / `amfid` denials, and the module's
own subsystem, so one run tells a signing denial (A) from a match gap (B):

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

- **A (signing / library-validation):** `icdd` logs a launch *attempt* for
  `BrScan Mac ICA.app`, followed by an AMFI / codesign *denial* naming the bundle
  or `me.tthoma24.brscan.ica`. The fix is a stronger signature (Developer-ID +
  notarization), not the plists.
- **B (match / bundle gap):** **no** launch attempt and **no** AMFI line naming
  the bundle. `icdd` browsed `_scanner._tcp.` but never bound the device to the
  module — a match-dict, `DeviceInfo`, or cache problem. Confirm the cache was
  cleared (loop step 4) and the `Missing DeviceMatchingInfo.plist in '…'!` warning
  is absent, then check the match dict.

Backward look (same predicate) if you missed the live stream:

```bash
log show --last 10m --info --debug --predicate '
  process == "icdd" OR process == "amfid"
  OR sender == "AppleMobileFileIntegrity"
  OR subsystem == "me.tthoma24.brscan.ica"
  OR eventMessage CONTAINS[c] "library validation"'
```
