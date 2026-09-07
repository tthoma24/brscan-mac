// Tests for the FUNC dispatch point (daemon/actions.h). FILE and the
// unimplemented-FUNC/unrecognized-FUNC fallbacks are pinned as before;
// IMAGE and EMAIL are exercised here with a fake CommandRunner so no real
// `open` or Mail.app process is ever spawned. OCR's real Vision/PDF path
// is covered separately in tests/action_ocr_test.mm, since it needs
// Objective-C++ to generate its synthetic input image and to read the
// resulting PDF back with PDFKit.

#include "actions.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

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
// several files; IMAGE opens every one of them, in order, via a single
// `open` invocation -- matching the manufacturer driver's Scan-to-Image,
// which opens every per-page JPEG it produces.
TEST(PerformActionImageTest, MultiFileWrittenOpensAllInASingleInvocation) {
  Config cfg = DefaultConfig();
  RecordingRunner runner;

  const Status status = PerformAction(
      "IMAGE", {"/tmp/scan-001.jpg", "/tmp/scan-002.jpg"}, cfg,
      std::ref(runner));

  EXPECT_EQ(status, Status::kOk);
  ASSERT_EQ(runner.calls().size(), 1u);
  const std::vector<std::string> want = {"/usr/bin/open", "/tmp/scan-001.jpg",
                                          "/tmp/scan-002.jpg"};
  EXPECT_EQ(runner.calls()[0], want);
}

// Same multi-file case, but with a configured `image_app` -- the `-a app`
// flag must precede the file list, and every file must still be present.
TEST(PerformActionImageTest, MultiFileWithConfiguredAppAddsDashABeforeFiles) {
  Config cfg = DefaultConfig();
  cfg.image_app = "Preview";
  RecordingRunner runner;

  const Status status = PerformAction(
      "IMAGE", {"/tmp/scan-001.jpg", "/tmp/scan-002.jpg", "/tmp/scan-003.jpg"},
      cfg, std::ref(runner));

  EXPECT_EQ(status, Status::kOk);
  ASSERT_EQ(runner.calls().size(), 1u);
  const std::vector<std::string> want = {
      "/usr/bin/open", "-a", "Preview", "/tmp/scan-001.jpg",
      "/tmp/scan-002.jpg", "/tmp/scan-003.jpg"};
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

// A CommandRunner that, when invoked, records whether every path it was
// told to watch still existed on disk at that moment, then returns a fixed
// exit status. Used to prove PerformAction's EMAIL branch attaches the
// file(s) (runs the AppleScript) *before* removing them -- so they must all
// be present when the runner runs, and gone only afterward.
class ExistenceCheckingRunner {
 public:
  explicit ExistenceCheckingRunner(int exit_status = 0)
      : exit_status_(exit_status) {}

  int operator()(const std::vector<std::string>& argv) {
    calls_.push_back(argv);
    all_existed_at_call_ = true;
    for (const std::string& path : watched_) {
      if (!std::filesystem::exists(path)) all_existed_at_call_ = false;
    }
    return exit_status_;
  }

  void Watch(std::vector<std::string> paths) { watched_ = std::move(paths); }
  bool all_existed_at_call() const { return all_existed_at_call_; }
  const std::vector<std::vector<std::string>>& calls() const { return calls_; }

 private:
  int exit_status_;
  std::vector<std::string> watched_;
  bool all_existed_at_call_ = false;
  std::vector<std::vector<std::string>> calls_;
};

// Creates `path` with a little content, asserting it now exists.
void WriteTempFile(const std::string& path) {
  std::ofstream(path) << "scan";
  ASSERT_TRUE(std::filesystem::exists(path));
}

// EMAIL leaves no persistent copy on disk (issue #20): HandleButtonEvent
// writes the scan to a temp directory, and PerformAction's EMAIL branch
// removes the file(s) once Mail has ingested them. The file(s) must still
// exist when the AppleScript runs (attach happens before removal) and be
// gone once PerformAction returns success.
TEST(PerformActionEmailTest, RemovesTempAttachmentsAfterSuccessfulAttach) {
  Config cfg = DefaultConfig();
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "brscan_actions_email_ok";
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(std::filesystem::create_directories(dir));
  const std::string f1 = (dir / "scan-doc001.pdf").string();
  const std::string f2 = (dir / "scan-doc002.pdf").string();
  WriteTempFile(f1);
  WriteTempFile(f2);

  ExistenceCheckingRunner runner;
  runner.Watch({f1, f2});
  const Status status = PerformAction("EMAIL", {f1, f2}, cfg, std::ref(runner));

  EXPECT_EQ(status, Status::kOk);
  ASSERT_EQ(runner.calls().size(), 1u);
  EXPECT_TRUE(runner.all_existed_at_call())
      << "attachments must still exist when the AppleScript runs";
  EXPECT_FALSE(std::filesystem::exists(f1))
      << "EMAIL must remove its temp attachment after a successful attach";
  EXPECT_FALSE(std::filesystem::exists(f2))
      << "EMAIL must remove its temp attachment after a successful attach";

  std::filesystem::remove_all(dir);
}

// If the attach fails (osascript non-zero), the scan must be kept rather
// than silently dropped, and the error must propagate.
TEST(PerformActionEmailTest, KeepsTempAttachmentsWhenAttachFails) {
  Config cfg = DefaultConfig();
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "brscan_actions_email_fail";
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(std::filesystem::create_directories(dir));
  const std::string f1 = (dir / "scan.pdf").string();
  WriteTempFile(f1);

  ExistenceCheckingRunner runner(/*exit_status=*/1);
  const Status status = PerformAction("EMAIL", {f1}, cfg, std::ref(runner));

  EXPECT_EQ(status, Status::kIoError);
  EXPECT_TRUE(std::filesystem::exists(f1))
      << "a failed EMAIL attach must keep the scan, not drop it";

  std::filesystem::remove_all(dir);
}

// The counterpart to the EMAIL removal above: FILE/IMAGE/OCR are the
// destinations whose written file *is* the deliverable, so PerformAction
// must never remove it. (IMAGE's `open` "succeeds" through the fake runner;
// FILE and OCR never touch the runner at all.)
TEST(PerformActionTest, NonEmailFuncsLeaveTheirFileInPlace) {
  Config cfg = DefaultConfig();
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "brscan_actions_nonemail";
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(std::filesystem::create_directories(dir));
  const std::string file = (dir / "scan.jpg").string();

  for (const char* func : {"FILE", "IMAGE", "OCR"}) {
    WriteTempFile(file);
    RecordingRunner runner;  // returns 0: IMAGE's `open` "succeeds".
    const Status status = PerformAction(func, {file}, cfg, std::ref(runner));
    EXPECT_EQ(status, Status::kOk) << func;
    EXPECT_TRUE(std::filesystem::exists(file))
        << func << " must not remove its saved file";
  }

  std::filesystem::remove_all(dir);
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
