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
// Schema (built by BuildScannerParameters): everything is wrapped in a single
// top-level `device` dict. The capability entries for the SELECTED functional
// unit sit FLAT inside it; a `functionalUnits` sub-dict carries ONLY the
// available/selected unit types.
//   device = {
//     ICAP_XRESOLUTION, ICAP_YRESOLUTION, ICAP_BITDEPTH, ICAP_PIXELTYPE,
//     ICAP_PHYSICALWIDTH, ICAP_PHYSICALHEIGHT, ICAP_SUPPORTEDSIZES, ICAP_UNITS,
//     CAP_FEEDERENABLED, CAP_DUPLEX,          // source / duplex controls.
//     functionalUnits : {
//       availableFunctionalUnitTypes : [0, 3],  // flatbed, documentFeeder.
//       selectedFunctionalUnitType   : <tracked>,  // 0 flatbed / 3 feeder.
//     }
//   }
// where each ICAP_*/CAP_* capability is a TWAIN-style container
//   { "type": "TWON_ENUMERATION" | "TWON_ONEVALUE",
//     "value": <array | scalar>, "current": <v>, "default": <v> }.
// The flat entries describe the SELECTED unit and selectedFunctionalUnitType
// echoes the host's tracked choice (BuildScannerParameters' argument): selecting
// the document feeder makes the host set selectedFunctionalUnitType=3 in
// SetParameters and re-call GetParameters, so answering with the tracked unit
// (not always the flatbed) is what stops the host re-selecting in a loop. Both
// unit types are always advertised, which is what makes the source picker appear.
//
// Ground-truth paper geometry comes from daemon/paper_size.{h,cpp} (the single
// source of truth per PROVENANCE.md and PLAN-2-DESIGN.md decision E); paper
// tokens map to ICScannerDocumentType values via
// scan_translate.h::DocumentTypeForPaperToken.
//
// CLEAN-ROOM / verifiability: the `device`/`functionalUnits` wrapper keys, the
// ICAP_*/CAP_* capability keys, the TWON_* type tags, and the ICScanner* enum
// VALUES are INTERFACE FACTS. The ICAP_*/CAP_*/TWON_* names are the TWAIN
// Specification vocabulary (ICAP_PIXELTYPE and ICAP_SUPPORTEDSIZES are named in
// the SDK header ICScannerFunctionalUnits.h; the rest are used by Image
// Capture's TWAIN-derived scanner modules); the enum values were verified
// against the public ImageCaptureCore SDK headers. The `device`-wrapper + flat
// capabilities structure is a documented-by-example interface fact, reconstructed
// from the shape those modules emit — no Apple/third-party source was copied. The
// incoming-theDict diagnostic and the live re-test confirm it end to end (see the
// task report).

#pragma once

#import <CoreFoundation/CoreFoundation.h>

namespace brscan::ica {

// Populates `dict` (an already-created CFMutableDictionaryRef owned by the
// caller) with the scanner's advertised capabilities under the top-level
// `device` dict described above (flat ICAP_*/CAP_* entries for the selected unit
// plus a functionalUnits sub-dict). Adds the `device` key only; never clears the
// dictionary.
//
// `selectedFunctionalUnitType` is the ICScannerFunctionalUnitType the host has
// currently selected (0 = flatbed, 3 = document feeder; the module tracks it
// across SetParameters). The flat capabilities describe THAT unit and
// functionalUnits.selectedFunctionalUnitType echoes it, so a host that selects
// the feeder reads its selection back unchanged instead of looping on a
// flatbed-only answer. Any value other than the document feeder is treated as
// the flatbed.
void BuildScannerParameters(CFMutableDictionaryRef dict,
                            int selectedFunctionalUnitType);

}  // namespace brscan::ica
