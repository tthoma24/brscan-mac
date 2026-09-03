#include "notification_deduper.h"

#include <string>

#include <gtest/gtest.h>

namespace brscan::scand {
namespace {

ButtonEvent MakeEvent(const std::string& regid, int seq) {
  ButtonEvent event;
  event.func = "OCR";
  event.user = "Test Mac";
  event.host_ip = "192.0.2.10";
  event.host_port = 54925;
  event.appnum = 3;
  event.regid = regid;
  event.seq = seq;
  return event;
}

TEST(NotificationDeduperTest, FirstOccurrenceIsNotDuplicate) {
  NotificationDeduper deduper;
  EXPECT_FALSE(deduper.IsDuplicate(MakeEvent("63700", 5)));
}

TEST(NotificationDeduperTest, RetransmitOfSamePressIsDuplicate) {
  // The exact scenario seen on hardware: the printer resends the identical
  // notification (same REGID and SEQ) for one press.
  NotificationDeduper deduper;
  const ButtonEvent press = MakeEvent("63700", 5);
  EXPECT_FALSE(deduper.IsDuplicate(press));
  EXPECT_TRUE(deduper.IsDuplicate(press));
  EXPECT_TRUE(deduper.IsDuplicate(press));  // A second retransmit, still a dup.
}

TEST(NotificationDeduperTest, DifferentSeqIsANewPress) {
  NotificationDeduper deduper;
  EXPECT_FALSE(deduper.IsDuplicate(MakeEvent("63700", 5)));
  EXPECT_FALSE(deduper.IsDuplicate(MakeEvent("63701", 6)));
}

TEST(NotificationDeduperTest, SameRegidDifferentSeqIsNotDuplicate) {
  // SEQ is folded into the identity, so even a reused REGID with a new SEQ
  // counts as a fresh press.
  NotificationDeduper deduper;
  EXPECT_FALSE(deduper.IsDuplicate(MakeEvent("63700", 5)));
  EXPECT_FALSE(deduper.IsDuplicate(MakeEvent("63700", 6)));
}

TEST(NotificationDeduperTest, IdentitiesAgeOutPastCapacity) {
  // With capacity 2, the first press is forgotten once two newer distinct
  // presses have been seen, so a much later repeat is treated as new rather
  // than remembered forever.
  NotificationDeduper deduper(2);
  const ButtonEvent first = MakeEvent("A", 1);
  EXPECT_FALSE(deduper.IsDuplicate(first));
  EXPECT_FALSE(deduper.IsDuplicate(MakeEvent("B", 2)));
  EXPECT_FALSE(deduper.IsDuplicate(MakeEvent("C", 3)));  // Evicts "A".
  EXPECT_FALSE(deduper.IsDuplicate(first));  // No longer remembered.
}

}  // namespace
}  // namespace brscan::scand
