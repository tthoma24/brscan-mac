#pragma once

#include <cstdint>
#include <vector>

namespace brscan {

// Returns the library version as a semantic version string.
const char* Version();

// Outcome of a Transport or protocol operation.
enum class Status {
  kOk,
  kIoError,
  kProtocolError,
  kBusy,
  kNoPaper,
  kCancelled,
  kTimeout,
};

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

// The scanner's granted-parameters reply to ESC I, decoded from the
// comma-terminated ASCII offer described in docs/PROTOCOL.md
// (`xdpi,ydpi,flag,?,xmaxpx,?,ymaxpx,`). Only the fields with a confirmed
// meaning are exposed; see response.h for the unconfirmed ones.
struct Offer {
  int x_dpi;
  int y_dpi;
  int width_px;
  int height_px;
};

// Pixel layout of a decoded Image.
enum class PixelFormat { kRgb, kGray };

// A fully decoded scan image: interleaved RGB (3 bytes/pixel) for a
// decoded JPEG, or raw 8-bit samples (1 byte/pixel) for a GRAY64/C=NONE
// payload.
struct Image {
  int width;
  int height;
  PixelFormat format;
  std::vector<uint8_t> pixels;
};

}  // namespace brscan
