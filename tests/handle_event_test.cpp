// Tests for the per-button-press pipeline (daemon/handle_event.h):
// FUNC -> RunButtonScan (PlanButtonScan decides Params/OutputSettings) ->
// save -> PerformAction. Hermetic: drives RunButtonScan over a
// brscan::FakeTransport queued with a synthetic button-flow session
// (greeting -> config frame -> offer -> block/payload -> job-final
// terminator; see tests/scanner_test.cpp's RunButtonScan tests for the
// same shape), and writes to a real temp directory (removed at the end of
// each test) rather than touching a real printer or the real 54925 UDP
// port.

#include "handle_event.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <turbojpeg.h>

#include "button_listener.h"
#include "config.h"
#include "fake_transport.h"
#include "paper_size.h"

namespace brscan::scand {
namespace {

// Since daemon/button_plan.cpp's PlanButtonScan now defaults a
// Touch-Panel-OFF (Auto) FUNC's scan area to AreaForPaper("LETTER", dpi)
// when no `<dest>.paper` is configured (see button_plan.cpp's
// kDefaultAutoPaper), the gray/IMAGE/EMAIL fixtures below -- which don't
// set image_paper/email_paper -- must supply a raw payload sized to that
// default area's height at their configured dpi (100), not the tiny
// offer-shaped height these fixtures used before that fix. This helper
// keeps that byte count and PGM header tied to the real production
// default instead of a duplicated magic literal.
int DefaultAutoAreaHeightAt(int dpi) {
  const std::optional<brscan::Area> area = AreaForPaper("LETTER", dpi);
  return area->y1 - area->y0;
}

// --- Fixture-building helpers, mirroring tests/scanner_test.cpp's -------

std::vector<uint8_t> EncodeOfferFrame(const std::string& csv) {
  std::vector<uint8_t> out;
  out.push_back(0x00);
  const uint16_t len = static_cast<uint16_t>(csv.size() + 1);
  out.push_back(static_cast<uint8_t>(len & 0xff));
  out.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
  out.insert(out.end(), csv.begin(), csv.end());
  out.push_back(0x00);
  return out;
}

std::vector<uint8_t> EncodeBlockHeader(uint16_t width) {
  return {0x00, 0x64, 0x07, 0x00, 0x01, 0x00, 0x84, 0xc0, 0x01, 0x00, 0x00,
          static_cast<uint8_t>(width & 0xff),
          static_cast<uint8_t>((width >> 8) & 0xff)};
}

// The 10-byte end-of-page marker (`82 07 00 <pidx> 00 84 00 00 00 00`; see
// reference/protocol-notes-adf-multipage.md and tests/scanner_test.cpp's
// RunScan tests, which decode this same shape). Followed by either the
// next page's block header (more pages) or `80 80` (job-final -- see
// EncodeJobFinalTerminator below).
std::vector<uint8_t> EncodeEndOfPageMarker(uint8_t pidx) {
  return {0x82, 0x07, 0x00, pidx, 0x00, 0x84, 0x00, 0x00, 0x00, 0x00};
}

// The 12-byte job-final terminator every single-page scan now ends with:
// EncodeEndOfPageMarker(pidx) followed by `80 80`.
std::vector<uint8_t> EncodeJobFinalTerminator(uint8_t pidx) {
  std::vector<uint8_t> out = EncodeEndOfPageMarker(pidx);
  out.push_back(0x80);
  out.push_back(0x80);
  return out;
}

std::vector<uint8_t> MakeSyntheticJpeg(int width, int height) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3, 128);
  tjhandle handle = tjInitCompress();
  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;
  const int rc = tjCompress2(handle, rgb.data(), width, 0, height, TJPF_RGB,
                              &jpeg_buf, &jpeg_size, TJSAMP_444, 90,
                              TJFLAG_ACCURATEDCT);
  EXPECT_EQ(rc, 0);
  tjDestroy(handle);
  std::vector<uint8_t> out(jpeg_buf, jpeg_buf + jpeg_size);
  tjFree(jpeg_buf);
  return out;
}

