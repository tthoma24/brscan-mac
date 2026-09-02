#pragma once

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "transport.h"

namespace brscan {

class FakeTransport : public Transport {
 public:
  void QueueRead(const std::string& bytes) {
    reads_.emplace_back(std::in_place, bytes.begin(), bytes.end());
  }
  void QueueRead(const std::vector<uint8_t>& bytes) {
    reads_.emplace_back(bytes);
  }

  // Queues a single Read() call that returns Status::kTimeout, without
  // consuming any later QueueRead() entries. Models a real TCP stream
  // going quiet between two logical replies -- e.g. a multi-packet reply
  // with no self-describing length, where the caller reads until a
  // timeout signals "no more of this reply is coming" and then continues
  // the protocol with the next real reply queued after this marker.
  void QueueTimeout() { reads_.emplace_back(std::nullopt); }

  Status Connect() override { return Status::kOk; }
  void Disconnect() override {}

  Status Write(const uint8_t* buf, size_t len) override {
    written_.insert(written_.end(), buf, buf + len);
    return Status::kOk;
  }

  Status Read(uint8_t* buf, size_t cap, size_t* out_len, int) override {
    if (reads_.empty()) return Status::kTimeout;
    if (!reads_.front().has_value()) {
      reads_.pop_front();
      return Status::kTimeout;
    }
    std::vector<uint8_t>& front = *reads_.front();
    const size_t n = front.size() < cap ? front.size() : cap;
    for (size_t i = 0; i < n; ++i) buf[i] = front[i];
    *out_len = n;
    if (n == front.size()) {
      // The whole queued chunk fit in this call's buffer: consume it.
      reads_.pop_front();
    } else {
      // A queued chunk larger than the caller's buffer must be doled out
      // over several Read() calls, like a real recv() on a TCP stream
      // would -- never silently dropped. Keep the unread remainder at the
      // front of the queue for the next call.
      front.erase(front.begin(), front.begin() + n);
    }
    return Status::kOk;
  }

  const std::vector<uint8_t>& written() const { return written_; }

 private:
  std::deque<std::optional<std::vector<uint8_t>>> reads_;
  std::vector<uint8_t> written_;
};

}  // namespace brscan
