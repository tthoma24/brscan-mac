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

// Scan color mode.
//
// kColor and kGray use the original M=CGRAY/C=JPEG and M=GRAY64/C=NONE
// flow. kBlackWhite (M=TEXT), kErrorDiffusion (M=ERRDIF), and kTrueGray
// (M=GRAY256) all use the newer C=RLENGTH per-row run-length payload
// (see docs/PROTOCOL.md's "RLENGTH" section and decode_rlength.h):
// kBlackWhite/kErrorDiffusion decode to 1-bit-per-pixel images
// (PixelFormat::kBitonal), kTrueGray to 8-bit gray (PixelFormat::kGray,
// same representation as kGray's raw payload).
enum class ScanMode { kColor, kGray, kBlackWhite, kErrorDiffusion, kTrueGray };

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
//
// kBitonal is 1 bit per pixel, packed 8 pixels to a byte, most-significant
// bit first, one bit value per pixel where 1 = black and 0 = white (the
// same convention as a PBM P4 file, and the standard fax/TIFF Group 3
// packing this codebase's RLENGTH decoder does not need to re-derive --
// see decode_rlength.h). Each row is padded to a whole byte, so a
// kBitonal Image's row stride is (width + 7) / 8 bytes.
enum class PixelFormat { kRgb, kGray, kBitonal };

// A fully decoded scan image: interleaved RGB (3 bytes/pixel) for a
// decoded JPEG, raw 8-bit samples (1 byte/pixel) for a GRAY64/C=NONE or
// GRAY256/C=RLENGTH payload, or packed 1-bit-per-pixel samples (see
// PixelFormat::kBitonal) for a TEXT or ERRDIF/C=RLENGTH payload.
struct Image {
  int width;
  int height;
  PixelFormat format;
  std::vector<uint8_t> pixels;
};

}  // namespace brscan
