# Brscan Config (Plan 1e)

A standalone macOS app for editing `brscan-scand`'s "set from computer"
config, `~/.config/brscan-scand.conf` — the per-destination defaults the
daemon uses when the printer's Touch Panel is off for that destination. See
[docs/BUTTON.md](../docs/BUTTON.md) for what the daemon does with those
settings, and its "Touch-Panel precedence" section for when they apply: **when
the printer's Touch Panel is on for a destination, the printer's own panel
settings win instead, and this app's settings are ignored for that scan.**

The app is a menu-bar item (shows whether the daemon is running, and opens the
config window) plus a tabbed window with five sections: **General** (the
machine-wide settings — printer host, display name, save folder) and one tab
each for the **File**, **Image**, **OCR**, and **Email** destinations (mode,
resolution, source, file format, and so on for that destination). See
[docs/PLAN-1E-DESIGN.md](../docs/PLAN-1E-DESIGN.md) for the full field list and
the design rationale.

## Layout

- `BrscanConfigCore/` — a Swift package (library) with the config round-trip
  reader/writer, the option-set model, and the typed `DaemonConfig`. No UI; no
  dependency on `BrscanConfigApp`. Build and test it on its own:

  ```bash
  cd gui/BrscanConfigCore
  swift test
  ```

- `BrscanConfigApp/` — a Swift package (executable) with the SwiftUI app and
  its view models. Depends on `BrscanConfigCore` via a local path dependency.

  ```bash
  cd gui/BrscanConfigApp
  swift build   # produces .build/debug/BrscanConfigApp
  swift test
  swift run     # builds (if needed) and launches the app
  ```

Both packages need macOS 13 (Ventura) or later and Swift 5.9 or later (the
Xcode or Command Line Tools that ship with recent macOS releases include
both).

For quick iteration you can build and test either package on its own with
`swift build`/`swift test`. The packaged `.app` (below) is built through CMake.

## Packaging

`cmake --build build` (from the repo root) builds the app in release, assembles
it into a real `.app` bundle, and ad-hoc signs it:

```bash
cmake -S . -B build
cmake --build build          # builds build/gui/Brscan Config.app (among others)
```

The bundle is `build/gui/Brscan Config.app`: `CFBundleExecutable =
BrscanConfigApp`, id `me.tthoma24.brscan.config`, and `LSUIElement = true` so it
runs as a menu-bar-only agent (no Dock icon) while still showing its config
window. SwiftPM's build output stays under `build/gui/swiftpm`, never in the
source tree.

Install it into `/Applications` (or remove it) with the helper script:

```bash
gui/install.sh install
gui/install.sh uninstall
```

## Signing and Gatekeeper

The bundle is **ad-hoc signed** by CMake (`codesign --sign -`), which is enough
to run it locally. Because it is not Developer-ID signed or notarized, Gatekeeper
will not open it on a first double-click: right-click the app in `/Applications`
and choose **Open** (then **Open** again in the dialog), or clear the quarantine
flag with `xattr -dr com.apple.quarantine "/Applications/Brscan Config.app"`.
`gui/install.sh install` prints this reminder.

The future Developer-ID signing + notarization path (for redistributing the app
to other machines) is documented in
[../docs/DISTRIBUTION.md](../docs/DISTRIBUTION.md); it is deliberately not wired
into the build.

## Not yet editable here

- **Panel-only toggles aren't editable.** Skip-blank, ADF high-speed, and the
  OCR Text/HTML/RTF sub-formats have no `brscan-scand.conf` key yet — they're
  parsed only as Touch-Panel-ON wire fields (see `daemon/button_config.h` and
  docs/BUTTON.md's "Not yet implemented"). Adding config keys for them is a
  daemon change.
- **No Printers & Scanners / Image Capture integration.** The system scanner UI
  (Image Capture, Printers & Scanners) exposes only a module's scan-time
  capabilities — there is no host hook for a persistent settings pane — so this
  standalone app stays the configuration surface.

## How changes apply: Save & apply

The window's **Save & apply** action writes `~/.config/brscan-scand.conf`
atomically (a temp file, then a rename), preserving every comment, blank line,
key order, and any key the app doesn't manage — then, if `brscan-scand` is
currently running as a LaunchAgent, sends it `SIGHUP` so it re-reads the config
in place and keeps running (no restart, no gap in listening for button
presses beyond the reload itself). If the daemon isn't running, the file still
saves; the change takes effect the next time the daemon starts. See
`gui/BrscanConfigApp/Sources/BrscanConfigApp/DaemonViewModel.swift` and
`DaemonControl.swift` for the `launchctl print`/`launchctl kill -s HUP` calls
behind this, and docs/BUTTON.md's "Install the LaunchAgent" section for how
the daemon gets installed in the first place.

## First run

If `~/.config/brscan-scand.conf` doesn't exist yet, the app offers to create a
starter config — the same defaults the daemon itself uses (color, 300 dpi,
flatbed, and so on; see `BrscanConfigCore`'s `DaemonConfig.default`), written
out with a short header comment — so there's something to load and edit
instead of a bare or missing file. It never overwrites an existing config.

## Option-set drift guard

The valid tokens for fields like `mode`, `source`, `format`,
`tiff_compression`, and `paper` are decoded from the daemon's C++ parsers
(`daemon/config.cpp`, `daemon/paper_size.h`). To keep the GUI from ever
offering a value the daemon would reject, those tokens live in one file,
[config/option-sets.json](../config/option-sets.json), which both sides
consume:

- `scripts/gen-option-sets.py` generates
  `BrscanConfigCore/Sources/BrscanConfigCore/GeneratedOptionSets.swift` from
  the JSON. Run it after editing the JSON:

  ```bash
  python3 scripts/gen-option-sets.py           # regenerate the committed file
  python3 scripts/gen-option-sets.py --check   # fail with a diff if it's stale
  ```

- `tests/option_sets_test.cpp` is a C++ test (part of the `brscan_tests`
  binary, run via `ctest --test-dir build`) that reads the same JSON and
  asserts the daemon's real parsers accept every listed token and reject a
  sentinel bogus one.
- A separate CTest entry, `OptionSetsSwiftConstantsUpToDate`, runs
  `gen-option-sets.py --check` so CI fails if someone edits the JSON without
  regenerating and committing the Swift file.

If you add, remove, or rename a token: edit `config/option-sets.json`, run
`scripts/gen-option-sets.py` and commit the regenerated Swift file, then run
both test suites — `swift test` (from `gui/BrscanConfigCore`) and
`ctest --test-dir build` (from the repo root, after `cmake --build build`) —
to confirm the daemon, the JSON, and the Swift constants all agree.
