// brscan-scand: the Scan-button daemon. Registers this Mac with the
// printer as a destination for the configured FUNCs (cfg.register_funcs,
// all of FILE/IMAGE/OCR/EMAIL by default -- see daemon/config.h), listens
// for button-press notifications on UDP 54925, and on each press ACKs it,
// pulls the scan over TCP 54921, writes it to disk, and dispatches the
// FUNC's action (only FILE is implemented so far -- see daemon/actions.h).
// See reference/plan-master.md's Plan 1b for the overall design.

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "brscan/transport_tcp.h"
#include "brscan/types.h"
#include "button_listener.h"
#include "config.h"
#include "handle_event.h"
#include "log_safe.h"
#include "notification_deduper.h"
#include "output/page_writer.h"
#include "sender_check.h"
#include "snmp_register.h"

namespace {

// Re-registration cadence: comfortably under the 360-second DURATION each
// registration advertises (see daemon/snmp_register.h's
// kDefaultRegistrationDurationSec), so a re-register always lands before
// the printer would let the prior one expire.
constexpr int kReregisterIntervalSec = 300;

// How long the daemon reuses a resolved printer address before resolving it
// again as a slow guard against a silently-changed IP (GitHub #22). The
// printer's SNMP address, this Mac's advertised local address, and the
// sender-check IPs are all cached across re-register cycles rather than
// re-resolved each time (each getaddrinfo of a `.local` host is an mDNS
// query the printer must answer). A failed registration re-resolves right
// away regardless of this interval; this only bounds how long an IP change
// that did *not* fail a send can go unnoticed. Chosen well above the 300 s
// re-register cadence so it doesn't reintroduce per-cycle lookups.
constexpr int kResolveRefreshSec = 3600;

// Upper bound on a single ButtonListener::Receive() call, independent of
// how long remains until the next re-register. Without this cap, a quiet
// period (no button presses, next re-register minutes away) would leave
// Receive() blocked in recvfrom() for that whole span; a SIGINT/SIGTERM
// arriving mid-wait interrupts that call (EINTR) but Receive()'s own
// retry loop just re-blocks for whatever of the original timeout remains
// (see daemon/button_listener.cpp), so the signal wouldn't be noticed
// until the timeout it was already waiting on elapsed. Re-checking
// g_stop_requested at least this often keeps shutdown responsive instead.
constexpr int kMaxReceiveWaitMs = 1000;

constexpr uint16_t kScanPort = 54921;
constexpr uint16_t kSnmpProbePort = 161;
constexpr char kSnmpCommunity[] = "internal";

// Fallback USER= name used only if the configured display_name fails
// SanitizeDisplayName() (contains '"' or ';') -- keeps registration
// working (with a generic name) rather than silently skipping it.
constexpr char kFallbackDisplayName[] = "brscan-mac";

// Set by the SIGINT/SIGTERM handler; std::sig_atomic_t so it's safe to
// write from a signal handler and read from the main loop without a lock.
volatile std::sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int) { g_stop_requested = 1; }

// Set by the SIGHUP handler, mirroring g_stop_requested above exactly: an
// async-signal-safe handler only flips this flag, and the loop below does
// the actual (non-async-signal-safe) config re-read at its own safe point.
// See daemon/config.h's TryReloadConfig() for the reload itself.
volatile std::sig_atomic_t g_reload_requested = 0;

void HandleReloadSignal(int) { g_reload_requested = 1; }

// One FUNC's registration constants (see daemon/snmp_register.h's
// kAppNum* and reference/plan-master.md's APPNUM table).
struct FuncSpec {
  const char* func;
  int appnum;
};

constexpr FuncSpec kFuncs[] = {
    {"FILE", brscan::scand::kAppNumFile},
    {"IMAGE", brscan::scand::kAppNumImage},
    {"OCR", brscan::scand::kAppNumOcr},
    {"EMAIL", brscan::scand::kAppNumEmail},
};

