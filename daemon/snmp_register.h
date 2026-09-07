#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <sys/socket.h>

#include "brscan/types.h"

// SNMPv1 registration for the Brother Scan-button destination table: this
// Mac tells the printer, over SNMP Set, where to send a button-press
// notification (this Mac's IP/port), what to call itself in the printer's
// Scan menu, and which destination (FUNC) the registration is for. See
// reference/protocol-notes-button.md for the captured protocol and
// PROVENANCE.md for the source capture.
namespace brscan::scand {

// Brother application numbers for FUNC, as observed in the capture.
constexpr int kAppNumImage = 1;
constexpr int kAppNumEmail = 2;
constexpr int kAppNumOcr = 3;
constexpr int kAppNumFile = 5;

// Default registration lifetime advertised to the printer, in seconds; the
// caller should re-register before this elapses.
constexpr int kDefaultRegistrationDurationSec = 360;

// Validates `name` for safe embedding as BuildRegisterValue's USER="..."
// field: a `"` would prematurely close that quoted field, and a `;` would
// inject an extra, attacker- or typo-controlled KEY=VALUE token into the
// registration string the printer parses. Returns std::nullopt if `name`
// contains either character, `name` unchanged otherwise. The wire format
// has no escape convention of its own for these characters (nothing in the
// captured protocol suggests one -- see reference/protocol-notes-button.md),
// so this rejects rather than guessing at an escaping scheme that couldn't
// be verified against a real device.
std::optional<std::string> SanitizeDisplayName(const std::string& name);

// Composes the OctetString value carried by the SNMP Set: e.g.
// "TYPE=BR;BUTTON=SCAN;DURATION=360;CC=1;HOST=192.0.2.10:54925;
// USER=\"Test Mac\";FUNC=FILE;APPNUM=5;". `ip`/`port` are where the printer
// should send button notifications (this Mac's UDP listener, see
// reference/protocol-notes-button.md section 2); `name` is the computer
// name shown in the printer's Scan menu; `func` is one of
// FILE|IMAGE|EMAIL|OCR; `appnum` is the matching Brother application number
// (see the kAppNum* constants above); `duration_sec` is the registration
// lifetime in seconds. Does not itself validate `name` -- callers should
// run it through SanitizeDisplayName() first (see above) so a `"` or `;`
// in a user-supplied display name can't corrupt this string.
std::string BuildRegisterValue(
    const std::string& ip, uint16_t port, const std::string& name,
    const std::string& func, int appnum,
    int duration_sec = kDefaultRegistrationDurationSec);

// Encodes a full SNMPv1 set-request PDU registering `value` at the Brother
// button-destination OID 1.3.6.1.4.1.2435.2.3.9.2.11.1.1.0, under
// `community`, tagged with `request_id`. Pure BER construction; does no
// I/O. See reference/protocol-notes-button.md for the byte layout this
// mirrors.
std::vector<uint8_t> BuildSnmpSetRegister(const std::string& community,
                                           uint32_t request_id,
                                           const std::string& value);

// One destination FUNC to advertise, pairing its wire name
// (FILE|IMAGE|OCR|EMAIL) with the matching Brother application number (see
// the kAppNum* constants above). RegisterDestinations() sends one SNMP Set
// per entry.
struct FuncRegistration {
  std::string func;
  int appnum;
};

// One resolved UDP endpoint for the printer's SNMP agent (port 161), as
// produced by an SnmpResolveFn. `addr_len` is the valid length of `addr`.
struct ResolvedEndpoint {
  sockaddr_storage addr{};
  socklen_t addr_len = 0;
};

// Resolves a printer host to its SNMP (UDP 161) endpoint(s); returns an empty
// vector on failure. The production implementation is DefaultSnmpResolver
// (getaddrinfo); tests inject a stub to avoid real name resolution.
using SnmpResolveFn =
    std::function<std::vector<ResolvedEndpoint>(const std::string& host)>;

// Sends `len` bytes at `data` to one resolved endpoint; returns true if the
// whole datagram was handed to the kernel. The production implementation is
// DefaultSnmpSender (a one-shot UDP socket/sendto/close); tests inject a stub.
using SnmpSendFn = std::function<bool(const ResolvedEndpoint& endpoint,
                                      const uint8_t* data, std::size_t len)>;

// The production resolver and sender. Exposed so a caller can wrap them and a
// test can bypass them via SnmpRegistrar's injectable seams.
std::vector<ResolvedEndpoint> DefaultSnmpResolver(const std::string& host);
bool DefaultSnmpSender(const ResolvedEndpoint& endpoint, const uint8_t* data,
                       std::size_t len);

// Sends Brother button-destination registrations (SNMP Set) to one printer,
// caching the printer's resolved SNMP address so repeated registrations do
// not re-resolve the host on every Set. This matters for a `.local` (mDNS)
// printer_host: without the cache each of the per-FUNC Sets, on each 300 s
// re-register cycle, triggered a fresh mDNS query, adding avoidable load to
// the printer's weak shared CPU (GitHub #22).
//
// The address is resolved lazily on the first Send() and reused thereafter. A
// Send() whose datagram cannot be delivered invalidates the cache so the next
// Send() re-resolves -- keeping the daemon correct if the printer's IP
// changes. InvalidateCache() forces a re-resolve on demand (e.g. after a
// config reload changes printer_host, or on a long refresh interval).
//
// Not thread-safe: intended for the daemon's single registration path.
class SnmpRegistrar {
 public:
  // `resolver`/`sender` default to DefaultSnmpResolver/DefaultSnmpSender when
  // left empty; tests pass stubs to observe resolution and capture sends.
  explicit SnmpRegistrar(std::string printer_host,
                         std::string community = "internal",
                         SnmpResolveFn resolver = {}, SnmpSendFn sender = {});

