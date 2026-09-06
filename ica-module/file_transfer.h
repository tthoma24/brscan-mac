// Plan 2 Task 12 — pure filename / UTI plan for file-based transfer.
//
// When Image Capture (or any local ImageCaptureCore client) runs a FINAL scan
// it asks the module for FILE-BASED transfer: it hands us a destination folder
// plus a document name, extension, and format (UTI), and expects the module to
// ENCODE the page and WRITE it into that folder as <name>.<extension> (see
// PLAN-2-DESIGN.md, "Data hand-back and pixel formats"). The overview/preview
// pass instead asks for in-memory bands and carries none of these keys.
//
// This unit is the framework-free, hermetically testable core of that path: it
// turns the host's three document strings (format UTI / extension / name) into
// a concrete {uti, extension, stem} plan and builds the per-page output file
// name. All of the CoreFoundation / ImageIO / security-scoped-URL work that
// consumes this plan lives in module_main.mm and is device-side, not unit
// testable; keeping the string policy here means the defaulting rules (default
// TIFF; derive a missing extension from the UTI and vice-versa; strip a
// duplicated extension from the name; index multi-page names) stay covered.
//
// Clean-room: our own string policy over libbrscan-free std::string inputs. The
// document key names and the <name>.<ext> file-naming convention are interface
// facts confirmed from Apple's public ICADevices sample module VirtualScanner
// (Sources/VirtualScanner.m: the "document name"/"document folder"/"document
// format"/"document extension" keys and its <folder>/<name>.<ext> save path);
// no source is copied. The UTIs are the stable public ImageIO type identifiers.

#pragma once

#include <string>

namespace brscan::ica {

// The resolved encode/write plan for a file-based transfer. `uti` is the
// ImageIO type identifier handed to CGImageDestinationCreateWithURL (e.g.
// "public.tiff"); `extension` is the lower-case filename extension without a
// dot (e.g. "tif"); `stem` is the document name with any duplicated extension
// removed (e.g. "Scan").
struct TransferPlan {
  std::string uti;
  std::string extension;
  std::string stem;
};

// Builds the plan from the host's document keys. Every input is optional and
// tolerated empty:
//   - format/extension both empty            -> TIFF ("public.tiff" / "tif").
//   - format empty, extension given          -> UTI derived from the extension
//                                               (unknown extension -> TIFF).
//   - format given, extension empty          -> extension derived from the UTI
//                                               (unknown UTI -> "tif").
//   - name empty                             -> "Scan".
//   - name ending in ".<extension>"          -> the extension is stripped from
//                                               the stem so it is not doubled.
// Only the panel's own formats (TIFF/JPEG/PNG) are mapped both ways; any other
// non-empty UTI is passed through verbatim (ImageIO validates it at encode).
TransferPlan PlanTransfer(const std::string& document_format,
                          const std::string& document_extension,
                          const std::string& document_name);

// The output file name for one page of a plan. Page 0 is "<stem>.<extension>";
// page N>0 is "<stem> N.<extension>" so a multi-page (ADF) job does not
// overwrite itself. Negative indices are treated as 0.
std::string TransferFilenameForPage(const TransferPlan& plan, int page_index);

}  // namespace brscan::ica
