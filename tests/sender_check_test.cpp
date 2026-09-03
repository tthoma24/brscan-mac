// Tests for the sender-address defense-in-depth check (daemon/
// sender_check.h). Hermetic: ResolveHostIps is only ever exercised
// against numeric IP literals here, which getaddrinfo() resolves locally
// without any DNS/network traffic (per POSIX getaddrinfo semantics for a
// numeric host), so this never touches the network or depends on it
// being available in the test environment.

#include "sender_check.h"

#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <gtest/gtest.h>

namespace brscan::scand {
namespace {

sockaddr_storage MakeIpv4(const char* ip) {
  sockaddr_storage storage{};
  auto* addr4 = reinterpret_cast<sockaddr_in*>(&storage);
  addr4->sin_family = AF_INET;
  inet_pton(AF_INET, ip, &addr4->sin_addr);
  return storage;
}

sockaddr_storage MakeIpv6(const char* ip) {
  sockaddr_storage storage{};
  auto* addr6 = reinterpret_cast<sockaddr_in6*>(&storage);
  addr6->sin6_family = AF_INET6;
  inet_pton(AF_INET6, ip, &addr6->sin6_addr);
  return storage;
}

TEST(AddressToStringTest, FormatsIpv4) {
  const auto addr = MakeIpv4("192.0.2.10");
  const auto s = AddressToString(addr);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(*s, "192.0.2.10");
}

TEST(AddressToStringTest, FormatsIpv6) {
  const auto addr = MakeIpv6("::1");
  const auto s = AddressToString(addr);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(*s, "::1");
}

TEST(AddressToStringTest, RejectsUnknownFamily) {
  sockaddr_storage storage{};
  storage.ss_family = AF_UNSPEC;
  EXPECT_FALSE(AddressToString(storage).has_value());
}

TEST(ResolveHostIpsTest, ResolvesNumericIpv4Literal) {
  const auto ips = ResolveHostIps("192.0.2.10");
  ASSERT_EQ(ips.size(), 1u);
  EXPECT_EQ(ips[0], "192.0.2.10");
}

TEST(ResolveHostIpsTest, ResolvesLoopback) {
  const auto ips = ResolveHostIps("127.0.0.1");
  ASSERT_EQ(ips.size(), 1u);
  EXPECT_EQ(ips[0], "127.0.0.1");
}

TEST(IsAllowedSenderTest, AllowsExactMatch) {
  const std::vector<std::string> allowed = {"192.0.2.10"};
  EXPECT_TRUE(IsAllowedSender(allowed, std::optional<std::string>("192.0.2.10")));
}

TEST(IsAllowedSenderTest, RejectsMismatch) {
  const std::vector<std::string> allowed = {"192.0.2.10"};
  EXPECT_FALSE(IsAllowedSender(allowed, std::optional<std::string>("192.0.2.99")));
}

TEST(IsAllowedSenderTest, RejectsMissingSenderIpWhenAllowedListNonEmpty) {
  const std::vector<std::string> allowed = {"192.0.2.10"};
  EXPECT_FALSE(IsAllowedSender(allowed, std::nullopt));
}

TEST(IsAllowedSenderTest, FailsOpenWhenAllowedListIsEmpty) {
  // Empty allowed_ips means "resolution was unavailable" -- see
  // ResolveHostIps's doc comment -- so this check alone doesn't drop
  // anything (daemon/handle_event.cpp's FUNC/path defenses still apply
  // regardless).
  EXPECT_TRUE(IsAllowedSender({}, std::optional<std::string>("192.0.2.99")));
  EXPECT_TRUE(IsAllowedSender({}, std::nullopt));
}

}  // namespace
}  // namespace brscan::scand
