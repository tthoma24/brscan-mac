#pragma once

namespace brscan {

// Returns the library version as a semantic version string.
const char* Version();

// Scan color mode. Text/black-and-white modes are not yet in scope; see
// docs/PROTOCOL.md.
enum class ScanMode { kColor, kGray };

// Physical source to scan from.
enum class Source { kFlatbed, kAdf };

// Scan area in pixels at the scan resolution: (x0, y0) to (x1, y1).
struct Area {
  int x0;
  int y0;
  int x1;
  int y1;
};

// Parameters for an ESC X execute command.
struct Params {
  ScanMode mode = ScanMode::kColor;
  Source source = Source::kFlatbed;
  int x_dpi = 300;
  int y_dpi = 300;
  int brightness = 50;
  int contrast = 50;
  Area area = {0, 0, 0, 0};
  bool duplex = false;
};

}  // namespace brscan
