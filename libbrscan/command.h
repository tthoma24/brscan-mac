#pragma once

#include <cstdint>
#include <vector>

#include "brscan/types.h"

namespace brscan {

// Encoders for the host-to-scanner command bytes described in
// docs/PROTOCOL.md. Every command is `0x1b <letter> 0x0a [body] 0x80`,
// except EncodeReset (see below).

// ESC Q: session init, sent once after connect. No body.
std::vector<uint8_t> EncodeQuery();

// ESC R: a bare two-byte sequence, `0x1b 0x52`, with no trailing 0x0a and
// no 0x80 terminator. This is the one exception to the standard
// `ESC <letter> LF body 0x80` framing used by every other command. In the
// capture it appeared exactly once, mid-stream (not at connection teardown
// as the name might suggest), and its exact role is unconfirmed; it is not
// part of the normal scan flow (ESC Q, ESC S/D, ESC I, ESC X).
std::vector<uint8_t> EncodeReset();

// ESC S FB: select the flatbed source.
std::vector<uint8_t> EncodeSelectFlatbed();

// ESC D ADF: select the document feeder source.
std::vector<uint8_t> EncodeSelectAdf();

// ESC I: negotiate resolution, mode, and simplex/duplex before a scan.
std::vector<uint8_t> EncodeInfo(int x_dpi, int y_dpi, ScanMode mode,
                                 bool duplex);

// ESC X: start the scan with the given parameters.
std::vector<uint8_t> EncodeExecute(const Params& params);

}  // namespace brscan