// The button-flow greeting only -- no ESC Q reply, no source-select ack
// (the button flow sends neither ESC Q nor ESC S/ESC D; see
// brscan/scanner.h's RunButtonScan and tests/scanner_test.cpp's
// QueueButtonGreeting, which this mirrors).
void QueueButtonGreeting(brscan::FakeTransport* t) {
  t->QueueRead(std::string("+OK 200\r\n"));
}

// The config-command frame the printer pushes right after ESC K: `0x30
// <len> 0x00` then the KEY=VALUE payload (see daemon/button_config.h and
// tests/scanner_test.cpp's EncodeButtonConfigFrame, which this mirrors).
std::vector<uint8_t> EncodeButtonConfigFrame(const std::string& payload) {
  std::vector<uint8_t> out;
  out.push_back(0x30);
  out.push_back(static_cast<uint8_t>(payload.size()));
  out.push_back(0x00);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// The short "Auto" form config payload (Touch-Panel-OFF): only F=/D=/E=,
// no R= -- see daemon/button_plan.h's Touch-Panel-ON-detection comment.
// This is what most tests below push, so this daemon's own per-FUNC
// config (not any printer-supplied setting) drives the scan -- matching
// what these tests assert about cfg.<dest>_params/cfg.<dest>_output.
std::string ShortFormConfigPayload(const std::string& func) {
  return "F=" + func + "\nD=SIN\nE=LON\n";
}

// Queues a full button-flow session up through the offer reply: greeting,
// the short-form (Touch-Panel-OFF) config frame for `func`, then the
// offer CSV. Callers queue the block header/payload and terminator after
// this, same as the old QueuePreamble's callers did for the offer.
void QueueButtonPreamble(brscan::FakeTransport* t, const std::string& func,
                          const std::string& offer_csv) {
  QueueButtonGreeting(t);
  t->QueueRead(EncodeButtonConfigFrame(ShortFormConfigPayload(func)));
  t->QueueRead(EncodeOfferFrame(offer_csv));
}

// Builds a well-formed FUNC=`func` button event with the given `regid`.
ButtonEvent MakeEvent(const std::string& func, const std::string& regid) {
  ButtonEvent event;
  event.func = func;
  event.user = "Test Mac";
  event.host_ip = "192.0.2.10";
  event.host_port = 54925;
  event.appnum = 5;
  event.regid = regid;
  event.seq = 1;
  return event;
}

// Reads `path` as raw bytes (not a std::string) so comparisons against a
// std::vector<uint8_t> fixture don't fall prey to char/uint8_t sign
// mismatches on bytes with the high bit set (e.g. a JPEG's 0xff markers).
std::vector<uint8_t> ReadWholeFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>());
}

bool Contains(const std::vector<uint8_t>& haystack, const std::string& needle) {
  const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(),
                               needle.end());
  return it != haystack.end();
}

// True if `path`'s first 4 bytes are the PDF magic number ("%PDF"). Used to
// confirm a file WriteConfiguredOutput wrote really is a PDF, without
// needing PDFKit (this file is plain C++, not Objective-C++ -- the deeper
// PDF content checks, e.g. page count and searchable text, already live in
// tests/output_writer_test.mm).
bool StartsWithPdfMagic(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  char buf[4] = {0};
  f.read(buf, sizeof(buf));
  return f.gcount() == static_cast<std::streamsize>(sizeof(buf)) &&
         std::string(buf, sizeof(buf)) == "%PDF";
}

// The number of directory entries directly under `dir` (non-recursive).
size_t CountFilesIn(const std::string& dir) {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    (void)entry;
    ++count;
  }
  return count;
}

// A fake CommandRunner for every pipeline test below. HandleButtonEvent
// ends by calling PerformAction (daemon/actions.h), whose IMAGE and EMAIL
// destinations spawn a real external process (`/usr/bin/open`,
// `/usr/bin/osascript`) unless a CommandRunner is injected. These
// pipeline tests are about FUNC-driven scan/save behavior -- which Params
// got used, what got written, path handling -- not about the destination
// action itself (that's covered in isolation, with its own fake runner,
// in tests/actions_test.cpp). Every HandleButtonEvent call below must go
// through this fake and the 5-argument overload, never the production
// (DefaultCommandRunner) overload: driving an IMAGE- or EMAIL-destination
// ButtonEvent through the production overload spawns the real command
// against a real file this test just wrote to a real temp directory --
// concretely, `open` on that file launches Preview.app for real. This bit
// once already (see ImageFuncUsesImageParamsDistinctFromFile below).
class RecordingRunner {
 public:
  int operator()(const std::vector<std::string>& argv) {
    calls_.push_back(argv);
    return 0;
  }

