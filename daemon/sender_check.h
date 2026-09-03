#pragma once

#include <optional>
#include <string>
#include <vector>

#include <sys/socket.h>

// Defense-in-depth against a forged Scan-button notification arriving
// from somewhere other than the actual printer: the daemon loop (see
// tools/brscan-scand.cpp) resolves `printer_host` to its numeric IP
// address(es) and drops any notification whose UDP sender doesn't match,
// before ever ACKing it or handing it to HandleButtonEvent. This is on
// top of, not instead of, daemon/handle_event.cpp's own FUNC/path
// validation -- an attacker who *is* on the same broadcast domain as the
// printer's real IP (e.g. via IP spoofing on an unswitched network)
// isn't stopped by this check alone.
namespace brscan::scand {

// Resolves `host` to its numeric IP address(es) (IPv4 and/or IPv6,
// de-duplicated). Returns an empty vector if `host` can't be resolved at
// all (e.g. a transient mDNS hiccup) -- callers should treat that as
// "the check is temporarily unavailable", not "reject everything": see
// IsAllowedSender's fail-open behavior on an empty list.
std::vector<std::string> ResolveHostIps(const std::string& host);

// Extracts the numeric IP address from an IPv4 or IPv6 `addr` (as filled
// in by e.g. ButtonListener::Receive's `*from`). std::nullopt for any
// other address family or on error.
std::optional<std::string> AddressToString(const sockaddr_storage& addr);

// True if `sender_ip` is one of `allowed_ips`, OR if `allowed_ips` is
// empty. The empty case means host resolution was unavailable when this
// was last attempted (see ResolveHostIps) -- failing open on *this*
// check alone rather than dropping every notification during a
// transient DNS/mDNS hiccup, since daemon/handle_event.cpp's FUNC and
// path-traversal defenses remain fully in effect regardless of this
// function's answer.
bool IsAllowedSender(const std::vector<std::string>& allowed_ips,
                      const std::optional<std::string>& sender_ip);

}  // namespace brscan::scand
