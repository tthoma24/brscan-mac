#pragma once

#include <string>
#include <vector>

#include <CoreGraphics/CoreGraphics.h>

#include "brscan/scanner.h"
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

// Converts one already-decoded scan page (a brscan::ScanResult, see
// libbrscan/include/brscan/scanner.h) to a CGImageRef entirely in memory,
// with no intermediate file. The three PixelFormat cases map exactly as
// LoadImageAsCGImage does for the on-disk forms:
//   - kRgb: `data` is a baseline JPEG stream, decoded via ImageIO.
//   - kGray: `data` is 8-bit grayscale, one byte per pixel, wrapped
//     directly.
//   - kBitonal: `data` is packed 1-bit-per-pixel (see PixelFormat::kBitonal
//     in types.h), expanded to 8-bit grayscale.
//
// Returns nullptr on any decode failure or if `page.data` is too short for
// its stated dimensions. The caller owns the returned image and must
// CGImageRelease() it. Shares its JPEG-bytes-to-CGImage and
// gray/bitonal-to-CGImage implementation with LoadImageAsCGImage.
CGImageRef CreateCGImageFromScanResult(const brscan::ScanResult& page);

// Writes `images` (already-decoded pages, in order) as one multi-page PDF
// at `pdf_path`, each page sized to its image's pixels with the image drawn
// as the page's visible content. If `searchable`, each page also gets a
// Vision-recognized invisible text layer (see ComposeSearchablePdf's page
// helper) so the PDF is selectable and searchable; if not, the pages carry
// no text layer. The caller retains ownership of every CGImageRef in
// `images` (this function neither retains nor releases them).
//
// Returns:
//   kOk             the PDF was written (a searchable page that recognized
//                    no text is still kOk -- it is just a faithful image).
//   kIoError        `images` is empty, or `pdf_path` could not be
//                    created/written.
//   kProtocolError  `searchable` was requested and Vision's recognition
//                    request itself failed on some page.
Status WriteSearchablePdf(const std::vector<CGImageRef>& images,
                          bool searchable, const std::string& pdf_path);

// The text document format WriteRecognizedText emits: plain UTF-8 text,
// HTML, or RTF. Distinct from OutputFormat's kText/kHtml/kRtf (which name
// the same three sinks at the config/output-writer layer); this enum keeps
// action_ocr's public surface independent of daemon/output_writer.h.
enum class OcrTextFormat { kPlain, kHtml, kRtf };

// Runs Vision text recognition (VNRecognizeTextRequest, accurate level) on
// every image in `images` (already-decoded pages, in order) and writes the
// recognized text to `path` as `format`:
//   - kPlain: UTF-8 plain text, one recognized line per '\n', with a blank
//     line separating consecutive pages.
//   - kHtml / kRtf: the same joined text built into one NSAttributedString
//     and exported via -dataFromRange:documentAttributes: with
//     NSHTMLTextDocumentType / NSRTFTextDocumentType (Foundation owns all
//     escaping/encoding). A viewer opens either as the recognized text.
//
// The caller retains ownership of every CGImageRef in `images` (this
// function neither retains nor releases them).
//
// Returns:
//   kOk             the text file was written (a page that recognized no
//                    text contributes no lines -- an all-blank scan yields
//                    an empty or near-empty file, still kOk).
//   kIoError        `images` is empty, or `path` could not be written (or,
//                    for kHtml/kRtf, Foundation could not serialize the
//                    document).
//   kProtocolError  Vision's recognition request itself failed on some
//                    page (matches WriteSearchablePdf's contract).
Status WriteRecognizedText(const std::vector<CGImageRef>& images,
                           OcrTextFormat format, const std::string& path);

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
