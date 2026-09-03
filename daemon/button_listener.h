#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <sys/socket.h>

#include "brscan/types.h"

// A listener for Brother's Scan-button UDP notifications: after this Mac
// registers as a destination (see snmp_register.h), the printer sends a
// small ASCII datagram here each time the panel's Scan button is pressed
// for a registered FUNC, and expects the exact same bytes echoed straight
// back as an acknowledgement. See reference/protocol-notes-button.md for
// the captured protocol and PROVENANCE.md for the source capture.
namespace brscan::scand {

// A parsed button-press notification.
struct ButtonEvent {
  std::string func;      // FILE | IMAGE | OCR | EMAIL.
  std::string user;      // The registered computer name (quoted in the wire
                          // format, unquoted here).
  std::string host_ip;   // The HOST= address's IP part (this Mac, as the
                          // printer knows it).
  uint16_t host_port;    // The HOST= address's port part (54925).
  int appnum;
  std::string regid;     // Per-notification id; kept as a string since it
                          // is only ever echoed back, never computed with.
  int seq;                // Increments across successive button presses.
};

// Parses a raw button-notification UDP datagram (`data`, `len` bytes: the
// exact bytes received off the wire, including the 4-byte header) into a
// ButtonEvent. Returns std::nullopt for anything that does not look like a
// well-formed notification -- too short, a bad or inconsistent header, a
// missing "TYPE=BR"/"BUTTON=SCAN" prefix, a truncated or unparsable
// KEY=VALUE payload, or a non-numeric APPNUM/SEQ/port -- and never reads
// outside [data, data + len).
//
// Wire format (see reference/protocol-notes-button.md): a 4-byte header
// `02 00 <len> 30`, then an ASCII, semicolon-delimited payload:
//   TYPE=BR;BUTTON=SCAN;USER="<name>";FUNC=<FILE|IMAGE|OCR|EMAIL>;
//   HOST=<ip>:<port>;APPNUM=<n>;P1=0;P2=0;P3=0;P4=0;REGID=<id>;SEQ=<n>;
// `<len>` (header byte 2) is not the payload's byte length directly: in
// the real capture (reference/streams/button_notify_hex.txt, git-ignored)
// it equals payload.size() + 5 -- 4 for this header plus 1, consistent
// with counting a trailing NUL that is never actually put on the wire.
// This parser checks that arithmetic against the datagram it actually
// received rather than trusting `<len>` for bounds, so a corrupt or
// truncated datagram is rejected instead of read out of bounds.
std::optional<ButtonEvent> ParseNotification(const uint8_t* data, size_t len);

// Binds UDP port 54925 (Brother's fixed button-notification port; see
// PROVENANCE.md) and receives Scan-button notifications from the printer,
// handing back both the parsed event and the raw datagram + sender address
// so the caller can Ack() it. Blocking BSD SOCK_DGRAM, mirroring
// TcpTransport's socket style (brscan/transport_tcp.h): Receive() uses
// SO_RCVTIMEO to bound a single recvfrom() call.
class ButtonListener {
 public:
  static constexpr uint16_t kDefaultPort = 54925;

  // `port` defaults to Brother's fixed notification port; tests pass 0 to
  // let the OS pick an ephemeral port (see port()), since binding the real
  // port is not reliable in every test environment.
  explicit ButtonListener(uint16_t port = kDefaultPort);
  ~ButtonListener();

  ButtonListener(const ButtonListener&) = delete;
  ButtonListener& operator=(const ButtonListener&) = delete;

  // Creates the UDP socket and binds it to the configured port on all
  // interfaces. Status::kIoError on any socket()/bind() failure, or if
  // this ButtonListener is already bound (call Bind() at most once per
  // instance; a second call would otherwise leak the first socket).
  Status Bind();

  // Blocks for up to `timeout_ms` for one UDP datagram. On success, fills
  // `*raw` with the exact bytes received and `*from`/`*fromlen` with the
  // sender's address (both needed by Ack()), and parses the datagram into
  // `*event`. Status::kTimeout if nothing arrives in time,
  // Status::kProtocolError if a datagram arrives but does not parse as a
  // notification (`*raw`/`*from` are still filled in that case; `*event`
  // is not), Status::kIoError on a socket error, Status::kOk otherwise.
  Status Receive(int timeout_ms, ButtonEvent* event, std::vector<uint8_t>* raw,
                 sockaddr_storage* from, socklen_t* fromlen);

  // Echoes `raw` back to `to` verbatim -- the printer's whole ACK contract
  // is a byte-for-byte echo of the notification it sent (confirmed in the
  // capture; see PROVENANCE.md). Status::kIoError if the datagram cannot
  // be sent in full.
  Status Ack(const std::vector<uint8_t>& raw, const sockaddr_storage& to,
             socklen_t tolen);

  // The UDP port actually bound. Differs from the constructor's `port`
  // argument only when that argument was 0 (OS-assigned ephemeral port),
  // as in the loopback test.
  uint16_t port() const { return port_; }

 private:
  uint16_t port_;
  int fd_ = -1;
};

}  // namespace brscan::scand
