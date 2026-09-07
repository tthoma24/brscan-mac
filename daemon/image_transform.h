#pragma once

#include <optional>

#include <CoreGraphics/CGImage.h>

#include "brscan/scanner.h"
#include "brscan/types.h"
#include "output/output_writer.h"  // brscan::scand::kDefaultJpegQuality

// Host-side page rotation for the scan-button "ADF High Speed" mode (the
// config command's `X=1`; see reference/protocol-notes-button-options.md and
// docs/BUTTON.md). In high-speed mode the device feeds the sheet *landscape*
// for throughput and does NOT transpose the scan area it reports -- so the
// raster that comes back is rotated 90 degrees from the upright portrait page
// the user scanned. There is no device flag to undo this; the host must
// rotate the image back. This header owns that transform.
namespace brscan::scand {

// Returns `page` rotated 90 degrees clockwise so a landscape-fed high-speed
// page reads upright, with `width`/`height` swapped. The result keeps the
// input's PixelFormat, re-encoded to that format's on-the-wire representation
// (kRgb -> baseline JPEG, kGray -> raw 8-bit gray, kBitonal -> packed
// 1-bit-per-pixel, MSB-first, 1=black -- matching brscan::ScanResult's
// contract in libbrscan/include/brscan/scanner.h), so a rotated page is
// indistinguishable from a natively-scanned one to every downstream writer.
//
// Decoding reuses output/action_ocr.h's CreateCGImageFromScanResult (the one
// place that turns a ScanResult's native bytes back into a CGImage), so the
// three PixelFormat cases decode exactly as they do everywhere else.
//
// Rotation direction is a single named constant (kRotationRadians in the .mm)
// so it is trivially flippable: high-speed pages could in principle be fed in
// either landscape orientation, and only a device-in-the-loop test can
// confirm which. If real pages come out upside-down, flip that one constant
// from clockwise to counter-clockwise -- nothing else changes.
//
// A kRgb page is re-encoded as JPEG after rotation (its on-the-wire form),
// at `jpeg_quality` (0-100) -- so the high-speed rotation's second-generation
// JPEG loss honors the destination's `<dest>.jpeg_quality` (see
// output_writer.h's kDefaultJpegQuality) rather than ImageIO's unspecified
// default. kGray/kBitonal pages re-encode losslessly and ignore it. The value
// is clamped to 0-100 defensively.
//
// Returns std::nullopt on any decode or re-encode failure (an unreadable
// page, an unsupported bitmap configuration, a JPEG encode that fails). The
// caller logs and leaves the original page in place rather than failing the
// whole job over one page that could not be rotated.
std::optional<brscan::ScanResult> RotatePortrait(const brscan::ScanResult& page,
                                                 int jpeg_quality = kDefaultJpegQuality);

// Same rotation as above, but from an already-decoded page image rather than a
// ScanResult -- the ScanResult overload is exactly this preceded by a
// CreateCGImageFromScanResult() decode. It lets a caller that has already
// decoded a page (e.g. daemon/handle_event's post-scan pipeline, which shares
// one decode between this rotation and the skip-blank check) avoid decoding it
// a second time. `page` supplies the source PixelFormat and dimensions (which
// equal `decoded`'s pixel dimensions) and the re-encode target; `decoded` is
// the pixels to rotate. The caller owns `decoded` and is responsible for
// releasing it. Returns std::nullopt on a re-encode failure, exactly as the
// ScanResult overload does (minus the decode-failure case, which the caller
// handles by not calling this).
std::optional<brscan::ScanResult> RotatePortrait(const brscan::ScanResult& page,
                                                 CGImageRef decoded,
                                                 int jpeg_quality);

}  // namespace brscan::scand
