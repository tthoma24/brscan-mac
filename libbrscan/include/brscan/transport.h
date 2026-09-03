#pragma once

#include <cstddef>
#include <cstdint>

#include "brscan/types.h"

namespace brscan {

class Transport {
 public:
  virtual ~Transport() = default;
  virtual Status Connect() = 0;
  virtual void Disconnect() = 0;
  virtual Status Write(const uint8_t* buf, size_t len) = 0;
  virtual Status Read(uint8_t* buf, size_t cap, size_t* out_len,
                      int timeout_ms) = 0;
};

}  // namespace brscan
