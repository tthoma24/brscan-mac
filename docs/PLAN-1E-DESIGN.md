# Plan 1e — graphical configuration UI (design)

This document is the design and task plan for Plan 1e: a native macOS
configuration UI for the "set from computer" side of `brscan-scand`. It is a
plan only — no application code is written yet.

The UI is a front end over the daemon's config file,
`~/.config/brscan-scand.conf`. The daemon stays the single source of truth for
what lands on disk (see `daemon/config.h`); the UI reads that file, edits the
keys it owns, and writes it back while preserving everything else. Plan 1e adds
no new config keys and no daemon behavior — it makes the existing Plan 1d schema
editable without a text editor.

## Scope

In scope:

- A standalone macOS app that edits the global and per-route settings the daemon
  already parses (`daemon/config.cpp`).
- Round-tripping the config file so hand-written comments, key order, and
  unknown keys survive a save.
- Validation against the daemon's own option sets so the UI can't offer a value
  the daemon rejects.

Out of scope (see "Out of scope and deferred" below):

- Any scanning UI (interactive scan is Plan 2, through Image Capture).
- Embedding in **Printers & Scanners** / **Image Capture** (depends on Plan 2's
  ICA device module, which doesn't exist yet).
- Editing panel toggles the daemon has no config key for yet (skip-blank,
  remove-background, high-speed, OCR sub-format) — these are Touch-Panel-ON wire
  fields only today.

## A correction to carry into implementation: the daemon does not re-read config per press

The Plan 1e brief and the master plan both assume "edits apply on the next scan
without a restart." **That is not how the daemon works today.** The daemon calls
`LoadConfig()` exactly once, at startup, before entering its listen loop
(`tools/brscan-scand.cpp`, `main()` — `const Config cfg = LoadConfig(...)` sits
above the `while` loop and is never reloaded). Nothing in the loop re-reads the
file, and there is no reload signal handler. `docs/BUTTON.md`'s own setup section
confirms the operational consequence: "After editing the plist or the config,
unload and reload it (bootout, then bootstrap) for the change to take effect."

So a save from the UI has no effect until the daemon reloads. The UI must make
that explicit rather than implying a save is live.

**Confirmed: apply-on-save works by `SIGHUP` reload, not a restart.** After a
save, the GUI signals the running daemon (`SIGHUP`, for example via
`launchctl kill SIGHUP gui/<uid>/com.brscan.scand`), and the daemon re-reads
`~/.config/brscan-scand.conf` at a safe point in its loop and keeps running —
no process restart, no gap in listening for button presses beyond the reload
itself. This depends on a small daemon change that is a **prerequisite** to
the GUI's "Save & apply" feature:

- **Task 1e.0 (daemon, prerequisite): `brscan-scand` installs a `SIGHUP`
  handler that re-reads `~/.config/brscan-scand.conf` at a safe point in the
  loop** (for example, between button presses, never mid-scan) and replaces
  the in-memory `Config` in place. This is a daemon-side change, tracked and
  built separately from the GUI (see the task breakdown below); the GUI code
  in this plan only needs to know that sending `SIGHUP` is how a save takes
  effect.

Reword any "applies without restart" or "applies on the next scan" wording
throughout this plan (and the master plan) to: **the GUI signals the daemon,
which reloads on `SIGHUP`** — not "restart," and not "takes effect
immediately with no daemon involvement."

## Recommended architecture

Each decision below lists the options considered, the recommendation, and why.
The choices that most deserve a human sign-off are collected again at the end.

### 1. App shape

Options:

- SwiftUI standalone app, preferences-style window only.
- SwiftUI standalone app, menu-bar extra (`MenuBarExtra`) only.
- AppKit standalone app.
- Both: a `MenuBarExtra` status item that opens a SwiftUI preferences-style
  window.

**Confirmed: both.** A `MenuBarExtra` status item lives in the menu bar
whenever the daemon's LaunchAgent is installed, showing at a glance whether
the daemon is running; choosing it (or a "Preferences…" item under it) opens
a normal SwiftUI window with a tabbed **General** / **File** / **Image** /
**OCR** / **Email** layout.

Why:

- SwiftUI over AppKit: the UI is a small, static set of forms (popups,
  steppers, a folder picker). SwiftUI's `Form`/`Picker` express exactly this
  with far less code, and it is the natural path if we later move these panes
  into a System Settings-style surface.
- Menu-bar status item plus window, not either alone: the daemon is a
  background LaunchAgent with no Dock presence of its own, so a
  `MenuBarExtra` is the at-a-glance "is it running" surface an earlier draft
  of this document called a nice-to-have. Pairing it with the preferences
  window — rather than picking one — gives both the glanceable status and
  the discoverable, full editing surface a "Preferences" app needs: the app
  opens a normal window when the user wants to edit settings, and stays out
  of the way as a menu-bar icon otherwise.
- Minimum macOS: **macOS 13 Ventura** (confirmed). It gives modern SwiftUI
  `Form` styling and `MenuBarExtra` without reaching for the newest APIs, and
  it matches the System Settings visual era the config should eventually
  blend into.

### 2. Build integration

Options:

- Drive a Swift target from the existing CMake build.
- A separate Swift Package (SwiftPM) under `gui/`, built independently.
- A separate Xcode project under `gui/`, built independently.

**Recommendation: keep the GUI out of CMake. Put the reusable logic in a SwiftPM
package under `gui/` and wrap it in a thin Xcode app target.** Concretely:

- `gui/BrscanConfigCore/` — a SwiftPM library package holding the config
  round-trip reader/writer, the option-set model, and the typed config model.
  It has no UI and is fully testable with `swift test`.
- `gui/BrscanConfigApp/` — an Xcode app project that depends on
  `BrscanConfigCore` and contains only SwiftUI views.

Why:

- The GUI shares **no compiled code** with `libbrscan` or the daemon — it only
  reads and writes a text file. Wiring an `.app` bundle, code signing, and an
  `Info.plist` into this C++/CMake project buys nothing and complicates both
  builds. Keeping them decoupled is the lowest-friction option.
- The SwiftPM library gives us a runnable, testable core (`swift test`) before
  any UI exists — which is the ordering the brief asks for.
- An Xcode app target (rather than a bare SwiftPM executable) is what produces a
  real, signable `.app` with an `Info.plist`, and it is the seam a future System
  Settings extension would grow from.

Packaging: **the project ships no DMG or installer today** (no CPack,
`productbuild`, `pkgbuild`, or `*.dmg` tooling exists in the tree — the README
and `docs/BUTTON.md` build binaries from source and copy them into place). So the
GUI ships as its own `.app`, built separately; it does not slot into an existing
package. A developer builds and runs it with:

- `cd gui/BrscanConfigCore && swift test` — the core logic and its tests.
- `xcodebuild -project gui/BrscanConfigApp/BrscanConfigApp.xcodeproj` (or open in
  Xcode and Run) — the app.

### 3. Config round-tripping

Options:

- (a) A Swift round-tripping reader/writer for the `KEY=VALUE` format, in the
  app. No IPC.
- (b) A helper the daemon exposes that the app calls.

**Recommendation: (a), a Swift round-tripping reader/writer.** There is no reason
to add IPC to edit a local text file the daemon reads at startup.

Round-trip rules (these mirror the daemon's tolerant parser in
`daemon/config.cpp` so the two never disagree about what a line means):

- Parse the file into an **ordered list of lines**, each classified as blank,
  comment (`#` first non-blank char), a recognized `KEY=VALUE`, or an
  unrecognized/unparsable line. Split a `KEY=VALUE` on the **first** `=`, and
  trim whitespace around key and value — exactly as `ParseConfig()` does.
- The UI **owns a fixed set of keys** (listed in "Config fields" below). On
  write:
  - A changed owned key that already has a line is updated **in place**, keeping
    its position and surrounding comments.
  - A newly set owned key with no existing line is appended (to the end of its
    logical section if one is identifiable, otherwise to the end of the file).
  - An owned key the user clears back to "unset/default" has its line removed
    (see the open decision on clear-vs-empty).
- **Every line the UI does not own is left byte-for-byte unchanged** — comments,
  blank lines, key order, unknown keys, and the file's trailing-newline style.
- Preserve a commented-out example line as a comment; do not treat it as an
  active key. If the user later sets that key, add a new active line rather than
  uncommenting the example (keeps the example intact and the diff obvious).
- Display `save_dir` with `~` handling that matches `ExpandHome()` (leading `~`
  or `~/…` only), and write back what the user entered rather than an expanded
  absolute path, so the file stays portable.
- Write atomically (write to a temp file in the same directory, then rename) so a
  crash mid-save can't truncate the config.

### 4. Shared option-set vocabulary (anti-drift)

The valid tokens live in C++ today: `ParseModeString`, `ParseSourceString`,
`ParseFormatString`, `ParseTiffCompressionString`, `ParseSeparationString`
(`daemon/config.cpp`) and the paper table (`daemon/paper_size.*`). The UI must
not offer a value the daemon would silently drop.

Options:

1. A single shared data file both sides consume (`config/option-sets.json`).
2. Hand-mirror the lists in Swift, guarded by a test.
3. Generate a Swift constants file from the C++ source (or from the shared
   data file) at build time.

**Proposal to confirm — not a locked choice: combine (1) and (3), guarded by
a C++ test.** Scoped concretely:

- Add `config/option-sets.json` (our own clean-room data — just token strings)
  listing the enumerable sets: `mode`, `source`, `format`, `tiff_compression`,
  and `paper`. (The non-enumerable fields — `dpi`, a positive integer, and
  `separation`, `combine`/`every:N` — are validated by rule, not by a token
  list; see below.) This file is the one hand-authored source of truth for the
  vocabulary.
- A build step generates a Swift file (for example
  `Sources/BrscanConfigCore/OptionSets.generated.swift`) from
  `option-sets.json` — a SwiftPM plugin or a small script run before
  `swift build`/`swift test` — so the Swift side is never hand-mirrored and
  can't drift from the JSON by editor error.
- Add a C++ guard test (`tests/option_sets_test.cpp`) that reads the same JSON
  and asserts, for every token in each set, that the daemon's matching parser
  **accepts** it, and that a sentinel bogus token is **rejected**. If someone
  adds a token to the JSON (and thus to the generated Swift and the UI)
  without teaching the daemon, or removes daemon support for a token the JSON
  still lists, CI fails.

Why this combination over the alternatives: hand-mirroring (2) alone keeps two
independent lists that drift; a shared JSON alone (1) still leaves room for a
typo in hand-written Swift bindings to the JSON's keys. Layering codegen (3) on
top of the shared JSON (1) removes that last hand-written seam — the Swift
symbols are generated, not typed — while keeping the authoring surface to one
small JSON file and one guard test in the existing CTest suite. This is scoped
as a proposal, not a locked decision: confirm the combination (and where the
codegen step lives — a SwiftPM plugin versus a plain script invoked by CI)
before task 1e.3 depends on it.

### 5. Validation and UX

Layout: a tabbed window. **General** holds the machine-wide keys; each of
**File / Image / OCR / Email** holds that route's `<dest>.*` keys.

- **Touch-Panel override banner.** Each route tab shows a persistent note at the
  top: *"These apply only when the printer's Touch Panel is off for this
  destination. When Touch Panel is on, the printer's own panel settings win."*
  This restates `docs/BUTTON.md`'s "Touch-Panel precedence" in plain terms.
- **Gating instead of error dialogs.** Prevent invalid combinations by disabling
  controls rather than validating after the fact:
  - `tiff_compression` is enabled only when `format = tiff`.
  - `separation` is enabled only when `format` is a container format
    (`pdf`/`tiff`); for `jpeg`/`png`/`native` it is shown disabled with a note
    that per-page formats always write one file per page.
  - `g3`/`g4` show a soft inline note that they apply only to black & white
    pages and fall back to LZW otherwise (matching `WriteConfiguredOutput`); this
    is informational, not blocking, because the daemon handles the fallback.
- **dpi.** Offer a popup of common values (100, 150, 200, 300, 400, 600) plus a
  custom field. The daemon accepts any positive integer (`ParsePositiveInt`), so
  the custom field's only rule is "positive integer."
- **source / two-sided.** One control mapping to `<dest>.source`: Flatbed →
  `flatbed`, ADF (one-sided) → `adf`, ADF (two-sided) → `adf-duplex`. There is no
  separate duplex-edge config key, so the UI exposes none.
- **paper.** A popup of the nine tokens plus an "Auto / none" choice that writes
  an empty/absent `<dest>.paper` (the daemon's "no explicit paper" state).
- **OCR specifics.** The OCR tab notes that OCR always produces a **searchable
  PDF**; the `ocr.format` popup is still offered, with the explanation that
  setting `tiff`/`jpeg`/`png` keeps the format but drops the text layer (matches
  `docs/BUTTON.md`).
- **Locating and creating the file.** On launch, read
  `~/.config/brscan-scand.conf`. If it is missing, offer to create it from the
  committed example (`config/brscan-scand.conf.example`) so the user starts from
  the documented, commented template rather than a bare file. Create `~/.config`
  on save if needed. Leave `save_dir` **creation** to the daemon (it already
  `create_directories`), but offer a folder picker for the value.
- **Required field.** `printer_host` has no safe default and the daemon refuses
  to start without it. The General tab flags an empty `printer_host` prominently
  (with the `dns-sd -B _scanner._tcp` hint from the example config) but still lets
  the user save — a partial config is valid on disk; it just won't start the
  daemon yet.
- **Daemon state.** Detect whether the LaunchAgent is installed and running
  (inspect `~/Library/LaunchAgents/com.brscan.scand.plist` /
  `/Library/LaunchAgents/…` and `launchctl print gui/<uid>/com.brscan.scand`) and
  show one of: *not installed* (link to the `docs/BUTTON.md` install steps),
  *installed but stopped*, or *running*. Because a save takes effect only when
  the daemon reloads (see the correction above), the save action is **"Save &
  apply"**, which writes the file and then sends the running daemon `SIGHUP`
  (task 1e.0) so it re-reads the config without restarting. If the daemon
  isn't installed or isn't running, the app still saves the file and says so —
  there is nothing to signal.

## Config fields the UI edits

All keys and value sets below are taken from `daemon/config.h`,
`daemon/config.cpp`, `daemon/output_writer.h`, and `daemon/paper_size.h`.
`<dest>` is one of `file`, `image`, `ocr`, `email`.

### General (machine-wide keys)

| UI field | Config key | Valid values | Notes |
|---|---|---|---|
| Printer host | `printer_host` | any string (hostname or IP) | **Required**; no default. Daemon won't start if empty. |
| Display name | `display_name` | any string | Defaults to this Mac's host name. A `"` or `;` is rejected at registration; warn if present. |
| Save folder | `save_dir` | a path; leading `~` expanded | Default `~/Scans`. Write the `~` form back, don't expand on save. |
| Image opens with (Image tab) | `image_app` | any string (app name) | Empty → open in the file's default app. |
| Email to (Email tab) | `email_to` | any string (address) | Empty → leave the To: field blank. Never auto-sends. |

### Per route: File / Image / OCR / Email

Every route takes the same `<dest>.*` keys. Route defaults come from
`brscan::Params` (color, 300 dpi, flatbed) and `OutputSettings` (native, LZW,
combine).

| UI field | Config key | Valid values | Gating / notes |
|---|---|---|---|
| Color mode | `<dest>.mode` | `color` \| `gray` \| `bw` \| `errdiff` \| `truegray` | Default `color`. |
| Resolution | `<dest>.dpi` | positive integer (sets x and y) | Default `300`. Popup of common values + custom. |
| Source / sides | `<dest>.source` | `flatbed` \| `adf` \| `adf-duplex` | Default `flatbed`. `adf-duplex` is "ADF, two-sided." |
| File type | `<dest>.format` | `pdf` \| `tiff` \| `jpeg` \| `png` \| `native` | Default `native`. OCR: always a searchable PDF regardless. |
| TIFF compression | `<dest>.tiff_compression` | `lzw` \| `g3` \| `g4` | Default `lzw`. Enabled only when `format = tiff`. `g3`/`g4` are bilevel-only (LZW fallback otherwise). |
| Document separation | `<dest>.separation` | `combine` \| `every:N` (N ≥ 1) | Default `combine`. Enabled only for `pdf`/`tiff`. `every:N` → a stepper for N. |
| Paper size | `<dest>.paper` | `LETTER` \| `LEGAL` \| `A4` \| `LEDGER` \| `A3` \| `A5` \| `EXECUTIVE` \| `PHOTO` \| `BCARD` \| *(empty = Auto/none)* | Default empty. Case-sensitive tokens (`daemon/paper_size.h`). |

Fields deliberately **not** editable by the UI, because the config schema has no
key for them today (they exist only as Touch-Panel-ON wire fields parsed in
`daemon/button_config.h`): skip-blank (`W=`), remove-background (`G=`/`L=`),
high-speed (`X=`), and the OCR sub-formats (`T=TXT/HTML/RTF`). Adding config keys
for these is a Plan 1d/daemon change, not a Plan 1e UI change; see the open
decision.

## Task breakdown

Small, independently reviewable tasks, one PR each, ordered so a runnable,
tested core lands before any UI. Each task lists its one-line scope and its test
surface.

| # | Scope | Test surface |
|---|---|---|
| 1e.1 | Add `config/option-sets.json` (mode/source/format/tiff_compression/paper tokens), a build step that generates a Swift constants file from it, and a C++ guard test asserting the daemon accepts every token and rejects a sentinel. | `tests/option_sets_test.cpp` in CTest (`ctest`); a build/regen check that the generated Swift file matches the JSON. |
| 1e.2 | SwiftPM package `gui/BrscanConfigCore` with the config round-trip reader/writer (ordered-line model; edit owned keys; byte-stable for the rest; atomic write). **First runnable deliverable.** | `swift test`: preserve comments/order/unknown keys; change in place; add key; clear key; identity round-trip. |
| 1e.3 | Option-set model in the core: consume the generated Swift constants from `option-sets.json` (task 1e.1), expose typed enums + rule validators (`dpi` positive int, `separation` combine/every:N, format-gating rules). | `swift test`: each token maps; bad tokens rejected; gating rules. |
| 1e.4 | Typed config model in the core: map the round-trip line model ↔ a `DaemonConfig` of General + four route structs, with defaults. | `swift test`: parse → model → serialize; default fill-in; unknown-key passthrough. |
| 1e.5 | App scaffold: Xcode app target `gui/BrscanConfigApp` with the tabbed window shell (General + File/Image/OCR/Email), static, no logic. First UI PR. | Builds via `xcodebuild`; launches; smoke/UI test that all five tabs render. |
| 1e.6 | Wire the General tab to the core (printer_host required-flag, display_name, save_dir folder picker, image_app, email_to). | UI test / view-model unit test for binding + required-field flag. |
| 1e.7 | Reusable route editor wired for File/Image/Email (mode/dpi/source/format/tiff_compression/separation/paper), with gating and the Touch-Panel banner. | View-model unit tests for gating (tiff→compression, container→separation) + binding. |
| 1e.8 | OCR route specialization (searchable-PDF note; format handling and text-layer caveat). | View-model unit test for the OCR note/format behavior. |
| 1e.9 | Load/save/first-run flow: locate config, offer create-from-example when missing, atomic save, unsaved-changes prompt. | View-model tests for missing-file, create-from-example, dirty-state. |
| 1e.10 | Daemon status + "Save & apply": detect installed/running via launchctl; send `SIGHUP` (task 1e.0) to reload instead of restarting; not-installed/not-running guidance. | View-model test with an injected launchctl/signal runner (install/stopped/running states; `SIGHUP` sent only when running). |
| 1e.11 | Build/run docs: how to build the app (`swift test`, `xcodebuild`), where it installs; a pointer from `README.md`/`docs/BUTTON.md`. | Docs only; prose review. |

Prerequisite, daemon-side, **not** a UI PR — confirmed, not optional:

| # | Scope | Test surface |
|---|---|---|
| 1e.0 | Daemon `SIGHUP` handler: `brscan-scand` re-reads `~/.config/brscan-scand.conf` at a safe point in its loop (never mid-scan) and replaces the in-memory `Config`, so a GUI save can take effect without a restart. Blocks task 1e.10 ("Save & apply"), which sends the signal. | A daemon-side test (for example `tests/config_reload_test.cpp`) asserting a second `SIGHUP` after a config edit changes the daemon's effective `Config` without a process restart. |

## Out of scope and deferred

- **No scanning UI.** Interactive scanning is Plan 2, via Image Capture.
- **No Printers & Scanners / Image Capture embedding.** The preferred surface
  depends on Plan 2's ICA device module (`ICADevices.framework`), which does not
  exist yet. The standalone app is the baseline until then; its logic lives in
  `BrscanConfigCore` precisely so the panes can later be reused inside a System
  Settings surface.
- **No editing of panel-only toggles.** Skip-blank, remove-background,
  high-speed, and OCR sub-formats have no config key; editing them needs a daemon
  schema change first.
- **No Bonjour device browser.** `printer_host` is entered manually with the
  `dns-sd` hint; discovery/browse UI belongs with Plan 2.

## Clean-room note

This design uses no Brother source or assets. The only Brother-derived strings
are the paper/format tokens already decoded clean-room into this project's own
headers (`daemon/paper_size.h`, `daemon/output_writer.h`, `daemon/button_config.h`).
Where a device placeholder is needed, use the synthetic `BRW00AABBCCDDEE`; no
real device identity (Bonjour name, MAC, or IP) appears here.

## Confirmed decisions

These were open questions in an earlier draft of this document. The human has
since confirmed all three:

1. **How a save takes effect: `SIGHUP` reload, not a restart.** The GUI
   signals the daemon (`SIGHUP`); the daemon re-reads its config in place.
   This adds daemon task 1e.0 as a prerequisite — see "A correction to carry
   into implementation" above. The master plan's "applies on the next scan
   without a restart" should be reworded to "the GUI signals the daemon,
   which reloads on SIGHUP."
2. **App shape: both.** A `MenuBarExtra` status item plus a SwiftUI
   preferences-style window with per-route tabs — see "1. App shape" above.
3. **Minimum macOS version: macOS 13 Ventura.**

## Open decisions for the human

1. **Anti-drift mechanism.** Proposal to confirm (not locked): a
   hand-authored `config/option-sets.json` (option 1) as the single source of
   truth, plus a build step that generates a Swift constants file from it
   (option 3), guarded by a C++ CTest that fails if the JSON drifts from the
   real daemon enums. See "4. Shared option-set vocabulary (anti-drift)" above
   for the full comparison. Confirm this combination, or pick a different one
   (hand-mirroring alone, codegen alone, or the JSON alone without codegen).
2. **Build/packaging seam.** Recommended: a SwiftPM `BrscanConfigCore` + an Xcode
   app under `gui/`, kept out of CMake, with no DMG (none exists today). Confirm
   this split, and whether the app should ever be packaged/signed for
   distribution or stay a build-from-source developer tool.
3. **Clear-vs-empty on write.** When the user resets an optional key to its
   default, should the UI **remove** the line or write an explicit empty value
   (for example `file.paper=`)? Recommended: remove the line (smaller, cleaner
   diffs); confirm.
4. **Should skip-blank / remove-background / high-speed / OCR sub-format become
   config keys** so the UI can edit them? That is a daemon schema addition
   (Plan 1d territory), out of Plan 1e's baseline. Confirm they stay deferred.
