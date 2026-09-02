#include "session.h"

#include <cstring>

namespace brscan {

Status Session::Open() {
  uint8_t buf[64];
  size_t got = 0;
  const Status s = transport_->Read(buf, sizeof(buf), &got, 5000);
  if (s != Status::kOk) return s;
  if (got >= 7 && std::memcmp(buf, "+OK 200", 7) == 0) return Status::kOk;
  if (got >= 7 && std::memcmp(buf, "-NG 401", 7) == 0) return Status::kBusy;
  return Status::kProtocolError;
}

void Session::Close() { transport_->Disconnect(); }

}  // namespace brscan