  const std::vector<std::vector<std::string>>& calls() const {
    return calls_;
  }

 private:
  std::vector<std::vector<std::string>> calls_;
};

class HandleButtonEventTest : public ::testing::Test {
 protected:
  void SetUp() override {
    save_dir_ = (std::filesystem::temp_directory_path() /
                 "brscan_scand_handle_event_test")
                    .string();
    std::filesystem::remove_all(save_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(save_dir_); }

  std::string save_dir_;
  RecordingRunner runner_;
};

TEST_F(HandleButtonEventTest, FileFuncUsesFileParamsAndSavesJpeg) {
  brscan::FakeTransport t;
  QueueButtonPreamble(&t, "FILE", "300,300,2,292,16,427,8,");

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  Config cfg = DefaultConfig();  // file_params: color, 300dpi, flatbed.
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("FILE", "1001");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  ASSERT_EQ(status, Status::kOk);
  ASSERT_FALSE(saved_path.empty());
  EXPECT_TRUE(saved_path.size() >= 4 &&
              saved_path.substr(saved_path.size() - 4) == ".jpg");
  EXPECT_NE(saved_path.find("FILE"), std::string::npos);
  EXPECT_NE(saved_path.find("1001"), std::string::npos);

  ASSERT_TRUE(std::filesystem::exists(saved_path));
  const std::vector<uint8_t> written_file = ReadWholeFile(saved_path);
  EXPECT_EQ(written_file, jpeg);

  // Confirm file_params' color/300dpi actually drove the ESC I/ESC X
  // commands sent to the transport.
  EXPECT_TRUE(Contains(t.written(), "R=300,300"));
  EXPECT_TRUE(Contains(t.written(), "M=CGRAY"));

  // FILE is a no-op action (saving the file *is* the FILE action -- see
  // daemon/actions.h) and must never touch the runner at all.
  EXPECT_TRUE(runner_.calls().empty());
}

TEST_F(HandleButtonEventTest, ImageFuncUsesImageParamsDistinctFromFile) {
  brscan::FakeTransport t;
  // width_px=4, height_px=3 (the offer's own height_px is dead weight here
  // -- no <dest>.paper is configured below, so PlanButtonScan's
  // Touch-Panel-OFF default area, not this offer, decides how many rows
  // RunButtonScan actually reads; see DefaultAutoAreaHeightAt above).
  QueueButtonPreamble(&t, "IMAGE", "300,300,2,292,4,427,3,");

  const int height = DefaultAutoAreaHeightAt(100);
  auto payload = EncodeBlockHeader(4);
  const std::vector<uint8_t> raw(4 * static_cast<size_t>(height), 0x42);
  payload.insert(payload.end(), raw.begin(), raw.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  Config cfg = DefaultConfig();
  // Give IMAGE its own settings, distinct from FILE's color/300dpi
  // defaults, so this test can confirm HandleButtonEvent actually looked
  // up IMAGE's Params rather than always using FILE's.
  cfg.image_params.mode = brscan::ScanMode::kGray;
  cfg.image_params.x_dpi = 100;
  cfg.image_params.y_dpi = 100;
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("IMAGE", "2002");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  ASSERT_EQ(status, Status::kOk);
  EXPECT_TRUE(saved_path.size() >= 4 &&
              saved_path.substr(saved_path.size() - 4) == ".pgm");
  EXPECT_NE(saved_path.find("IMAGE"), std::string::npos);
  EXPECT_NE(saved_path.find("2002"), std::string::npos);

  ASSERT_TRUE(std::filesystem::exists(saved_path));
  const std::vector<uint8_t> written_file = ReadWholeFile(saved_path);
  const std::string want_header = "P5\n4 " + std::to_string(height) + "\n255\n";
  ASSERT_GE(written_file.size(), want_header.size() + raw.size());
  const std::vector<uint8_t> header_bytes(written_file.begin(),
                                            written_file.begin() + want_header.size());
  EXPECT_EQ(header_bytes, std::vector<uint8_t>(want_header.begin(), want_header.end()));
  const std::vector<uint8_t> payload_bytes(written_file.begin() + want_header.size(),
                                             written_file.end());
  EXPECT_EQ(payload_bytes, raw);

  // IMAGE's gray/100dpi settings, not FILE's color/300dpi ones, must have
  // driven the commands sent to the transport.
  EXPECT_TRUE(Contains(t.written(), "R=100,100"));
  EXPECT_TRUE(Contains(t.written(), "M=GRAY64"));
  EXPECT_FALSE(Contains(t.written(), "R=300,300"));

  // IMAGE's action really does invoke the runner (unlike FILE above) --
  // confirm it went through the injected fake, with the expected argv,
  // and not through a real `/usr/bin/open` process.
  ASSERT_EQ(runner_.calls().size(), 1u);
  EXPECT_EQ(runner_.calls()[0][0], "/usr/bin/open");
  EXPECT_EQ(runner_.calls()[0].back(), saved_path);
}

// Regression test for a Task 1c.1 review finding: WritePages (tools/
// scan_output.h) never writes the bare, unnumbered base path for a
// multi-page scan -- only "-001", "-002", etc. HandleButtonEvent must
// report the actual numbered page-1 file as saved_path, not a path that
// was never written to disk, and IMAGE must open every numbered page
// (see daemon/actions.h's IMAGE bullet). A 2-page synthetic ADF-shaped
// gray scan (same inter-page 10-byte marker framing as
// tests/scanner_test.cpp's GrayAdfMultiPageReturnsAllPages) drives this
// through IMAGE so the recording CommandRunner shows exactly what
// PerformAction ran against.
TEST_F(HandleButtonEventTest, ImageFuncMultiPageSavesAllPagesAndOpensBoth) {
  brscan::FakeTransport t;
  // width_px=4, height_px=3 (same offer as ImageFuncUsesImageParamsDistinctFromFile;
  // see that test's comment on why the offer's own height_px is dead
  // weight once no <dest>.paper leaves the Touch-Panel-OFF default area in
  // charge -- DefaultAutoAreaHeightAt above).
  QueueButtonPreamble(&t, "IMAGE", "300,300,2,292,4,427,3,");

  const int height = DefaultAutoAreaHeightAt(100);
  auto block1 = EncodeBlockHeader(4);
  const std::vector<uint8_t> raw1(4 * static_cast<size_t>(height), 0x11);
  block1.insert(block1.end(), raw1.begin(), raw1.end());
  t.QueueRead(block1);
  t.QueueRead(EncodeEndOfPageMarker(1));

  auto block2 = EncodeBlockHeader(4);
  const std::vector<uint8_t> raw2(4 * static_cast<size_t>(height), 0x22);
  block2.insert(block2.end(), raw2.begin(), raw2.end());
  t.QueueRead(block2);
  t.QueueRead(EncodeJobFinalTerminator(2));

  Config cfg = DefaultConfig();
  cfg.image_params.mode = brscan::ScanMode::kGray;
  cfg.image_params.x_dpi = 100;
  cfg.image_params.y_dpi = 100;
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("IMAGE", "7007");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  ASSERT_EQ(status, Status::kOk);
  ASSERT_FALSE(saved_path.empty());

  // saved_path must be the numbered page-1 file (tools/scan_output.h's
  // PagePath(base, 1, 2)), never the unnumbered base BuildOutputPath()
  // built -- that exact path is never written for a 2-page scan.
  ASSERT_NE(saved_path.find("-001."), std::string::npos)
      << "saved_path must be the numbered page-1 file, not the unwritten "
         "base path: "
      << saved_path;
  ASSERT_TRUE(std::filesystem::exists(saved_path));

  const size_t page_marker_pos = saved_path.rfind("-001");
  ASSERT_NE(page_marker_pos, std::string::npos);
  std::string page2_path = saved_path;
  page2_path.replace(page_marker_pos, 4, "-002");
  ASSERT_TRUE(std::filesystem::exists(page2_path))
      << "page 2 must also be written to disk: " << page2_path;

  // Each numbered file must hold its own page's pixel data, not a copy of
  // the other page's.
  const std::string want_header = "P5\n4 " + std::to_string(height) + "\n255\n";
  const std::vector<uint8_t> page1_file = ReadWholeFile(saved_path);
  ASSERT_GE(page1_file.size(), want_header.size() + raw1.size());
  const std::vector<uint8_t> page1_payload(
      page1_file.begin() + static_cast<long>(want_header.size()), page1_file.end());
  EXPECT_EQ(page1_payload, raw1);

  const std::vector<uint8_t> page2_file = ReadWholeFile(page2_path);
  ASSERT_GE(page2_file.size(), want_header.size() + raw2.size());
  const std::vector<uint8_t> page2_payload(
      page2_file.begin() + static_cast<long>(want_header.size()), page2_file.end());
  EXPECT_EQ(page2_payload, raw2);

  // The FUNC action (IMAGE -> `/usr/bin/open`) must run against the files
  // that actually exist on disk -- both numbered page files, in order --
  // not the never-written base path. This is the exact check that would
  // have caught the Critical: before the fix, this ran `/usr/bin/open` on
  // a nonexistent path; it now also confirms both pages, not just the
  // first, are opened.
  ASSERT_EQ(runner_.calls().size(), 1u);
  const std::vector<std::string> want = {"/usr/bin/open", saved_path,
                                          page2_path};
  EXPECT_EQ(runner_.calls()[0], want);
}

// Task 1c.2b: a FUNC configured with a non-native `<dest>.format` must
// actually produce that format end to end -- not the native per-PixelFormat
// file the pre-1c.2b flow always wrote.
TEST_F(HandleButtonEventTest, FileFuncWithPdfFormatProducesSinglePdf) {
  brscan::FakeTransport t;
  QueueButtonPreamble(&t, "FILE", "300,300,2,292,16,427,8,");

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  Config cfg = DefaultConfig();
  cfg.save_dir = save_dir_;
  cfg.file_output.format = OutputFormat::kPdf;

  const ButtonEvent event = MakeEvent("FILE", "4004");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  ASSERT_EQ(status, Status::kOk);
  ASSERT_FALSE(saved_path.empty());
  EXPECT_TRUE(saved_path.size() >= 4 &&
              saved_path.substr(saved_path.size() - 4) == ".pdf");
  ASSERT_TRUE(std::filesystem::exists(saved_path));
  EXPECT_TRUE(StartsWithPdfMagic(saved_path));

  // Exactly the one combined PDF should be on disk -- not the native .jpg
  // BuildOutputPath's page-1 extension would suggest.
  EXPECT_EQ(CountFilesIn(save_dir_), 1u);

  // FILE is still a no-op action either way.
  EXPECT_TRUE(runner_.calls().empty());
}

// OCR's OutputSettings is forced to a searchable PDF regardless of
// configuration (see daemon/handle_event.cpp); the deliverable comes from
// WriteConfiguredOutput itself, so PerformAction's OCR branch must be a
// pure no-op -- no separate OCR action (and, in particular, no runner
// invocation) on top of it.
TEST_F(HandleButtonEventTest, OcrFuncProducesSearchablePdfWithNoSeparateOcrAction) {
  brscan::FakeTransport t;
  QueueButtonPreamble(&t, "OCR", "300,300,2,292,16,427,8,");

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  Config cfg = DefaultConfig();  // ocr_output defaults to native -> promoted
                                  // to a searchable PDF for this FUNC.
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("OCR", "5005");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  ASSERT_EQ(status, Status::kOk);
  ASSERT_FALSE(saved_path.empty());
  EXPECT_TRUE(saved_path.size() >= 4 &&
              saved_path.substr(saved_path.size() - 4) == ".pdf");
  ASSERT_TRUE(std::filesystem::exists(saved_path));
  EXPECT_TRUE(StartsWithPdfMagic(saved_path));
  EXPECT_EQ(CountFilesIn(save_dir_), 1u);

  EXPECT_TRUE(runner_.calls().empty());
}

// A multi-page scan with `every:1` separation produces one document per
// page; EMAIL must attach every one of them, not just the first.
TEST_F(HandleButtonEventTest, EmailFuncWithSeparationAttachesAllProducedFiles) {
  brscan::FakeTransport t;
  // width_px=4, height_px=3 (same offer as the IMAGE multi-page test above;
  // see that test's comment on why the offer's own height_px is dead
  // weight once no <dest>.paper leaves the Touch-Panel-OFF default area in
  // charge -- DefaultAutoAreaHeightAt above).
  QueueButtonPreamble(&t, "EMAIL", "300,300,2,292,4,427,3,");

  const int height = DefaultAutoAreaHeightAt(100);
  auto block1 = EncodeBlockHeader(4);
  const std::vector<uint8_t> raw1(4 * static_cast<size_t>(height), 0x11);
  block1.insert(block1.end(), raw1.begin(), raw1.end());
  t.QueueRead(block1);
  t.QueueRead(EncodeEndOfPageMarker(1));

  auto block2 = EncodeBlockHeader(4);
  const std::vector<uint8_t> raw2(4 * static_cast<size_t>(height), 0x22);
  block2.insert(block2.end(), raw2.begin(), raw2.end());
  t.QueueRead(block2);
  t.QueueRead(EncodeJobFinalTerminator(2));

  Config cfg = DefaultConfig();
  cfg.email_params.mode = brscan::ScanMode::kGray;
  cfg.email_params.x_dpi = 100;
  cfg.email_params.y_dpi = 100;
  cfg.email_output.format = OutputFormat::kPdf;
  cfg.email_output.separation = OutputSeparation::kEveryN;
  cfg.email_output.separate_n = 1;
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("EMAIL", "8008");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  ASSERT_EQ(status, Status::kOk);
  ASSERT_FALSE(saved_path.empty());
  ASSERT_NE(saved_path.find("-doc001."), std::string::npos) << saved_path;

  std::string doc2_path = saved_path;
  const size_t marker_pos = doc2_path.rfind("-doc001");
  ASSERT_NE(marker_pos, std::string::npos);
  doc2_path.replace(marker_pos, 7, "-doc002");

  ASSERT_TRUE(std::filesystem::exists(saved_path));
  ASSERT_TRUE(std::filesystem::exists(doc2_path))
      << "the second document must also be written: " << doc2_path;
  EXPECT_EQ(CountFilesIn(save_dir_), 2u);

  ASSERT_EQ(runner_.calls().size(), 1u);
  ASSERT_EQ(runner_.calls()[0][0], "/usr/bin/osascript");
  ASSERT_EQ(runner_.calls()[0].size(), 3u);
  const std::string& script = runner_.calls()[0][2];

  EXPECT_NE(script.find(saved_path), std::string::npos)
      << "script does not mention " << saved_path << ": " << script;
  EXPECT_NE(script.find(doc2_path), std::string::npos)
      << "script does not mention " << doc2_path << ": " << script;
  EXPECT_EQ(script.find("send"), std::string::npos);
}

// Touch-Panel-ON precedence, end to end: when the printer's config frame
// is the full LCD-set form (carries R=), its own settings drive the scan
// and the output format, overriding this daemon's configured FILE
// settings entirely -- see daemon/button_plan.h.
TEST_F(HandleButtonEventTest,
       TouchPanelOnConfigFrameOverridesConfiguredParamsAndFormat) {
  brscan::FakeTransport t;
  QueueButtonGreeting(&t);
  const std::string full_lcd_set_form =
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=JPEG\nW=0\nG=0\nX=0\n";
  t.QueueRead(EncodeButtonConfigFrame(full_lcd_set_form));
  t.QueueRead(EncodeOfferFrame("300,300,2,292,16,427,8,"));

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  Config cfg = DefaultConfig();
  // Configure FILE with settings the LCD-set config frame above must
  // override: gray/100dpi Params, and a TIFF output format.
  cfg.file_params.mode = brscan::ScanMode::kGray;
  cfg.file_params.x_dpi = 100;
  cfg.file_params.y_dpi = 100;
  cfg.file_output.format = OutputFormat::kTiff;
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("FILE", "9001");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  ASSERT_EQ(status, Status::kOk);
  ASSERT_FALSE(saved_path.empty());
  // T=JPEG in the LCD-set config frame must have driven the output
  // format, overriding cfg.file_output.format == kTiff.
  EXPECT_TRUE(saved_path.size() >= 4 &&
              saved_path.substr(saved_path.size() - 4) == ".jpg")
      << saved_path;
  ASSERT_TRUE(std::filesystem::exists(saved_path));
  EXPECT_EQ(CountFilesIn(save_dir_), 1u);

  // R=300 (the LCD's own resolution), not cfg.file_params' 100dpi, must
  // have driven the ESC I/ESC X commands -- confirming the printer's own
  // settings, not the daemon's config, drove this scan.
  EXPECT_TRUE(Contains(t.written(), "R=300,300"));
  EXPECT_FALSE(Contains(t.written(), "R=100,100"));
  EXPECT_TRUE(Contains(t.written(), "M=CGRAY"));

  EXPECT_TRUE(runner_.calls().empty());
}

TEST_F(HandleButtonEventTest, ScanFailurePropagatesStatusAndSavesNothing) {
  brscan::FakeTransport t;
  t.QueueRead(std::string("-NG 401\r\n"));  // Busy greeting.

  Config cfg = DefaultConfig();
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("FILE", "3003");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  EXPECT_EQ(status, Status::kBusy);
  EXPECT_TRUE(saved_path.empty());
  // No file should have been written for a scan that never happened: the
  // save_dir is either never created, or created but left empty.
  EXPECT_TRUE(!std::filesystem::exists(save_dir_) ||
              std::filesystem::is_empty(save_dir_));
  EXPECT_TRUE(runner_.calls().empty());
}

TEST_F(HandleButtonEventTest, RejectsUnknownFuncWithoutScanning) {
  // No reads queued at all: if HandleButtonEvent tried to scan despite the
  // bad FUNC, RunScan's very first read (the greeting) would find the
  // FakeTransport's queue empty. Asserting the transport saw *nothing*
  // written confirms RunScan was never even invoked.
  brscan::FakeTransport t;

  Config cfg = DefaultConfig();
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("../../../../etc/passwd", "1234");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  EXPECT_EQ(status, Status::kProtocolError);
  EXPECT_TRUE(saved_path.empty());
  EXPECT_TRUE(t.written().empty());
  EXPECT_TRUE(!std::filesystem::exists(save_dir_) ||
              std::filesystem::is_empty(save_dir_));
  EXPECT_TRUE(runner_.calls().empty());
}

TEST_F(HandleButtonEventTest, SanitizesPathTraversalInRegidAndStaysInsideSaveDir) {
  brscan::FakeTransport t;
  QueueButtonPreamble(&t, "FILE", "300,300,2,292,16,427,8,");
  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  Config cfg = DefaultConfig();
  cfg.save_dir = save_dir_;

  // A forged REGID (straight off an untrusted UDP notification in real
  // use) trying to escape save_dir. FUNC is a valid, known value, so this
  // exercises BuildOutputPath's REGID sanitization and
  // HandleButtonEvent's save_dir-containment check specifically, distinct
  // from the FUNC-rejection test above.
  const ButtonEvent event = MakeEvent("FILE", "../../../../../../tmp/evil");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  ASSERT_EQ(status, Status::kOk);
  ASSERT_FALSE(saved_path.empty());

  // The write must land strictly inside save_dir_, never at the forged
  // REGID's literal target.
  const auto canonical_save_dir = std::filesystem::weakly_canonical(save_dir_);
  const auto canonical_saved_path = std::filesystem::weakly_canonical(saved_path);
  EXPECT_EQ(canonical_saved_path.parent_path(), canonical_save_dir);

  const std::string filename = canonical_saved_path.filename().string();
  EXPECT_EQ(filename.find('/'), std::string::npos);
  EXPECT_EQ(filename.find(".."), std::string::npos);

  EXPECT_FALSE(std::filesystem::exists("/tmp/evil"));
  EXPECT_TRUE(runner_.calls().empty());
}

TEST_F(HandleButtonEventTest, UnwritableSaveDirReturnsIoErrorNotCrash) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "running as root: permission bits don't block writes";
  }

