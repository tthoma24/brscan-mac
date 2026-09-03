#pragma once

#include "brscan/transport.h"

namespace brscan {

class Session {
 public:
  explicit Session(Transport* transport) : transport_(transport) {}
  Status Open();
  void Close();

 private:
  Transport* transport_;
};

}  // namespace brscan
