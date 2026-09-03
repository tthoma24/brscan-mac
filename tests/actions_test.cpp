// Tests for the FUNC dispatch point (daemon/actions.h). FILE and the
// unimplemented-FUNC/unrecognized-FUNC fallbacks are pinned as before;
// IMAGE and EMAIL are exercised here with a fake CommandRunner so no real
// `open` or Mail.app process is ever spawned. OCR's real Vision/PDF path
// is covered separately in tests/action_ocr_test.mm, since it needs
// Objective-C++ to generate its synthetic input image and to read the
// resulting PDF back with PDFKit.

#include "actions.h"

#include <gtest/gtest.h>

#include "config.h"

namespace brscan::scand {
namespace {

TEST(PerformActionTest, FileReturnsOk) {
  const Config cfg = DefaultConfig();
  EXPECT_EQ(PerformAction("FILE", {"/tmp/whatever.jpg"}, cfg), Status::kOk);
}

TEST(PerformActionTest, UnrecognizedFuncIsTreatedAsNoOp) {
  const Config cfg = DefaultConfig();
  EXPECT_EQ(PerformAction("BOGUS", {"/tmp/whatever.jpg"}, cfg), Status::kOk);
}

// A CommandRunner that records every argv it's called with and returns a
// fixed exit status, without running anything.
class RecordingRunner {
 public:
  explicit RecordingRunner(int exit_status = 0) : exit_status_(exit_status) {}

  int operator()(const std::vector<std::string>& argv) {
    calls_.push_back(argv);
    return exit_status_;
  }

  const std::vector<std::vector<std::string>>& calls() const {
    return calls_;
  }