  // Sends one SNMP Set carrying `value` (see BuildRegisterValue), tagged with
  // `request_id`. Resolves the printer host on first use and reuses the cached
  // address. Fire-and-forget: no application-level reply is awaited. Returns
  // Status::kIoError if the host cannot be resolved or the datagram cannot be
  // sent (and drops the cache so the next call re-resolves), Status::kOk
  // otherwise (a UDP send succeeding is not itself delivery confirmation).
  Status Send(const std::string& value, uint32_t request_id);

  // Drops any cached address so the next Send() resolves the host again.
  void InvalidateCache();

  // How many times the host has actually been resolved -- lets a test assert
  // the cache is reused across Sets.
  int resolve_count() const { return resolve_count_; }

  const std::string& printer_host() const { return printer_host_; }

 private:
  // Resolves the host if not already cached; returns true if usable
  // endpoint(s) are available.
  bool EnsureResolved();

  std::string printer_host_;
  std::string community_;
  SnmpResolveFn resolver_;
  SnmpSendFn sender_;
  std::vector<ResolvedEndpoint> endpoints_;
  bool resolved_ = false;
  int resolve_count_ = 0;
};

// Registers each FUNC in `funcs` with the printer via `registrar`, building
// the SNMP Set value from `local_ip`/`port`/`name`/`duration_sec` (see
// BuildRegisterValue) and drawing a fresh request id from `*next_request_id`
// (post-incremented per Set). `on_result`, when set, is invoked with each
// FUNC's name and its send Status so the caller can log per-FUNC. Best-effort:
// a failure for one FUNC does not stop the others. Returns Status::kOk only if
// every FUNC registered successfully, Status::kIoError if any Set failed.
Status RegisterDestinations(
    SnmpRegistrar& registrar, const std::vector<FuncRegistration>& funcs,
    const std::string& local_ip, uint16_t port, const std::string& name,
    int duration_sec, uint32_t* next_request_id,
    const std::function<void(const std::string& func, Status)>& on_result = {});

}  // namespace brscan::scand
