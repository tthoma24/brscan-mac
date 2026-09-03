#include "sender_check.h"

#include <algorithm>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

namespace brscan::scand {

std::optional<std::string> AddressToString(const sockaddr_storage& addr) {
  char buf[INET6_ADDRSTRLEN] = {0};
  const void* addr_ptr = nullptr;

  if (addr.ss_family == AF_INET) {
    addr_ptr = &reinterpret_cast<const sockaddr_in*>(&addr)->sin_addr;
  } else if (addr.ss_family == AF_INET6) {
    addr_ptr = &reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_addr;
  } else {
    return std::nullopt;
  }

  if (inet_ntop(addr.ss_family, addr_ptr, buf, sizeof(buf)) == nullptr) {
    return std::nullopt;
  }
  return std::string(buf);
}

std::vector<std::string> ResolveHostIps(const std::string& host) {
  std::vector<std::string> ips;

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  addrinfo* results = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 ||
      results == nullptr) {
    return ips;
  }

  for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_addrlen == 0 ||
        static_cast<size_t>(ai->ai_addrlen) > sizeof(sockaddr_storage)) {
      continue;
    }
    sockaddr_storage storage{};
    std::memcpy(&storage, ai->ai_addr, ai->ai_addrlen);
    if (const auto ip = AddressToString(storage)) {
      if (std::find(ips.begin(), ips.end(), *ip) == ips.end()) {
        ips.push_back(*ip);
      }
    }
  }

  freeaddrinfo(results);
  return ips;
}

bool IsAllowedSender(const std::vector<std::string>& allowed_ips,
                      const std::optional<std::string>& sender_ip) {
  if (allowed_ips.empty()) return true;  // Resolution unavailable: fail open.
  if (!sender_ip.has_value()) return false;
  return std::find(allowed_ips.begin(), allowed_ips.end(), *sender_ip) !=
         allowed_ips.end();
}

}  // namespace brscan::scand
