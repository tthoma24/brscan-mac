// Plan 2 Task 2 — scanner parameter enumeration for the ICA module.
//
// The ICD_ScannerGetParameters callback hands the host a CFMutableDictionary
// describing what the scanner can do: its resolutions, colour modes, sources,
// and the standard paper sizes within the device's maximum scannable area. The
// host reads that dictionary to populate the resolution / mode / source / size
// pickers of the Image Capture scan panel.
//
// This unit builds that dictionary. It reuses the captured, ground-truth paper
// geometry from daemon/paper_size.{h,cpp} (the single source of truth per
// PROVENANCE.md and PLAN-2-DESIGN.md decision E) rather than re-deriving areas
// from nominal page dimensions.
//
// IMPORTANT (clean-room / verifiability): the CFString KEYS this dictionary is
// keyed by are NOT published in the ICADevices SDK headers. The module-side
// parameter vocabulary is a legacy, undocumented contract; the public headers
// only expose the CLIENT-side enums (ImageCaptureCore's ICScannerPixelDataType,
// ICScannerBitDepth, ICScannerFunctionalUnitType, ICScannerMeasurementUnit).
// The keys used here are our own descriptive names carrying values in those
// public enums' units. They must be confirmed against a live icdd trace before
// the panel's pickers can be trusted to bind to them; see the task report. The
// values (resolutions, modes, sources, areas) are correct device black-box
// facts regardless of the key spelling.

#pragma once

#import <CoreFoundation/CoreFoundation.h>

namespace brscan::ica {

// Populates `dict` (an already-created CFMutableDictionaryRef owned by the
// caller) with the scanner's advertised capabilities: supported resolutions,
// pixel-data types, functional units (flatbed + document feeder), a default
// selection for each, and an array of standard paper sizes (name + max
// scannable pixel extent at a reference DPI, and whether the size is
// flatbed-only). Adds keys only; never clears the dictionary.
void BuildScannerParameters(CFMutableDictionaryRef dict);

}  // namespace brscan::ica
