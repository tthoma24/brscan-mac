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
| **A3. Scan panel + pickers populate** | Device selected in either UI | Open the scan panel; inspect every control | Pickers populate: **Scan Mode** (Flatbed / Document Feeder — the source), **Resolution/DPI**, **Kind** (Color / Black & White / Text — the pixel type), **Size**, **Format**; the feeder exposes a **Duplex / 2-sided** control (#67, #68, #76, #80) | ☐ | No control implies the capability schema regressed. Note the two popups Image Capture confusingly places together: **Scan Mode** is the source, **Kind** is the color mode |
| **A4. Kind popup reflects our pixel types** | Device selected in either UI | Open the scan panel; open the **Kind** popup | The **Kind** popup offers **Color**, **Black & White**, and **Text** — Image Capture builds this list host-side from our advertised `ICAP_PIXELTYPE` {RGB, Gray, BW} (`ica-module/scan_parameters.mm`) | ☐ | Verifies host-side Kind construction against **our** module, not Brother's. If the list is wrong (e.g. a missing or mislabeled entry), see the A5 Notes and issue [#143](https://github.com/tthoma24/brscan-mac/issues/143) |
| **A5. Format menu is gated by Kind** | Device selected in either UI; A4 confirmed | For each **Kind**, open the **Format** menu and note the offered formats: select **Text**, then **Black & White**, then **Color** | The **Format** list is gated by Kind, matching Brother's native driver: **Text** (1-bit) → **TIFF, PNG, PDF** only; **Black & White** (grayscale) → adds **JPEG, JPEG 2000**; **HEIC** appears **only under Color**. This gating is Apple's host-side rule, derived from the Kind's pixel type — our module does not list it | ☐ | Only observed with Brother's native module before, so confirm it against ours. If the Kind renders wrong or the Format list is not gated as above, the likely lever is making `ICAP_BITDEPTH` track pixel type (1-bit under BW, 8-bit under Gray/RGB) instead of the current flat `{1, 8}` — see issue [#143](https://github.com/tthoma24/brscan-mac/issues/143) |

### B. Flatbed (platen)

| Scenario | Preconditions | Steps | Expected result | Pass/Fail | Notes |
|---|---|---|---|---|---|
| **B1. Live progressive overview + geometry** | Scan Mode = Flatbed; original on the glass | Trigger **Overview** / preview | The overview fills **band-by-band top-to-bottom** (live progressive, not only on completion), with a moving progress bar; a **Letter** selection is correctly proportioned against the A3 platen (#77, #78, #75) | ☐ | Proportion live-confirmed in #75. Residual nuance: the platen extent is advertised as a union (A3 width × Ledger height), not one real glass rectangle — see [ICA-PROTOCOL Geometry](ICA-PROTOCOL.md#geometry-platen-extent--open). Confirm the Letter proportion still looks right |
| **B2. Low-resolution scan** | Scan Mode = Flatbed; **Kind = Color** | Set **Resolution = 100 or 150**; Scan | Image completes at the selected DPI | ☐ | The resolution sweep (B2–B5) runs in Color; the per-mode correctness rows are B6–B8 at 300 dpi |
| **B3. Mid-resolution scan** | Scan Mode = Flatbed; **Kind = Color** | Set **Resolution = 600**; Scan | Completes; pixel dimensions scale with DPI | ☐ | Clamped to the device `ESC I` offer max |
| **B4. 1200 dpi scan** | Scan Mode = Flatbed; **Kind = Color** | Set **Resolution = 1200**; Scan | Completes at 1200 dpi; pixel dimensions are 2x the 600 dpi (B3) scan; **opened in Preview the file reports 1200 ppi, not 72** | ☐ | The flatbed now advertises 1200 and 2400 (the manufacturer driver exposes 1200; 2400 is the flatbed optical max) — confirm the menu offers both. Two caps used to hide these: the advertised `ICAP_XRESOLUTION` list stopped at 600 and `TranslateScanParams` clamped every request to 600; PR D lifts both, source-dependent |
| **B5. 2400 dpi scan (small crop)** | Scan Mode = Flatbed; **Kind = Color** | Set **Resolution = 2400**; **crop to a SMALL area** (e.g. a ~1x1 in region); Scan | Completes at 2400 dpi over the cropped region; **Preview reports 2400 ppi, not 72** | ☐ | **Use a small crop, not full A3.** A full-glass A3 page at 2400 dpi is ~28k x 40k px (~3 GB decoded RGB) — expect heavy memory and time. The whole-page buffer math is 64-bit (`buffer_descriptor` accumulates stride/size in `int64_t`; the delivery path carries page byte counts as `size_t`), so nothing wraps |
| **B6. Color** | Scan Mode = Flatbed; **Resolution = 300** | **Kind = Color**; Scan | Correct 24-bit color image | ☐ | Kind **Color** = pixel type RGB → `kColor`; kRgb decoded via DecodeJpeg |
| **B7. Grayscale** | Scan Mode = Flatbed; **Resolution = 300** | **Kind = Black & White**; Scan | Correct 8-bit grayscale image | ☐ | In Image Capture, **grayscale is the "Black & White" Kind** (pixel type Gray → `kTrueGray`/GRAY256, the path #150 routes ICA gray to and fixes) — **not** "Text", and there is no Kind literally named "Gray" |
| **B8. 1-bit black & white** | Scan Mode = Flatbed; **Resolution = 300** | **Kind = Text**; Scan | Correct 1-bit bitonal image; `1 = black` renders as ink | ☐ | Image Capture's **"Text" Kind is the 1-bit path** (pixel type BW → `kBlackWhite`). Confirmed by the Format gating (A5): Text → no JPEG (1-bit); Black & White → adds JPEG (grayscale) |
| **B9. Flatbed size sweep** | Scan Mode = Flatbed; **Resolution = 300** | Step through the size menu, scanning (or previewing) each: **Default (Auto)**, A3, US Ledger, A4, US Letter, US Legal, A5, A6, US Executive, JIS B4, JIS B5, Business Card, 4R, 3R, 5R (#82) | Each selection crops the scan area to the named size | ☐ | Tester has Letter/Legal/A4 media. For smaller sizes, place a smaller original and confirm the crop matches. Note per size whether real media was used or eyeballed |
| **B10. File / format save** | Scan Mode = Flatbed; a save destination and format chosen | Scan to the chosen folder in the chosen format (TIFF/JPEG/PNG) | The scan **saves to the chosen destination** as `<name>.<ext>` in the chosen format (#70, #71) | ☐ | Uses the security-scoped destination URL |
| **B11. Cancel mid-scan** | Scan Mode = Flatbed; a scan running | Press **Cancel** while the scan is in progress | The scan aborts promptly and cleanly; no file is written; the device is ready for the next scan (#66, #71) | ☐ | Mid-scan cancel via `userCanceledErr` on a band reply |

### C. ADF (document feeder)

| Scenario | Preconditions | Steps | Expected result | Pass/Fail | Notes |
|---|---|---|---|---|---|
| **C1. Feeder actually feeds** | Scan Mode = Document Feeder; **Kind = Color**; **Resolution = 300**; sheets loaded in the ADF | Scan | The **sheets feed through the ADF** (not the glass); output matches the fed pages (#79) | ☐ | Confirms source resolves to kAdf, not flatbed |
| **C2. Centered window — Letter** | Scan Mode = Document Feeder; **Resolution = 300**; a **Letter** sheet in the ADF | Scan | The page is **centered**: no left padding, no right-edge cut-off (#81) | ☐ | Device center-registers; module re-centers the window |
| **C3. Centered window — Legal** | Scan Mode = Document Feeder; **Resolution = 300**; a **Legal** sheet in the ADF | Scan | Centered as C2, no cut-off, on Legal | ☐ | |
| **C4. Low-resolution ADF scan** | Scan Mode = Document Feeder; **Kind = Color**; a sheet in the ADF | Set **Resolution = 100 or 150**; Scan | The fed page completes at the selected DPI | ☐ | The ADF resolution sweep (C4–C7) runs in Color |
| **C5. Mid-resolution ADF scan** | Scan Mode = Document Feeder; **Kind = Color**; a sheet | Set **Resolution = 600**; Scan | Completes; pixel dimensions scale with DPI | ☐ | |
| **C6. 1200 dpi ADF scan** | Scan Mode = Document Feeder; **Kind = Color**; a sheet | Set **Resolution = 1200**; Scan | The fed page **completes at 1200 dpi**, centered as C2; **Preview reports 1200 ppi, not 72** | ☐ | 1200 is the ADF optical max. A 2400 dpi request on the ADF (if the host somehow sends one) is clamped to 1200 by the per-source runtime cap |
| **C7. ADF Resolution menu stops at 1200 (no 2400)** | Scan Mode = Document Feeder | Open the **Resolution** menu | Offers up to **1200 dpi** and **NOT 2400** — the ADF's optical maximum is 1200 dpi (the 2400 x 1200 sensor asymmetry) | ☐ | The flatbed menu (B4) still lists 2400; only the feeder drops it. Both share the base set 100/150/200/300/400/600 |
| **C8. Duplex (2-sided) — Color** | Scan Mode = Document Feeder; **Resolution = 300**; a 2-sided original; **Kind = Color**; Duplex enabled | Scan | Front and back pages come out, correct and in order (#80) | ☐ | Duplex honored only for the feeder. Color de-interleaves the two sides' JPEG chunks by page index (`RunColorScan`); the grayscale/black-and-white de-interleave is C9 below |
| **C9. Duplex — grayscale / black & white / text (de-interleave)** | Scan Mode = Document Feeder; **Resolution = 300**; a 2-sided original; **Kind = Black & White** (grayscale/RLENGTH), then repeat with **Kind = Text** (1-bit); Duplex on | Scan each Kind | **All pages come out, correct and in order** — front and back of every sheet. This is the RLENGTH duplex de-interleave (`RunRlengthScan`), which routes each side's interleaved row blocks to its own page by the header's page index. Before it, this scan failed with `status=2 pages=0` (C8's color path already de-interleaved) | ☐ | **Confirms INFERRED framing.** No real duplex gray/RLENGTH capture exists, so the de-interleave framing (page index in header byte[3], the 10-byte end-of-page marker, and the `0x80` job-final) is inferred from the color duplex framing — same device, same markers — and verified only by a synthetic interleaved unit test (`RunScan.{TrueGray,BlackWhite}AdfDuplexInterleavedPagesDeinterleave`). A **Pass** here is the device-in-the-loop confirmation; a **Fail** (missing pages, wrong order, or a scan error) means the real gray/RLENGTH duplex framing differs from color and needs a captured stream |
| **C10. Multi-page / duplex ADF viewed IN-APP yields all pages** | Scan Mode = Document Feeder; **Resolution = 300**; several sheets (or a 2-sided original with Duplex on); **no file destination** — scan into the Image Capture window (`fileTransfer == false`); run once with **Kind = Color**, then once with **Kind = Black & White** | Scan and watch the window | **Every fed page appears as its OWN image** — one per sheet (per side, for duplex), in order — not all collapsed onto the first. Each page's overview still fills band-by-band top-to-bottom (B1) | ☐ | **Confirms the in-memory per-page delivery (PR B).** In-app, the bands are the pixel delivery and the host accumulates them into one overview keyed on row offset; every page's bands restart at row 0, so before this fix pages 2..N overwrote page 1. The module now posts a path-less `kICANotificationTypeScannerPageDone` at each page boundary — decided by the pure `InMemoryPageSplitter` (`ica-module/page_delivery.h`), N pages → N-1 boundaries, the last closed by `ScannerScanDone`. **ASSUMPTION under test:** that a document-name-less `ScannerPageDone` makes the host snapshot the current overview into a distinct image and reset the accumulator. A **Fail** (one collapsed image, a blank/duplicated page, or a hang) means the host needs a different per-page signal — capture the `me.tthoma24.brscan.ica` log (`inMemPageBoundaries=N` and the per-boundary `ScannerPageDone (no path)` lines). The FILE path (folder/PDF/JPEG) already delivered all pages (C12, live-confirmed) and is unchanged; single-page/flatbed crosses no boundary and is byte-identical to before. Run **Color first** to isolate this from C9 |
| **C11. Feeder size set** | Scan Mode = Document Feeder | Inspect the size menu | Offers: **Default (Auto)**, US Letter, US Legal, A4, US Ledger, A3, A5, US Executive, JIS B4, JIS B5 — and **no** A6/photos/Business Card (below the ADF minimum width) (#80, #82) | ☐ | Scan the sizes you have media for |
| **C12. Multi-page feed** | Scan Mode = Document Feeder; **Resolution = 300**; several sheets loaded | Scan | **Multi-page output** — one page per fed sheet, in order | ☐ | File transfer appends a page index. This is the FILE-path multipage that C10 relies on as a baseline |
| **C13. Empty ADF — simplex** | Scan Mode = Document Feeder; **no paper**; Duplex off | Press **Scan** | Image Capture promptly shows the **native alert "Scanner reported an error / Document feeder is empty."** (`kICAErrStrDFEmptyErr`) (#84, #85) | ☐ | Instant via the `ESC D` ack (`0xc2`), not a timeout. Regression check that generalizing the notifier (`PostScannerError`, PR C) preserved this |
| **C14. Empty ADF — 2-sided** | Scan Mode = Document Feeder; **no paper**; Duplex on | Press **Scan** | Same native "Document feeder is empty." alert, promptly (#84, #85) | ☐ | Must fire for duplex as well as simplex |
| **C15. Cancel mid-ADF-scan** | Scan Mode = Document Feeder; several sheets loaded; a scan running | Press **Cancel** while pages are still feeding | The scan aborts cleanly; the device **feeds the remaining sheets out until the feeder is empty**, then stops; Image Capture returns to ready **without hanging**, and a new scan starts normally | ☐ | Guards mid-feed abort + recovery; no wedged session |
| **C16. Scan failure shows a readable dialog** | Scan Mode = Document Feeder; induce a failure — feed a sheet, then **physically jam the ADF mid-feed** | Scan and induce a jam | Image Capture shows a **readable** message (e.g. "An error occurred during scanning.", or a specific alert) — **not** the bland generic failure; **record what a jam shows** | ☐ | **Readable errors (PR C):** the module posts a `kICANotificationTypeDeviceStatusError` whose subtype is an `Error.loctable` key. Empty-feeder (C13/C14) already renders "Document feeder is empty.". **Jam is DISCOVERY:** its signature is uncaptured, so a jam is not yet mapped to `kICAErrStrDFPaperErr` ("Document feeder has a paper jam or paper feed error.") — note the observed message; mapping the jam key is a follow-up pending a captured jam signature |

### Known limitations

This section records confirmed defects that have **no module-side fix**, so a
tester does not re-file them or read them as regressions of the rows above.

**KL1. "Combine into single document" → TIFF duplicates the first page for application destinations — Preview, Photos, Mail (macOS 26).**

**Symptom.** With **Combine into single document** selected, the format set to
**TIFF**, and the destination set to **an application** — Preview, Photos, and
Mail all reproduce it — an N-page ADF scan
produces an **N+1**-page TIFF: the first page is emitted twice, so the composition
is `[page0, page0, page1, …]` (a 2-page scan yields `[page0, page0, page1]`).
`tiffutil -info` reports **N+1** image file directories (IFDs), and the file is
exactly **(N+1)×** one page's byte size — a verified 2-page scan reports **3 IFDs,
75.7 MB**. The **same** Combine → TIFF job saved to a **folder** destination is
correct: a proper N-page TIFF, **2 IFDs, 50.5 MB** for that same scan. Preview's
own **File ▸ Import from Scanner** path also does not duplicate, so the defect is
specific to Image Capture's open-in-app destination on macOS 26.

**Root cause.** The defect is host-side, in Image Capture's open-in-app
handoff / combine-staging on macOS 26 — **not** the folder-write path, and **not**
the module. The module delivers exactly N pages (`pages=N`,
one `PostFilePage` / `ScannerPageDone` per page) with the same filenames
(`stem.tif`, `stem 1.tif`) and the same `ScannerPageDone(path)` sequence as the
vendor driver, which combines correctly on macOS 15.7 (Intel) but ships no
Apple-Silicon build to test on 26. Four module-side fixes were tried; none removed
the duplicate:

1. **Not the progress bands.** Removing every image-info `ScanProgressStatus` band
   (dataless progress) still duplicated — the bands were never the cause.
2. **Not a timing race.** Spacing page delivery ~5 s apart to match the vendor
   driver's per-sheet cadence still duplicated — not a burst/timing race.
3. **Page 0 cannot be renamed.** In separate-files mode page 0 must be the bare
   `stem.tif`; indexing it from 1 regresses that mode. The module cannot tell
   combine from separate mode — **Combine** is an Image Capture UI setting, absent
   from the ImageCaptureCore client API.
4. **No page signal to add.** The ICA notification API exposes no page-index,
   page-number, or page-count key; a `ScannerPageDone` carries only
   `kICANotificationScannerDocumentNameKey` (the file path), confirmed against the
   exported symbols in `ICADevices.tbd` and `ImageCapture.tbd`.

The destination-dependence is additional confirmation the defect is host-side. The
module only ever receives a folder path — the real destination folder, or Image
Capture's temporary staging folder for the app handoff — and writes byte-identical
files either way. Identical module output that yields a correct folder TIFF but a
duplicated open-in-app TIFF places the defect entirely in the host's open-in-app
staging.

**Workarounds.**

- Scan **Combine → TIFF to a folder** destination — the folder-write path produces
  a correct native multi-page TIFF. Best option: keeps TIFF and a single file.
- Combine to **PDF** instead of TIFF (correct output).
- Save **separate files** — one correct TIFF per page.
- Stitch the separate files into one multi-page TIFF yourself:
  `tiffutil -cat page*.tiff -out multi.tiff`.

Filed to Apple Feedback Assistant: `FB24717493`

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
