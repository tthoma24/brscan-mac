#pragma once

#include <string>
#include <vector>

#include "brscan/scanner.h"
#include "brscan/types.h"

// The library-level output writer: turns a completed multi-page scan (a
// list of brscan::ScanResult pages, as RunScan now returns) into the file
// format the user configured for a destination -- mirroring the
// iPrint&Scan Settings dialog's "File Type" and "Document separation"
// choices. This is deliberately hermetic: it decodes each page to a
// CoreGraphics image in memory and writes PDF / TIFF / JPEG / PNG / native
// files, but knows nothing about the daemon, the config file, or which
// FUNC triggered the scan. Task 1c.2b wires it into the daemon's actions.
//
// The Objective-C++ implementation (ImageIO / CoreGraphics / Vision) lives
// in daemon/output_writer.mm; this header is plain C++ so an ordinary .cpp
// caller can drive it.
namespace brscan::scand {

// The output file format, one per "File Type" the vendor dialog offers.
//   kNative writes each page in its own per-PixelFormat file (JPEG for a
//   color page, PGM/P5 for gray, PBM/P4 for bitonal) -- exactly what
//   tools/scan_output.cpp's WritePages already produces, and the format
//   used when no `<dest>.format` key is set.
//
// kText/kHtml/kRtf are OCR-only text sinks: rather than reproduce the
// scanned image, they run Vision text recognition on each page and write
// the recognized text as a single plain-text (.txt), HTML (.html), or RTF
// (.rtf) file (see daemon/action_ocr.h's WriteRecognizedText, where the
// Vision dependency lives). They are only ever selected for the OCR
// destination -- the scan-button's `T=TXT/HTML/RTF` sub-format or the
// `ocr.ocr_format` config key -- since a text sink discards the image.
// Appended after the image formats so existing switch coverage keeps its
// enumerator values stable.
enum class OutputFormat { kNative, kPdf, kTiff, kJpeg, kPng, kText, kHtml, kRtf };

// TIFF compression codec, as an NSTIFFCompression value at write time
// (LZW=5, CCITT Group 3=3, CCITT Group 4=4). G3/G4 are 1-bit fax codecs
// and only apply to a bilevel page; see WriteConfiguredOutput's comment on
// how a non-bilevel page requested with G3/G4 is handled.
enum class TiffCompression { kLzw, kG3, kG4 };

// Document separation, as the vendor dialog's "Document" separation
// offers: combine into one document, separate by image count (a new
// document every N single-sided images), or separate by page count (a new
// document every N pages). kCombine puts every page in one container;
// kEveryImage/kEveryPage each start a new container every `separate_n`
// ScanResults.
//
// kEveryImage and kEveryPage currently behave identically: each
// brscan::ScanResult this codebase produces is one scanned image = one
// side, so for a simplex scan "image" and "page" already coincide. For a
// duplex scan, whether the vendor's "page count" means a physical sheet
// (2 ScanResults) or stays 1-per-side is NOT established by any capture we
// have -- see daemon/config.cpp's ParseSeparationString comment for the
// capture that would confirm it. Until that's known, kEveryPage is
// implemented on ScanResults directly (documented page==image assumption),
// the same as kEveryImage, rather than guessing at sheet-grouping.
enum class OutputSeparation { kCombine, kEveryImage, kEveryPage };

// One destination's output settings (see daemon/config.h, which parses the
// per-FUNC keys into these). Defaults match the config parser's defaults:
// native format, LZW, combine-all.
struct OutputSettings {
  OutputFormat format = OutputFormat::kNative;
  TiffCompression tiff_compression = TiffCompression::kLzw;
  OutputSeparation separation = OutputSeparation::kCombine;
  int separate_n = 1;       // Used when separation != kCombine (>= 1).
  bool searchable = false;  // PDF only: lay a Vision OCR text layer.
};

// Writes `pages` to disk as `settings.format`, deriving each output path
// from `base_path` (e.g. ".../scan-20260903-131605-FILE-64620.jpg"): its
// directory and stem are kept, its extension replaced to match the chosen
// format. Appends every path actually written to `*written` (which is
// cleared first). Returns kOk on success.
//
// Behavior by format:
//   - kNative: delegates to brscan::cli::WritePages -- one native file per
//     page, numbered `-NNN` when there is more than one page. Separation
//     does not apply (there is no container to split).
//   - kPdf: one multi-page PDF; if `settings.searchable`, each page also
//     gets a Vision-recognized invisible text layer.
//   - kTiff: one multi-page TIFF, each page tagged with the requested
//     compression. G3/G4 require a bilevel image: a kBitonal page is
//     written 1-bit with the requested fax codec; a kGray/kRgb page is not
//     bilevel, so G3/G4 falls back to LZW for that page (documented,
//     lossless -- no thresholding).
//   - kJpeg / kPng: one file per page, numbered `-NNN` when there is more
//     than one page (via brscan::cli::PagePath). Separation does not apply
//     to these per-page formats -- kEveryImage/kEveryPage behave like
//     kCombine.
//   - kText / kHtml / kRtf: one `.txt` / `.html` / `.rtf` file holding the
//     Vision-recognized text of every page (see daemon/action_ocr.h's
//     WriteRecognizedText). Single-file output, like kNative -- document
//     separation does not apply.
//
// Separation (kEveryImage / kEveryPage) applies only to the container
// formats (PDF, TIFF): it produces ceil(P / separate_n) files, each
// holding up to `separate_n` pages, named with a `-docNNN` suffix (e.g.
// `-doc001.pdf`). Each ScanResult counts as one page/image; kEveryImage and
// kEveryPage split identically for now (see OutputSeparation's doc
// comment on the duplex page-vs-sheet question this leaves open).
//
// Returns kIoError if a file cannot be written or a page cannot be
// decoded, kProtocolError if a searchable PDF's Vision request fails.
brscan::Status WriteConfiguredOutput(
    const std::vector<brscan::ScanResult>& pages, const OutputSettings& settings,
    const std::string& base_path, std::vector<std::string>* written);

}  // namespace brscan::scand
