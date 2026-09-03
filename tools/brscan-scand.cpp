// brscan-scand: the Scan-button daemon. Registers this Mac with the
// printer as a destination for each of FILE/IMAGE/OCR/EMAIL, listens for
// button-press notifications on UDP 54925, and on each press ACKs it,
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
#include "notification_deduper.h"
#include "scan_output.h"
#include "sender_check.h"
#include "snmp_register.h"

namespace {

// Re-registration cadence: comfortably under the 360-second DURATION each
// registration advertises (see daemon/snmp_register.h's
// kDefaultRegistrationDurationSec), so a re-register always lands before
// the printer would let the prior one expire.
constexpr int kReregisterIntervalSec = 300;

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

// SNMP-registers every FUNC in kFuncs with the printer. Best-effort: a
// failure for one FUNC (or overall, if the local IP can't be determined)
// is logged and does not stop the daemon -- the next scheduled
// re-registration will try again.
void RegisterAllDestinations(const brscan::scand::Config& cfg,
                              uint16_t listen_port, uint32_t* next_request_id) {
  const auto local_ip = LocalIpForPeer(cfg.printer_host, kSnmpProbePort);
  if (!local_ip.has_value()) {
    std::cerr << "[register] could not determine this Mac's local address "
                  "for '"
               << cfg.printer_host << "'; skipping registration this round\n";
    return;
  }

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

  for (const FuncSpec& spec : kFuncs) {
    const std::string value = brscan::scand::BuildRegisterValue(
        *local_ip, listen_port, name, spec.func, spec.appnum,
        brscan::scand::kDefaultRegistrationDurationSec);
    const brscan::Status status = brscan::scand::SendSnmpRegister(
        cfg.printer_host, kSnmpCommunity, value, (*next_request_id)++);
    std::cout << "[register] FUNC=" << spec.func << " -> "
               << (status == brscan::Status::kOk ? "sent" : "failed") << "\n";
  }
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

  const brscan::scand::Config cfg = brscan::scand::LoadConfig(config_path);
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

  uint32_t request_id = 1;
  // A deadline already in the past forces the first loop iteration to
  // register immediately, before waiting on anything.
  auto next_register = std::chrono::steady_clock::now();

  // Numeric IP(s) `cfg.printer_host` resolves to, refreshed alongside
  // registration (see below). Used to drop a notification whose UDP
  // sender doesn't match the real printer -- defense in depth against a
  // forged notification from elsewhere on the LAN; see
  // daemon/sender_check.h. Starts empty (unresolved), which
  // IsAllowedSender() treats as "check unavailable, allow" until the
  // first registration cycle resolves it.
  std::vector<std::string> allowed_sender_ips;

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
    try {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_register) {
        RegisterAllDestinations(cfg, listener.port(), &request_id);
        allowed_sender_ips = brscan::scand::ResolveHostIps(cfg.printer_host);
        if (allowed_sender_ips.empty()) {
          std::cerr << "[listener] warning: could not resolve '"
                     << cfg.printer_host
                     << "' to verify notification senders; sender check "
                        "disabled until the next re-register\n";
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

      std::cout << "[listener] button press: FUNC=" << event.func
                 << " user=" << event.user << " seq=" << event.seq << "\n";
      // ACK first (even for a duplicate): the byte-for-byte echo is what
      // tells the printer to stop retransmitting this notification.
      if (listener.Ack(raw, from, fromlen) != brscan::Status::kOk) {
        std::cerr << "[listener] failed to ACK the notification (continuing "
                      "anyway)\n";
      }

      // A retransmit of a press already handled is ACKed above but not
      // scanned again -- see daemon/notification_deduper.h.
      if (deduper.IsDuplicate(event)) {
        std::cout << "[listener] duplicate notification (REGID=" << event.regid
                   << " SEQ=" << event.seq
                   << "); ACKed, not re-scanning\n";
        continue;
      }

      brscan::TcpTransport transport(cfg.printer_host, kScanPort);
      const brscan::Status connect_status = transport.Connect();
      if (connect_status != brscan::Status::kOk) {
        std::cerr << "[scan] could not connect to " << cfg.printer_host << ":"
                   << kScanPort << ": "
                   << brscan::cli::DescribeFailure(connect_status) << "\n";
        continue;
      }

      std::string saved_path;
      const brscan::Status handled =
          brscan::scand::HandleButtonEvent(event, cfg, transport, &saved_path);
      transport.Disconnect();

      if (handled != brscan::Status::kOk) {
        std::cerr << "[scan] FUNC=" << event.func << ": "
                   << brscan::cli::DescribeFailure(handled) << "\n";
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
  }

  std::cout << "brscan-scand: exiting\n";
  return 0;
}
