#pragma once

#include <string>

#include <CoreGraphics/CoreGraphics.h>

#include "brscan/types.h"

// The OCR destination action's core: turn one already-saved scan (JPEG
// for color, PGM/P5 for gray/truegray, PBM/P4 for bw/errdiff -- see
// tools/scan_output.h's WriteOutput, which is what wrote it) into a
// searchable PDF via the Vision framework's text recognition.
//
// This header is plain C++ (its one non-standard type, CGImageRef, is a
// C API from CoreGraphics.h and compiles fine under a C++-only
// translation unit) so daemon/actions.cpp -- an ordinary .cpp file, not
// Objective-C++ -- can call OcrImageToSearchablePdf() directly. The
// Objective-C++ bits (Vision, CoreText, PDFKit-adjacent CoreGraphics PDF
// context calls) live in daemon/action_ocr.mm.
namespace brscan {

// Loads `image_path` into a CGImageRef. Recognizes three formats by
// content, not by extension:
//   - PGM (P5): an 8-bit grayscale raster, loaded directly.
//   - PBM (P4): a 1-bit-per-pixel raster (MSB-first, 1=black per
//     PixelFormat::kBitonal's convention -- see brscan/types.h), expanded
//     to 8-bit grayscale.
//   - anything else: handed to ImageIO (CGImageSourceCreateWithData),
//     which covers the JPEG case (color scans).
//
// Returns nullptr on any read, parse, or decode failure. The caller owns
// the returned image and must CGImageRelease() it.
CGImageRef LoadImageAsCGImage(const std::string& image_path);

// Runs Vision text recognition (VNRecognizeTextRequest, accurate level)
// on `image_path` and composes a searchable PDF at `pdf_path`: a single
// page sized to the image, the image itself drawn as the page's visible
// content, and each recognized text string drawn again on top in
// invisible text (kCGTextInvisible), scaled and positioned over its
// normalized bounding box -- so the page looks identical to the original
// scan but its text is selectable and searchable in a PDF viewer.
//
// Returns:
//   kOk             the PDF was written successfully. This includes the
//                    case where Vision recognized no text at all -- the
//                    page is still a faithful copy of the image, just
//                    without a text layer.
//   kIoError        `image_path` could not be loaded, or `pdf_path`
//                    could not be created/written.
//   kProtocolError  Vision's text recognition request itself failed
//                    (e.g. an internal Vision/CoreML error).
Status OcrImageToSearchablePdf(const std::string& image_path,
                                const std::string& pdf_path);

}  // namespace brscan
