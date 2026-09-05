# Live validation runbook — Plan 1d (scan-button flow)

This is the hardware acceptance test for Plan 1d: pressing a scan button on the
printer and confirming the daemon replicates the button flow and honors the
Touch-Panel settings. It assumes `brscan-scand` is already built, configured,
and registered — see [BUTTON.md](BUTTON.md) "Setup" for that. Here we only run
the daemon in the foreground and work a press matrix.

The companion checklist (a table you can tick off at the printer) is the
Plan 1d live-validation artifact; this file is the reference behind it.

## 1. Run the daemon in the foreground

```bash
build/brscan-scand --config ~/.config/brscan-scand.conf
```

Leave it running in a terminal so you can watch the log. On start it prints its
settings and registers each FUNC; within a few seconds this Mac appears in the
printer's Scan-to menu.

## 2. What a good press looks like in the log

Each press should produce, in order:

```
[listener] button press: FUNC=FILE ...
[handle_event] FUNC=FILE: starting button scan
[handle_event] FUNC=FILE: Touch-Panel-ON (printer settings), dpi=300 source=adf
[handle_event] FUNC=FILE: scan complete (3 pages, 2512x3253)
[handle_event] FUNC=FILE: wrote /Users/you/Scans/scan-...-FILE-....pdf
```

Read three things off it:

- **The precedence line** — `Touch-Panel-ON (printer settings)` vs
  `Touch-Panel-OFF (daemon config)`, plus the `dpi=` the scan actually used.
  This is the direct check that the right settings source won.
- **`scan complete (N pages, WxH)`** — the page count and the pixel dimensions
  the device delivered for page 1. Compare `WxH` to the expected size below.
- **`wrote <path>`** — one line per file written; the extension tells you the
  format (`.pdf` / `.tiff` / `.jpg`).

A `[button_plan] unknown P= token …` / `unexpected M=`/`T=` warning means the
device sent a token the tables don't know yet — capture it and report it; the
scan still falls back to a safe default rather than aborting.

## 3. Expected page dimensions (300 dpi)

The scan area per paper size at 300 dpi (halve for 200, double for 600):

| Paper | `P=` token | pixels @300 (W×H) |
|---|---|---|
| Letter | `LETTER` | 2512 × 3253 |
| Legal | `LEGAL` | 2512 × 4153 |
| A4 | `A4` | 2448 × 3461 |
| Ledger | `LEDGER` | 3264 × 5053 |
| A3 | `A3` | 3472 × 4913 |

## 4. How to verify a written file

- **Image (`.jpg`/`.tiff`):**
  ```bash
  sips -g pixelWidth -g pixelHeight ~/Scans/scan-...jpg
  ```
  Should match the table above (within a couple px).
- **PDF:** open in Preview and check Tools → Show Inspector: the page should be
  Letter (8.5 × 11 in) / Legal (8.5 × 14 in) proportioned, the whole page
  present, right-side up, and the page count correct. Or:
  ```bash
  mdls -name kMDItemNumberOfPages -name kMDItemPageWidth -name kMDItemPageHeight ~/Scans/scan-...pdf
  ```
  (`PageWidth`/`PageHeight` are in points — Letter ≈ 612 × 792, Legal ≈ 612 × 1008.)

## 5. The press matrix

Set each on the printer's LCD (Touch Panel ON unless the row says Auto), load
the ADF (or use the flatbed where noted), press, then check the log + file.

| # | Route | LCD settings | Source | Expect |
|---|---|---|---|---|
| 1 | File | Color · PDF · Letter · 300 · 1-sided | ADF (multi-sheet) | `Touch-Panel-ON dpi=300`; multi-page PDF; pages 2512×3253 |
| 2 | File | Color · PDF · Legal · 300 · 1-sided | ADF | Legal PDF; pages 2512×4153 |
| 3 | File | Color · PDF · Letter · 300 · **2-sided** | ADF (multi-sheet) | all front+back pages, in order; each 2512×3253 |
| 4 | File | Color · **JPEG** · Letter · 300 | ADF | one `.jpg` per page (not PDF); 2512×3253 |
| 5 | File | **BW** · **TIFF** · Letter · 300 | ADF | `.tiff`, bitonal, Letter |
| 6 | File | **Auto** (Touch Panel off) | ADF | `Touch-Panel-OFF (daemon config)`; format/dpi from `~/.config/brscan-scand.conf`'s `file.*` |
| 7 | OCR | (any) | ADF or flatbed | searchable PDF (see caveat below) |
| 8 | Image | Color · Letter | flatbed | file written; Preview opens the front page |
| 9 | Email | Color · PDF · Letter | ADF | Mail.app opens a new message with the PDF attached — **not sent** |

Row 1 is the headline acceptance case. Rows 2–3 exercise paper→area and the
duplex de-interleave. Rows 4–5 exercise the `T=` format override. Row 6
exercises the daemon-config fallback. Rows 7–9 exercise the routes.

## 6. Known caveats (deferred to follow-ons — not failures)

These LCD options are parsed but intentionally **not acted on yet**, so don't
score them as failures:

- **Skip Blank Page (`W`)** — blank pages are not skipped host-side yet.
- **ADF High Speed (`X`)** — feeds the page landscape; the raw image will come
  out rotated 90°. Leave High Speed **off** for this matrix.
- **OCR sub-formats `T=TXT/HTML/RTF`** — the OCR route always produces a
  searchable PDF for now, regardless of the LCD's OCR file-type choice.

## 7. Recording results

Tick each row off in the Plan 1d live-validation checklist artifact. For any
failure, capture the full log lines for that press and the offending file's
`sips`/`mdls` output. If a `[button_plan] unknown … token` warning appears,
note the exact token — that means a new capture is needed to extend a table.
