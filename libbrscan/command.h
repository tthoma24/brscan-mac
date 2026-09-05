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

// ESC K: the scan-button flow's opener (`1b 4b 0a 80`), sent once per
// button-scan connection in place of ESC Q. No body. The device replies
// with the pushed config-command frame (see docs/BUTTON.md's "Config
// command") rather than the ESC Q capability block.
std::vector<uint8_t> EncodeButtonQuery();

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
// button_flow appends S=NORMAL_SCAN even for a color mode (the scan-button
// flow carries it unconditionally; see docs/BUTTON.md). It defaults false
// so the normal RunScan flow's bytes are unchanged.
std::vector<uint8_t> EncodeInfo(int x_dpi, int y_dpi, ScanMode mode,
                                 bool duplex, bool button_flow = false);

// ESC X: start the scan with the given parameters.
std::vector<uint8_t> EncodeExecute(const Params& params);

}  // namespace brscan
