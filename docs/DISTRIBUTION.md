<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Signing and distribution

This project builds two macOS `.app` bundles:

- **`build/BrScan Mac ICA.app`** — the Plan 2 Image Capture device module,
  installed into `/Library/Image Capture/Devices/` (see
  [RUNBOOK-plan-2-ica.md](RUNBOOK-plan-2-ica.md) and
  [ica-module/install.sh](../ica-module/install.sh)).
- **`build/gui/Brscan Config.app`** — the Plan 1e standalone configuration app,
  installed into `/Applications` (see [../gui/README.md](../gui/README.md) and
  [gui/install.sh](../gui/install.sh)).

Both are signed the same way, by the same CMake helper, and this doc is the one
place that describes it. It covers (1) the ad-hoc signature that ships today and
why it is enough for a local/dev install, and (2) the future Developer-ID +
notarization path for redistributing either bundle to other machines.

## Ad-hoc signing (what ships today)

`cmake --build build` builds and **ad-hoc signs** both bundles. The shared
CMake helper `brscan_adhoc_sign` (in [cmake/BrscanCodesign.cmake](../cmake/BrscanCodesign.cmake))
runs, as a POST_BUILD step on each bundle target:

```bash
codesign --force --sign - --timestamp=none "<bundle>"
codesign --verify --deep --strict --verbose=2 "<bundle>"
```

An ad-hoc signature (`--sign -`) carries no certificate and no team identity. It
costs nothing, needs no credentials, and makes every local iteration a plain
rebuild. Verify either bundle yourself:

```bash
codesign --verify --deep --strict --verbose=2 "build/gui/Brscan Config.app"
codesign -dvvv "build/gui/Brscan Config.app"    # Signature=adhoc, flags=0x2(adhoc)
```

**Why ad-hoc is sufficient for local/dev install:**

- **The ICA module** loads from `/Library/Image Capture/Devices/`. Running our
  ad-hoc bundle's binary directly launches it, loads `ICADevices.framework`, and
  enters `ICD_ScannerMain` — ad-hoc code executes on this macOS. Whether icdd's
  launch context additionally enforces library validation on the child is the
  one open question the live runbook settles; it is a *load* question, not a
  signing-format one.
- **The config app** is a normal user app. Ad-hoc is enough to run it locally;
  Gatekeeper will quarantine a copy downloaded from the internet, but a locally
  built one you install yourself runs after the first-launch right-click ▸ Open
  (or `xattr -dr com.apple.quarantine`, see `gui/install.sh`).

Gatekeeper rejects an ad-hoc bundle for *distribution* (`spctl -a -t exec` →
"rejected"). That is expected and is a distribution check, not a local-run or
module-load blocker.

## Developer-ID + notarization (future, NOT wired)

To ship either bundle to *another* Mac without the Gatekeeper friction above,
replace the ad-hoc signature with a Developer-ID Application signature and
notarize it. This path is **deliberately not part of the build** — it needs a
paid Apple Developer account and credentials, which this project does not wire
into CMake. There is **no credentialed build step**; the steps below are manual,
run by a human who holds the certificate. Ad-hoc signing stays the default.

Using `build/gui/Brscan Config.app` as the example (identical for the ICA
bundle — swap the path):

```bash
# 1. Re-sign with a Developer ID Application cert (hardened runtime).
codesign --force --options runtime --timestamp \
  --sign "Developer ID Application: <Name> (<TEAMID>)" \
  "build/gui/Brscan Config.app"

# 2. Submit to Apple's notary service and wait for the result.
ditto -c -k --keepParent "build/gui/Brscan Config.app" "Brscan Config.zip"
xcrun notarytool submit "Brscan Config.zip" \
  --apple-id "<apple-id>" --team-id "<TEAMID>" --password "<app-specific-pw>" \
  --wait

# 3. Staple the notarization ticket into the bundle.
xcrun stapler staple "build/gui/Brscan Config.app"
```

None of the above is added as a CMake step — it would require credentials in the
build. For the ICA module specifically, Apple's own modules in the system
directory are Team-signed and carry the `library-validation` flag, so a
redistributed third-party module may need this path even to *load* on another
machine; only a live install on the target OS confirms it (see the runbook).