  brscan::FakeTransport t;
  QueueButtonPreamble(&t, "FILE", "300,300,2,292,16,427,8,");
  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);
  t.QueueRead(EncodeJobFinalTerminator(1));

  ASSERT_EQ(::mkdir(save_dir_.c_str(), 0700), 0);
  ASSERT_EQ(::chmod(save_dir_.c_str(), 0000), 0);

  Config cfg = DefaultConfig();
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("FILE", "6006");
  std::string saved_path;
  const Status status =
      HandleButtonEvent(event, cfg, t, &saved_path, std::ref(runner_));

  // Restore permissions so TearDown's remove_all can clean up.
  ::chmod(save_dir_.c_str(), 0700);

  EXPECT_EQ(status, Status::kIoError);
  EXPECT_TRUE(saved_path.empty());
  EXPECT_TRUE(runner_.calls().empty());
}

TEST(ExtensionForFormatTest, MapsEachPixelFormat) {
  EXPECT_EQ(ExtensionForFormat(brscan::PixelFormat::kRgb), "jpg");
  EXPECT_EQ(ExtensionForFormat(brscan::PixelFormat::kGray), "pgm");
  EXPECT_EQ(ExtensionForFormat(brscan::PixelFormat::kBitonal), "pbm");
}

TEST(BuildOutputPathTest, IncludesFuncRegidAndExtension) {
  const ButtonEvent event = MakeEvent("OCR", "9999");
  const std::string path =
      BuildOutputPath("/tmp/somewhere", event, brscan::PixelFormat::kBitonal);
  EXPECT_NE(path.find("/tmp/somewhere/"), std::string::npos);
  EXPECT_NE(path.find("OCR"), std::string::npos);
  EXPECT_NE(path.find("9999"), std::string::npos);
  EXPECT_EQ(path.substr(path.size() - 4), ".pbm");
}

