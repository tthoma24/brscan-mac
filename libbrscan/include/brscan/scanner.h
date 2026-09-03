#pragma once

#include <cstdint>
#include <vector>

#include "brscan/transport.h"
#include "brscan/types.h"

namespace brscan {

// The result of a completed scan: the pixel dimensions the device actually
// delivered, and the payload already decoded to a form ready to write out
// (or, for color, still the native encoding) --
//   - PixelFormat::kRgb (M=CGRAY, C=JPEG): a baseline JPEG stream, as the
//     device sent it. Run it through DecodeJpeg for decoded RGB pixels.
//   - PixelFormat::kGray (M=GRAY64, C=NONE -- or M=GRAY256, C=RLENGTH):
//     raw 8-bit samples, one byte per pixel, already the same bytes
//     DecodeGrayRaw would wrap. GRAY256 arrives per-row RLENGTH-
//     compressed on the wire; RunScan decodes it before this struct is
//     filled in, so both cases look identical here.
//   - PixelFormat::kBitonal (M=TEXT or M=ERRDIF, C=RLENGTH): decoded
//     1-bit-per-pixel samples packed per PixelFormat::kBitonal's
//     convention in types.h, already decompressed from the wire's
//     RLENGTH payload -- ready to write out as a PBM P4, for instance.
struct ScanResult {
  PixelFormat format;
  int width;
  int height;
  std::vector<uint8_t> data;
};

// Runs one full scan end-to-end over `transport`, per the flow in
// docs/PROTOCOL.md: greeting, ESC Q (session init), source select
// (ESC S, plus ESC D for the document feeder), ESC I (per-scan negotiate),
// ESC X (execute), then the block header and payload readout.
//
// `transport` must already be connected (see Transport::Connect); RunScan
// neither connects nor disconnects it, matching how Session is used
// elsewhere in this codebase.
//
// If `params.area` is the zero value ({0,0,0,0}), RunScan requests the
// full area granted by the per-scan ESC I offer. Otherwise it requests
// exactly the area given.
//
// On Status::kOk, `out` holds the scanned image's native payload bytes and
// the dimensions the device reported for it. Any other Status reports a
// specific failure:
//   - kBusy: the device reported busy at the greeting (-NG 401).
//   - kNoPaper: params.source == Source::kAdf and the feeder did not ack
//     source selection within the ack timeout. See scanner.cpp for the
//     caveat on this mapping -- no ADF-empty response was ever captured
//     on the wire to confirm it against.
//   - kTimeout: a read stalled past its deadline. Most commonly a
//     device-panel cancel mid-scan, which docs/PROTOCOL.md documents as
//     emitting no explicit status -- the device just stops sending.
//   - kProtocolError: a malformed or incomplete reply (bad offer CSV, bad
//     block header, truncated JPEG, short gray payload).
//   - kIoError: the connection dropped.
//
// Residual risk: two replies in this flow (the ESC Q capability block and
// the ESC S/D source-select ack) have no fixed length and no
// self-describing length prefix, so RunScan reads them by draining until
// the connection goes quiet for a short idle window (see
// Framer::DrainQuiet and kDrainIdleTimeoutMs in scanner.cpp). A reply that
// legitimately pauses mid-transmission longer than that window would be
// read as complete early, and its late tail bytes would corrupt the next
// read. Not observed in this task's live testing, but not something the
// protocol lets this code rule out structurally.
Status RunScan(Transport& transport, const Params& params, ScanResult* out);

}  // namespace brscan
