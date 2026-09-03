#pragma once

#include <cstddef>
#include <deque>
#include <string>

#include "button_listener.h"

// De-duplication for repeated Scan-button notifications. Brother's printer
// retransmits the same button-press notification (identical REGID and SEQ)
// until it is satisfied the press was consumed, so a single press arrives as
// several identical datagrams. Observed on real hardware: one OCR press
// produced two full scans seconds apart. The daemon must still ACK every
// copy (the ACK is what tells the printer to stop resending), but it must
// scan only once per distinct press -- see reference/protocol-notes-button.md.
namespace brscan::scand {

// Remembers the identity of recently handled notifications so a retransmitted
// press is recognized. Not thread-safe: the daemon's single event loop is the
// only caller.
class NotificationDeduper {
 public:
  // `capacity` is how many recent distinct notifications to remember. The
  // default comfortably covers a printer's burst of retransmits for one
  // press while staying small; older identities age out so a long-running
  // daemon never grows unbounded (and SEQ, which increments per press, keeps
  // later presses distinct from anything still remembered).
  explicit NotificationDeduper(size_t capacity = 16) : capacity_(capacity) {}

  // Records `event` as handled and reports whether it is a repeat. Returns
  // false the first time a given (REGID, SEQ) pair is seen -- the caller
  // should scan -- and true for any later copy within the remembered window,
  // which the caller should ACK but not re-scan.
  bool IsDuplicate(const ButtonEvent& event);

 private:
  static std::string KeyFor(const ButtonEvent& event);

  size_t capacity_;
  std::deque<std::string> recent_;
};

}  // namespace brscan::scand