TEST(BuildOutputPathTest, SanitizesTraversalCharactersInRegid) {
  const ButtonEvent event = MakeEvent("FILE", "../../../../etc/passwd");
  const std::string path =
      BuildOutputPath("/tmp/somewhere", event, brscan::PixelFormat::kRgb);

  const std::filesystem::path p(path);
  EXPECT_EQ(p.parent_path(), "/tmp/somewhere");
  const std::string filename = p.filename().string();
  // Only the '/' path-component boundary and the extension's '.' remain
  // literal; every '/' and '.' from the forged REGID is stripped, so no
  // ".." or "/" survives into the filename itself.
  EXPECT_EQ(filename.find(".."), std::string::npos);
  EXPECT_EQ(filename.find('/'), std::string::npos);
  EXPECT_NE(filename.find("etcpasswd"), std::string::npos);
}

TEST(BuildOutputPathTest, SanitizesTraversalCharactersInFunc) {
  const ButtonEvent event = MakeEvent("../../../../etc/passwd", "1234");
  const std::string path =
      BuildOutputPath("/tmp/somewhere", event, brscan::PixelFormat::kRgb);

  const std::filesystem::path p(path);
  EXPECT_EQ(p.parent_path(), "/tmp/somewhere");
  const std::string filename = p.filename().string();
  EXPECT_EQ(filename.find(".."), std::string::npos);
  EXPECT_EQ(filename.find('/'), std::string::npos);
}

TEST(BuildOutputPathTest, CapsOverlongRegidLength) {
  // REGID arrives off an untrusted UDP datagram and could be thousands of
  // bytes; the built filename must still stay within the filesystem's
  // per-component limit (255 on macOS) rather than failing the write with
  // ENAMETOOLONG.
  const std::string huge_regid(4000, 'A');
  const ButtonEvent event = MakeEvent("FILE", huge_regid);
  const std::string path =
      BuildOutputPath("/tmp/somewhere", event, brscan::PixelFormat::kRgb);

  const std::string filename = std::filesystem::path(path).filename().string();
  EXPECT_LE(filename.size(), 255u);
  EXPECT_EQ(std::filesystem::path(path).parent_path(), "/tmp/somewhere");
}

}  // namespace
}  // namespace brscan::scand
