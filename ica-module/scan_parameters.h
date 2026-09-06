// Plan 2 Task 8 — scanner capability enumeration for the ICA module.
//
// The ICD_ScannerGetParameters callback hands the host a CFMutableDictionary
// describing what the scanner can do. Image Capture reads it to populate the
// resolution / mode / source / size pickers of the scan panel. Plan 2 Task 2
// filled it with invented descriptive keys (supportedResolutions, paperSizes,
// …); the live Scan test showed the host recognised none of them, rendered no
// controls, and never called Start. Task 8 replaces that with the real,
// TWAIN-derived schema Image Capture's scanner modules use.
//
// Schema (built by BuildScannerParameters):
//   functionalUnits = {
//     availableFunctionalUnitTypes : [0, 3],   // flatbed, documentFeeder.
//     selectedFunctionalUnitType   : 0,
//     "0" : { <per-unit capabilities> },        // flatbed.
//     "3" : { <per-unit capabilities> },        // feeder.
//   }
// where each per-unit capability is a TWAIN-style container
//   { "type": "TWON_ENUMERATION" | "TWON_ONEVALUE",
//     "value": <array | scalar>, "current": <v>, "default": <v> }
// under the keys ICAP_XRESOLUTION, ICAP_YRESOLUTION, ICAP_BITDEPTH,
// ICAP_PHYSICALWIDTH, ICAP_PHYSICALHEIGHT, ICAP_SUPPORTEDSIZES, ICAP_UNITS.
//
// Ground-truth paper geometry comes from daemon/paper_size.{h,cpp} (the single
// source of truth per PROVENANCE.md and PLAN-2-DESIGN.md decision E); paper
// tokens map to ICScannerDocumentType values via
// scan_translate.h::DocumentTypeForPaperToken.
//
// CLEAN-ROOM / verifiability: the ICAP_* capability keys, the TWON_* type tags,
// and the ICScanner* enum VALUES are INTERFACE FACTS. The ICAP_* / TWON_* names
// are the TWAIN Specification vocabulary (referenced by name in the SDK header
// ICScannerFunctionalUnits.h and used by Image Capture's TWAIN-derived scanner
// modules); the enum values were verified against the public ImageCaptureCore
// SDK headers. Reimplemented from those facts — no Apple/third-party source was
// copied. What is NOT pinned down by the public headers is the exact NESTING of
// the functionalUnits dict and the per-unit keying (by stringified type); that
// is the module's best reconstruction and is device-in-the-loop, resolved by the
// live re-test and the incoming-theDict diagnostic (see the task report).

#pragma once

#import <CoreFoundation/CoreFoundation.h>

namespace brscan::ica {

// Populates `dict` (an already-created CFMutableDictionaryRef owned by the
// caller) with the scanner's advertised capabilities in the functionalUnits +
// ICAP_* schema described above, scoped per functional unit (flatbed vs document
// feeder). Adds keys only; never clears the dictionary.
void BuildScannerParameters(CFMutableDictionaryRef dict);

}  // namespace brscan::ica
