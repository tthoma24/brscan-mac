#pragma once

#include <deque>
#include <string>
#include <vector>

#include "transport.h"

namespace brscan {

class FakeTransport : public Transport {
 public:
  void QueueRead(const std::string& bytes) {
    reads_.emplace_back(bytes.begin(), bytes.end());
  }
  void QueueRead(const std::vector<uint8_t>& bytes) { reads_.push_back(bytes); }

  Status Connect() override { return Status::kOk; }
  void Disconnect() override {}

  Status Write(const uint8_t* buf, size_t len) override {
    written_.insert(written_.end(), buf, buf + len);
    return Status::kOk;
  }

  Status Read(uint8_t* buf, size_t cap, size_t* out_len, int) override {
    if (reads_.empty()) return Status::kTimeout;
    const std::vector<uint8_t>& front = reads_.front();
    const size_t n = front.size() < cap ? front.size() : cap;
    for (size_t i = 0; i < n; ++i) buf[i] = front[i];
    *out_len = n;
    reads_.pop_front();
    return Status::kOk;
  }

  const std::vector<uint8_t>& written() const { return written_; }

 private:
  std::deque<std::vector<uint8_t>> reads_;
  std::vector<uint8_t> written_;
};

}  // namespace brscan
