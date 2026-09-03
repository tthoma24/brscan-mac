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
// ESC X (execute), then the block header and payload readout, looped over
// every page the device sends.
//
// `transport` must already be connected (see Transport::Connect); RunScan
// neither connects nor disconnects it, matching how Session is used
// elsewhere in this codebase.
//
// If `params.area` is the zero value ({0,0,0,0}), RunScan requests the
// full area granted by the per-scan ESC I offer. Otherwise it requests
// exactly the area given.
//
// A single ESC X can make the document feeder stream more than one page
// over the same connection (see docs/PROTOCOL.md, "Multi-page (ADF)"): the
// ESC Q / source-select / ESC I / ESC X sequence runs once, but the
// block/payload readout loops until the device's job-final terminator
// arrives. `out` holds one ScanResult per page, in device order
// (1-based page order, front-to-back for a duplex ADF scan). A flatbed or
// single-sheet ADF scan yields a 1-element vector -- the degenerate case
// of the same loop.
//
// `out->clear()` runs first. On Status::kOk, `out` holds one ScanResult
// per page, each with the scanned image's native payload bytes and the
// dimensions the device reported for it. On any other Status, `out` is
// left empty (cleared) rather than holding a partial page list, so a
// caller can never mistake a partial read for a complete one:
//   - kBusy: the device reported busy at the greeting (-NG 401).
//   - kNoPaper: params.source == Source::kAdf and the feeder did not ack
//     source selection within the ack timeout. See scanner.cpp for the
//     caveat on this mapping -- no ADF-empty response was ever captured
//     on the wire to confirm it against.
//   - kTimeout: a read stalled past its deadline. Most commonly a
//     device-panel cancel mid-scan, which docs/PROTOCOL.md documents as
//     emitting no explicit status -- the device just stops sending.
//   - kProtocolError: a malformed or incomplete reply (bad offer CSV, bad
//     block header, truncated JPEG, short gray payload, malformed
//     end-of-page marker).
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
Status RunScan(Transport& transport, const Params& params,
                std::vector<ScanResult>* out);

}  // namespace brscan
