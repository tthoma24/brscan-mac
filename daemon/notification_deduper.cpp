#include "notification_deduper.h"

namespace brscan::scand {

std::string NotificationDeduper::KeyFor(const ButtonEvent& event) {
  // REGID identifies the press and stays constant across its retransmits;
  // SEQ (which increments per press) is folded in as well so two distinct
  // presses can never collide even if a REGID were ever reused. The 0x1f
  // unit separator can't appear in either field, so the join is unambiguous.
  return event.regid + '\x1f' + std::to_string(event.seq);
}

bool NotificationDeduper::IsDuplicate(const ButtonEvent& event) {
  const std::string key = KeyFor(event);
  for (const std::string& seen : recent_) {
    if (seen == key) return true;
  }
  recent_.push_back(key);
  if (recent_.size() > capacity_) recent_.pop_front();
  return false;
}

}  // namespace brscan::scand