// The FUNCs to register this run, derived from cfg.register_funcs (GitHub
// #22): only the configured destinations are advertised, kept in the
// canonical kFuncs order. cfg.register_funcs defaults to all four (see
// daemon/config.h), so an unconfigured daemon still registers every
// destination -- the filtering only narrows the set when the user opts into
// a subset.
std::vector<brscan::scand::FuncRegistration> ConfiguredFuncRegistrations(
    const brscan::scand::Config& cfg) {
  std::vector<brscan::scand::FuncRegistration> registrations;
  for (const FuncSpec& spec : kFuncs) {
    if (std::find(cfg.register_funcs.begin(), cfg.register_funcs.end(),
                  spec.func) != cfg.register_funcs.end()) {
      registrations.push_back({spec.func, spec.appnum});
    }
  }
  return registrations;
}

// Determines the local IP address this Mac would use to reach `host`, by
// opening a UDP socket, connect()ing it (which resolves the route but
// sends no packet -- UDP "connect" just records a default peer), and
// reading back the socket's own address. This is what the registration's
// HOST= field needs to tell the printer where to send button
// notifications, and lets the daemon work on a different LAN/interface
// without a hardcoded IP in the config.
std::optional<std::string> LocalIpForPeer(const std::string& host,
                                            uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo* results = nullptr;
  const std::string port_str = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &results) != 0 ||
      results == nullptr) {
    return std::nullopt;
  }

  std::optional<std::string> ip;
  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
      close(fd);
      continue;
    }

    sockaddr_storage local{};
    socklen_t local_len = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0) {
      char buf[INET6_ADDRSTRLEN] = {0};
      const void* addr_ptr = nullptr;
      if (local.ss_family == AF_INET) {
        addr_ptr = &reinterpret_cast<sockaddr_in*>(&local)->sin_addr;
      } else if (local.ss_family == AF_INET6) {
        addr_ptr = &reinterpret_cast<sockaddr_in6*>(&local)->sin6_addr;
      }
      if (addr_ptr != nullptr &&
          inet_ntop(local.ss_family, addr_ptr, buf, sizeof(buf)) != nullptr) {
        ip = std::string(buf);
      }
    }
    close(fd);
    if (ip.has_value()) break;
  }

  freeaddrinfo(results);
  return ip;
}

// SNMP-registers the configured FUNCs (see ConfiguredFuncRegistrations) with
// the printer via `registrar`, using the already-resolved `local_ip` for the
// HOST= field. The registrar caches the printer's SNMP address across Sets
// and cycles (see daemon/snmp_register.h), so this no longer re-resolves the
// host per Set. Best-effort: a per-FUNC failure is logged and does not stop
// the daemon. Returns false if any FUNC failed to send (so the caller
// re-resolves next cycle in case the printer's IP changed) or if there was
// nothing configured to register.
bool RegisterConfiguredDestinations(brscan::scand::SnmpRegistrar& registrar,
                                     const brscan::scand::Config& cfg,
                                     const std::string& local_ip,
                                     uint16_t listen_port,
                                     uint32_t* next_request_id) {
  std::string name;
  if (const auto sanitized = brscan::scand::SanitizeDisplayName(cfg.display_name)) {
    name = *sanitized;
  } else {
    std::cerr << "[register] display_name '" << cfg.display_name
               << "' contains '\"' or ';', which would corrupt the "
                  "registration string; using '"
               << kFallbackDisplayName << "' instead\n";
    name = kFallbackDisplayName;
  }

  const auto registrations = ConfiguredFuncRegistrations(cfg);
  if (registrations.empty()) {
    // Can't normally happen: register_funcs defaults to all four and an
    // empty parse falls back to that (see daemon/config.h). Guarded anyway
    // so a future config path can't silently register nothing.
    std::cerr << "[register] no FUNCs configured to register; skipping\n";
    return false;
  }

  const brscan::Status status = brscan::scand::RegisterDestinations(
      registrar, registrations, local_ip, listen_port, name,
      brscan::scand::kDefaultRegistrationDurationSec, next_request_id,
      [](const std::string& func, brscan::Status result) {
        std::cout << "[register] FUNC=" << func << " -> "
                   << (result == brscan::Status::kOk ? "sent" : "failed")
                   << "\n";
      });
  return status == brscan::Status::kOk;
}

