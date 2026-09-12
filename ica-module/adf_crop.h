// Plan 2 — pure ADF trailing-pad auto-crop.
//
// WHY THIS EXISTS. On ADF color/JPEG scans the MFC-J6920DW pads the decoded
// image up to the REQUESTED height with uniform full-width mid-gray (~128) once
// the physical sheet has ended: when Size is set larger than the fed sheet, the
// tail of the page is a solid gray band (band rows ≈ requested_height −
// sheet_length; measured on our captures: C1-2 ~20 rows, C3 ~8, C5 ~1840, C6-7
// ~3792 — the real content above ends at the true sheet length). Left in place it
// writes an ugly gray strip along the bottom of every over-sized ADF color page.
//
// This unit is the framework-free, hermetically testable core of the crop: given
// a decoded interleaved-RGB page it counts the contiguous trailing rows that are
// device padding, so RunScanSynchronous can shorten the encoded page height to the
// real sheet before writing the file. The JPEG decode (DecodeJpeg) and the
// CoreGraphics encode that consume this live in module_main.mm and are device-side.
//
// Scope: ADF color (kRgb) only. The flatbed does not pad this way, and the
// RLENGTH gray/BW path pads differently (our own white/black fill), so it is out
// of scope here — it could later be auto-cropped from `rows_read` instead. Only
// the FILE (final-encode) path is trimmed; the live band/preview emission is
// untouched.
//
// Clean-room: our own pixel scan over a plain byte buffer; no source copied. The
// 128 pad value and the per-source band heights are measured facts from our own
// captures (see docs/PROTOCOL.md and the runbook C-rows).

#pragma once

#include <cstdint>

namespace brscan::ica {

// Counts the contiguous trailing rows of `rgb` that are DEVICE PADDING: mid-gray
// rows the scanner appended past the end of the sheet. A real pad is not a
// bit-perfect fill — it is the pad value 128 for the overwhelming majority of a
// row's samples with a sparse scatter of bright JPEG-decode specks (measured
// ~1.3% of samples on a captured scan). A row counts as padding when BOTH hold:
//   - at most kPadMaxOutlierPct of its width*3 samples fall OUTSIDE the near-128
//     tolerance band [128-tol, 128+tol] — so a solid white/black margin of real
//     content (≈100% out of band) is not mistaken for padding, while the sparse
//     specks a genuine pad shows are tolerated; AND
//   - the IN-BAND (near-128) samples are FLAT (their max − min ≤ kPadFlatEps) —
//     real scanned content, even a gray region, carries per-pixel variation, so a
//     textured 128-ish row (e.g. dithering 124/132) is not padding.
// Scanning stops at the first row from the bottom that is not padding (the pad is
// contiguous at the tail), so interior gray content is never touched. Returns the
// number of trailing pad rows in [0, height]; a fully-padding buffer returns
// `height` (the caller must guard `height - pad > 0` so a page is never cropped to
// nothing). `bytesPerRow` is the row stride in bytes (width*3 for a tight RGB
// buffer, but a larger stride with row padding is honored). Returns 0 for a null
// buffer, a non-positive width/height, or a stride shorter than width*3.
int TrailingPadRows(const uint8_t* rgb, int width, int height, int bytesPerRow);

// Counts the total contiguous trailing rows to trim from an ADF color page: the
// device gray-128 pad (TrailingPadRows) PLUS the near-white backing overscan above
// it, down to the last row of real content.
//
// WHY GATED ON THE GRAY PAD. On a color scan the device hands back a JPEG already
// padded to the requested height, with no signal for where the sheet actually
// ended. Past the sheet the ADF scans its own near-WHITE backing/roller before the
// device appends the gray-128 pad — and that backing is the same white as paper,
// so a trailing near-white band is indistinguishable by pixels from a genuine
// blank bottom margin. The gray pad is the one thing the device only emits when the
// sheet was SHORTER than the scan window (a page whose content filled the window
// has none). So we treat the gray pad as the device's own "this page overscanned"
// signal: only when a gray pad is present do we also trim the near-white band above
// it. A page with no gray pad is left entirely untouched — its bottom, margin and
// all, is preserved.
//
// A near-white overscan row is one that is ≥ kBlankMinNearWhitePct near-white
// (luminance ≥ kBlankNearWhiteLum) AND ≤ kBlankMaxInkPct ink (luminance <
// kInkLum): the backing reads as clean near-white with at most a sparse fold/mark,
// while any inked or colored content (the footer bar, a line of text) fails one of
// the two and stops the scan. Scanning stops at the first such content row from the
// bottom, so interior blank gaps and the page above are never touched. Returns a
// count in [0, height); a page that is blank to the top still keeps ≥ 1 row via the
// same caller guard as TrailingPadRows. Same defensive returns (0) as TrailingPadRows.
int TrailingOverscanRows(const uint8_t* rgb, int width, int height,
                         int bytesPerRow);

// Paints the last `rows` rows of an interleaved-RGB buffer solid white (0xFF),
// leaving every earlier row untouched.
//
// WHY FILL INSTEAD OF CROP. The ADF trailing overscan (backing + gray pad) has to
// be erased, but SHORTENING the image is counter-productive for the file/PDF path:
// macOS 26's Image Capture builds each PDF page at the selected paper size (Letter)
// and CENTERS the delivered image on it, so a shorter image is padded back with a
// white margin split top and bottom (the wider the crop, the wider the margin).
// Keeping the full height lets the image fill the page exactly (no host margin,
// like an uncropped page), while painting the overscan white removes the gray/
// backing band -- a clean full-page scan with white trailing space. The row count
// comes from TrailingOverscanRows; this only performs the fill.
//
// Fills exactly the width*3 pixel bytes of each row (never the stride padding).
// No-op on a null buffer, non-positive width/height, rows<=0, or a stride shorter
// than width*3; `rows` is clamped to [0,height].
void FillTrailingRowsWhite(uint8_t* rgb, int width, int height, int bytesPerRow,
                           int rows);

}  // namespace brscan::ica