 private:
  int exit_status_;
  std::vector<std::vector<std::string>> calls_;
};

TEST(PerformActionImageTest, DefaultAppUsesPlainOpen) {
  Config cfg = DefaultConfig();
  ASSERT_TRUE(cfg.image_app.empty());
  RecordingRunner runner;

  const Status status =
      PerformAction("IMAGE", {"/tmp/scan.jpg"}, cfg, std::ref(runner));

  EXPECT_EQ(status, Status::kOk);
  ASSERT_EQ(runner.calls().size(), 1u);
  const std::vector<std::string> want = {"/usr/bin/open", "/tmp/scan.jpg"};
  EXPECT_EQ(runner.calls()[0], want);
}

TEST(PerformActionImageTest, ConfiguredAppAddsDashA) {
  Config cfg = DefaultConfig();
  cfg.image_app = "Preview";
  RecordingRunner runner;

  const Status status =
      PerformAction("IMAGE", {"/tmp/scan.jpg"}, cfg, std::ref(runner));

  EXPECT_EQ(status, Status::kOk);
  ASSERT_EQ(runner.calls().size(), 1u);
  const std::vector<std::string> want = {"/usr/bin/open", "-a", "Preview",
                                          "/tmp/scan.jpg"};
  EXPECT_EQ(runner.calls()[0], want);
}

// A multi-page write (e.g. per-page native/JPEG/PNG output) produces
// several files; IMAGE opens only the first one.
TEST(PerformActionImageTest, MultiFileWrittenOpensOnlyTheFirst) {
  Config cfg = DefaultConfig();
  RecordingRunner runner;

  const Status status = PerformAction(
      "IMAGE", {"/tmp/scan-001.jpg", "/tmp/scan-002.jpg"}, cfg,
      std::ref(runner));

  EXPECT_EQ(status, Status::kOk);
  ASSERT_EQ(runner.calls().size(), 1u);
  const std::vector<std::string> want = {"/usr/bin/open", "/tmp/scan-001.jpg"};
  EXPECT_EQ(runner.calls()[0], want);
}

TEST(PerformActionImageTest, NonzeroExitIsIoError) {
  Config cfg = DefaultConfig();
  RecordingRunner runner(/*exit_status=*/1);

  EXPECT_EQ(PerformAction("IMAGE", {"/tmp/scan.jpg"}, cfg, std::ref(runner)),
            Status::kIoError);
}

TEST(PerformActionOcrTest, NoOpAndDoesNotTouchRunner) {
  // OCR's searchable PDF is already produced upstream by
  // WriteConfiguredOutput (see daemon/handle_event.cpp); PerformAction's
  // OCR branch must be a pure log-and-return, never touching the runner.
  Config cfg = DefaultConfig();
  RecordingRunner runner;

  const Status status =
      PerformAction("OCR", {"/tmp/scan.pdf"}, cfg, std::ref(runner));

  EXPECT_EQ(status, Status::kOk);
  EXPECT_TRUE(runner.calls().empty());
}

TEST(PerformActionEmailTest, ArgvIsOsascriptDashE) {
  Config cfg = DefaultConfig();
  RecordingRunner runner;

  const Status status =
      PerformAction("EMAIL", {"/tmp/scan.jpg"}, cfg, std::ref(runner));

  EXPECT_EQ(status, Status::kOk);
  ASSERT_EQ(runner.calls().size(), 1u);
  const std::vector<std::string>& argv = runner.calls()[0];
  ASSERT_EQ(argv.size(), 3u);
  EXPECT_EQ(argv[0], "/usr/bin/osascript");
  EXPECT_EQ(argv[1], "-e");
  // argv[2] is the AppleScript text, checked in detail below.
}

TEST(PerformActionEmailTest, ScriptComposesAndAttachesWithoutSending) {
  Config cfg = DefaultConfig();
  RecordingRunner runner;

  PerformAction("EMAIL", {"/tmp/scan.jpg"}, cfg, std::ref(runner));

  ASSERT_EQ(runner.calls().size(), 1u);
  const std::string& script = runner.calls()[0][2];

  EXPECT_NE(script.find("tell application \"Mail\""), std::string::npos);
  EXPECT_NE(script.find("make new outgoing message"), std::string::npos);
  EXPECT_NE(script.find("make new attachment"), std::string::npos);
  EXPECT_NE(script.find("POSIX file \"/tmp/scan.jpg\""), std::string::npos);
  EXPECT_NE(script.find("activate"), std::string::npos);

  // The message must never be sent automatically -- no `send` statement
  // anywhere in the script.
  EXPECT_EQ(script.find("send"), std::string::npos);
}

// A multi-page/every:N write produces several files; EMAIL must attach
// every one of them, in order, and still never send the message.
TEST(PerformActionEmailTest, MultipleWrittenFilesAreAllAttached) {
  Config cfg = DefaultConfig();
  RecordingRunner runner;

  PerformAction("EMAIL", {"/tmp/scan-doc001.pdf", "/tmp/scan-doc002.pdf"}, cfg,
                std::ref(runner));

  ASSERT_EQ(runner.calls().size(), 1u);
  const std::string& script = runner.calls()[0][2];

  EXPECT_NE(script.find("POSIX file \"/tmp/scan-doc001.pdf\""),
            std::string::npos);
  EXPECT_NE(script.find("POSIX file \"/tmp/scan-doc002.pdf\""),
            std::string::npos);
  // Both attachments, and nothing else -- exactly two "make new attachment"
  // statements.
  size_t count = 0;
  size_t pos = 0;
  while ((pos = script.find("make new attachment", pos)) != std::string::npos) {
    ++count;
    pos += 1;
  }
  EXPECT_EQ(count, 2u);

  EXPECT_EQ(script.find("send"), std::string::npos);
}

TEST(PerformActionEmailTest, EscapesQuotesAndBackslashesInPath) {
  Config cfg = DefaultConfig();
  RecordingRunner runner;

  PerformAction("EMAIL", {"/tmp/weird\"path\\name.jpg"}, cfg,
                std::ref(runner));

  ASSERT_EQ(runner.calls().size(), 1u);
  const std::string& script = runner.calls()[0][2];
  EXPECT_NE(script.find("POSIX file \"/tmp/weird\\\"path\\\\name.jpg\""),
            std::string::npos);
}

TEST(PerformActionEmailTest, ConfiguredRecipientAddsToRecipient) {
  Config cfg = DefaultConfig();
  cfg.email_to = "someone@example.com";
  RecordingRunner runner;

  PerformAction("EMAIL", {"/tmp/scan.jpg"}, cfg, std::ref(runner));

  ASSERT_EQ(runner.calls().size(), 1u);
  const std::string& script = runner.calls()[0][2];
  EXPECT_NE(script.find("make new to recipient"), std::string::npos);
  EXPECT_NE(script.find("someone@example.com"), std::string::npos);
}

TEST(PerformActionEmailTest, NoRecipientConfiguredOmitsToRecipient) {
  Config cfg = DefaultConfig();
  ASSERT_TRUE(cfg.email_to.empty());
  RecordingRunner runner;

  PerformAction("EMAIL", {"/tmp/scan.jpg"}, cfg, std::ref(runner));

  ASSERT_EQ(runner.calls().size(), 1u);
  const std::string& script = runner.calls()[0][2];
  EXPECT_EQ(script.find("make new to recipient"), std::string::npos);
}

TEST(PerformActionEmailTest, NonzeroExitIsIoError) {
  Config cfg = DefaultConfig();
  RecordingRunner runner(/*exit_status=*/1);

  EXPECT_EQ(PerformAction("EMAIL", {"/tmp/scan.jpg"}, cfg, std::ref(runner)),
            Status::kIoError);
}

TEST(PerformActionTest, DefaultOverloadUsesDefaultCommandRunner) {
  // Not exercising a real `open`/Mail here -- just confirming the
  // no-runner overload compiles and dispatches through
  // DefaultCommandRunner without crashing for the FILE/no-op cases,
  // which never call the runner at all.
  const Config cfg = DefaultConfig();
  EXPECT_EQ(PerformAction("FILE", {"/tmp/whatever.jpg"}, cfg), Status::kOk);
}

}  // namespace
}  // namespace brscan::scand