void PrintUsage(const char* argv0) {
  std::cout << "Usage: " << argv0 << " [--config PATH]\n"
             << "\n"
             << "Runs the Scan-button daemon: registers with the printer\n"
             << "named in the config, listens for button presses on UDP "
             << brscan::scand::ButtonListener::kDefaultPort << ", and "
                "saves each scan.\n"
             << "\n"
             << "  --config PATH   Config file (default "
             << brscan::scand::DefaultConfigPath() << ").\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = brscan::scand::DefaultConfigPath();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        std::cerr << "--config requires a value\n";
        return 2;
      }
      config_path = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unrecognized argument: " << arg << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }

  // Not const: a SIGHUP reload (see below) swaps this in place at a safe
  // point in the loop.
  brscan::scand::Config cfg = brscan::scand::LoadConfig(config_path);
  if (cfg.printer_host.empty()) {
    // No built-in default on purpose -- see config.h's kDefaultPrinterHost
    // comment: every printer's mDNS name is device-specific, so there is
    // no safe fallback to ship.
    std::cerr << "printer_host is not configured. Find your printer with:\n"
                  "  dns-sd -B _scanner._tcp\n"
                  "then add a line like:\n"
                  "  printer_host=BRWxxxxxxxxxxxx.local\n"
                  "to "
               << config_path << "\n";
    return 1;
  }

  std::error_code ec;
  std::filesystem::create_directories(cfg.save_dir, ec);
  if (ec) {
    std::cerr << "warning: could not create save_dir '" << cfg.save_dir
               << "': " << ec.message() << "\n";
  }

  std::cout << "brscan-scand starting: printer_host=" << cfg.printer_host
             << " display_name=" << cfg.display_name
             << " save_dir=" << cfg.save_dir << "\n";

  brscan::scand::ButtonListener listener;
  if (listener.Bind() != brscan::Status::kOk) {
    std::cerr << "could not bind UDP port "
               << brscan::scand::ButtonListener::kDefaultPort
               << " (already running, or another process is using it?)\n";
    return 1;
  }

  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);
  std::signal(SIGHUP, HandleReloadSignal);

  uint32_t request_id = 1;
  // A deadline already in the past forces the first loop iteration to
  // register immediately, before waiting on anything.
  auto next_register = std::chrono::steady_clock::now();

  // Registers destinations over SNMP, caching the printer's resolved SNMP
  // address so the daemon no longer re-resolves printer_host on every Set /
  // cycle (GitHub #22). Rebuilt if a SIGHUP reload changes printer_host.
  brscan::scand::SnmpRegistrar registrar(cfg.printer_host, kSnmpCommunity);

  // Numeric IP(s) `cfg.printer_host` resolves to. Used to drop a
  // notification whose UDP sender doesn't match the real printer -- defense
  // in depth against a forged notification from elsewhere on the LAN; see
  // daemon/sender_check.h. Starts empty (unresolved), which IsAllowedSender()
  // treats as "check unavailable, allow" until the first registration cycle
  // resolves it.
  std::vector<std::string> allowed_sender_ips;

  // This Mac's address to advertise in HOST=, resolved via LocalIpForPeer.
  // Cached across cycles like allowed_sender_ips: both call getaddrinfo (an
  // mDNS query for a `.local` host), so they are refreshed only when
  // `needs_resolve` is set -- on the first cycle, after a registration send
  // fails (the printer's IP may have changed), after a config reload, and at
  // most every kResolveRefreshSec (see above) as a slow staleness guard --
  // rather than on every 300 s cycle (GitHub #22).
  std::optional<std::string> cached_local_ip;
  bool needs_resolve = true;
  auto next_resolve_refresh = std::chrono::steady_clock::now();

  // The printer retransmits a button notification until it's satisfied the
  // press was consumed, so one press arrives as several identical datagrams
  // (observed on hardware: one press, two scans). Each copy is still ACKed
  // below -- the ACK is what stops the retransmits -- but only the first is
  // scanned. See daemon/notification_deduper.h.
  brscan::scand::NotificationDeduper deduper;

  while (g_stop_requested == 0) {
    // Note on signal responsiveness: g_stop_requested is only consulted
    // here, at each iteration of this loop. A SIGINT/SIGTERM that arrives
    // while a scan is already in flight (HandleButtonEvent -> RunScan,
    // below) is not acted on until that call returns: TcpTransport::Read
    // bounds each individual recv() with SO_RCVTIMEO, but its own EINTR
    // retry loop (libbrscan/transport_tcp.cpp) re-blocks for whatever of
    // that read's timeout remains rather than consulting this flag, so
    // shutdown during an active scan waits for the scan to finish (or
    // time out on its own) rather than being instant. Only the *idle*
    // wait in listener.Receive() below is bounded by kMaxReceiveWaitMs
    // specifically to keep shutdown prompt outside of an active scan.
    //
    // @autoreleasepool (defense in depth for the autorelease leak fix): the
    // post-scan image pipeline decodes color pages through an autoreleased
    // NSData (output/action_ocr.mm), and this daemon has no NSRunLoop draining
    // an ambient pool. The per-page decode sites already wrap their own pools,
    // but wrapping each loop iteration here drains any other stray autoreleased
    // object created anywhere under HandleButtonEvent, so nothing accumulates
    // across the daemon's lifetime.
    @autoreleasepool {
    try {
      // Config reload (SIGHUP): checked here, at the very top of each loop
      // iteration -- the same safe point g_stop_requested is checked at,
      // and for the same reason (see the note above): a scan already in
      // flight below (HandleButtonEvent -> RunScan) runs to completion
      // before this is consulted again, so a reload never lands mid-scan.
      if (g_reload_requested != 0) {
        g_reload_requested = 0;
        if (const auto reloaded = brscan::scand::TryReloadConfig(config_path)) {
          cfg = *reloaded;
          std::error_code reload_ec;
          std::filesystem::create_directories(cfg.save_dir, reload_ec);
          if (reload_ec) {
            std::cerr << "warning: could not create save_dir '" << cfg.save_dir
                       << "': " << reload_ec.message() << "\n";
          }
          std::cout << "[config] reloaded " << config_path << "\n";
          // A changed printer_host means the cached registrar and resolved
          // addresses are for the wrong device: rebuild the registrar and
          // force a fresh resolve next cycle.
          if (registrar.printer_host() != cfg.printer_host) {
            registrar =
                brscan::scand::SnmpRegistrar(cfg.printer_host, kSnmpCommunity);
          }
          needs_resolve = true;
          // Force an immediate re-register rather than waiting up to
          // kReregisterIntervalSec: a changed display_name or printer_host
          // should take effect right away, and registration is
          // idempotent/safe to call early.
          next_register = std::chrono::steady_clock::now();
        } else {
          std::cerr << "[config] reload failed: " << config_path
                     << " has no usable printer_host, keeping previous "
                        "settings\n";
        }
      }

      const auto now = std::chrono::steady_clock::now();
      if (now >= next_register) {
        // Refresh the cached resolutions only when due (see needs_resolve /
        // kResolveRefreshSec above), not on every cycle -- this is what
        // eliminates the steady-state mDNS chatter (GitHub #22).
        if (needs_resolve || now >= next_resolve_refresh) {
          registrar.InvalidateCache();
          cached_local_ip = LocalIpForPeer(cfg.printer_host, kSnmpProbePort);
          allowed_sender_ips = brscan::scand::ResolveHostIps(cfg.printer_host);
          if (allowed_sender_ips.empty()) {
            std::cerr << "[listener] warning: could not resolve '"
                       << cfg.printer_host
                       << "' to verify notification senders; sender check "
                          "disabled until the next re-register\n";
          }
          next_resolve_refresh = now + std::chrono::seconds(kResolveRefreshSec);
          needs_resolve = false;
        }

        if (!cached_local_ip.has_value()) {
          std::cerr << "[register] could not determine this Mac's local "
                        "address for '"
                     << cfg.printer_host
                     << "'; skipping registration this round\n";
          needs_resolve = true;  // Retry the resolve next cycle.
        } else if (!RegisterConfiguredDestinations(registrar, cfg,
                                                    *cached_local_ip,
                                                    listener.port(),
                                                    &request_id)) {
          // A send failed: the printer's IP may have moved. Re-resolve
          // everything (including this Mac's local address) next cycle.
          needs_resolve = true;
        }
        next_register = now + std::chrono::seconds(kReregisterIntervalSec);
      }

      const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      next_register - std::chrono::steady_clock::now())
                                      .count();
      const int timeout_ms =
          remaining_ms > 0
              ? static_cast<int>(std::min<long long>(remaining_ms, kMaxReceiveWaitMs))
              : 0;

      brscan::scand::ButtonEvent event;
      std::vector<uint8_t> raw;
      sockaddr_storage from{};
      socklen_t fromlen = sizeof(from);
      const brscan::Status receive_status =
          listener.Receive(timeout_ms, &event, &raw, &from, &fromlen);

      if (g_stop_requested != 0) break;

      if (receive_status == brscan::Status::kTimeout) {
        continue;  // Nothing arrived before the next re-register is due.
      }
      if (receive_status == brscan::Status::kIoError) {
        std::cerr << "[listener] I/O error waiting for a notification\n";
        continue;
      }
      if (receive_status == brscan::Status::kProtocolError) {
        std::cerr << "[listener] received a datagram that did not parse as a "
                      "button notification; ignoring\n";
        continue;
      }

      // receive_status == Status::kOk: a well-formed notification.
      // Defense in depth: drop it (no ACK, no processing) unless its UDP
      // sender matches the printer we resolved above -- see
      // daemon/sender_check.h. This is on top of, not instead of,
      // HandleButtonEvent's own FUNC/path validation below.
      const auto sender_ip = brscan::scand::AddressToString(from);
      if (!brscan::scand::IsAllowedSender(allowed_sender_ips, sender_ip)) {
        std::cerr << "[listener] dropping notification from unexpected "
                      "sender "
                   << (sender_ip.has_value() ? *sender_ip : std::string("<unknown>"))
                   << " (expected " << cfg.printer_host << ")\n";
        continue;
      }

      std::cout << "[listener] button press: FUNC=" << brscan::scand::LogSafe(event.func)
                 << " user=" << brscan::scand::LogSafe(event.user) << " seq=" << event.seq
                 << "\n";
      // ACK first (even for a duplicate): the byte-for-byte echo is what
      // tells the printer to stop retransmitting this notification.
      if (listener.Ack(raw, from, fromlen) != brscan::Status::kOk) {
        std::cerr << "[listener] failed to ACK the notification (continuing "
                      "anyway)\n";
      }

      // A retransmit of a press already handled is ACKed above but not
      // scanned again -- see daemon/notification_deduper.h.
      if (deduper.IsDuplicate(event)) {
        std::cout << "[listener] duplicate notification (REGID="
                   << brscan::scand::LogSafe(event.regid) << " SEQ=" << event.seq
                   << "); ACKed, not re-scanning\n";
        continue;
      }

      brscan::TcpTransport transport(cfg.printer_host, kScanPort);
      const brscan::Status connect_status = transport.Connect();
      if (connect_status != brscan::Status::kOk) {
        std::cerr << "[scan] could not connect to " << cfg.printer_host << ":"
                   << kScanPort << ": "
                   << brscan::output::DescribeFailure(connect_status) << "\n";
        continue;
      }

      std::string saved_path;
      const brscan::Status handled =
          brscan::scand::HandleButtonEvent(event, cfg, transport, &saved_path);
      transport.Disconnect();

      if (handled != brscan::Status::kOk) {
        std::cerr << "[scan] FUNC=" << brscan::scand::LogSafe(event.func) << ": "
                   << brscan::output::DescribeFailure(handled) << "\n";
      }
    } catch (const std::exception& e) {
      // Cheap exception-safety net for a long-running daemon: an
      // unexpected exception (e.g. std::bad_alloc, a std::filesystem
      // error thrown from the non-error_code overloads) shouldn't take
      // the whole process down when the loop can just log it and move on
      // to the next notification.
      std::cerr << "[loop] unexpected exception: " << e.what()
                 << "; continuing\n";
    } catch (...) {
      std::cerr << "[loop] unexpected exception of unknown type; continuing\n";
    }
    }  // @autoreleasepool
  }

  std::cout << "brscan-scand: exiting\n";
  return 0;
}
