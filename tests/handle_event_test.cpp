// Tests for the per-button-press pipeline (daemon/handle_event.h):
// FUNC -> Params -> RunScan -> save -> PerformAction. Hermetic: drives
// RunScan over a brscan::FakeTransport queued with a synthetic scan
// reply, and writes to a real temp directory (removed at the end of each
// test) rather than touching a real printer or the real 54925 UDP port.

#include "handle_event.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

namespace brscan::scand {
namespace {

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

void QueuePreamble(brscan::FakeTransport* t) {
  t->QueueRead(std::string("+OK 200\r\n"));
  t->QueueRead(std::vector<uint8_t>{0xc1, 0x00, 0x35, 0x0a});  // ESC Q reply
  t->QueueTimeout();
  t->QueueRead(std::vector<uint8_t>{0x80, 0x00});  // ESC S (flatbed) ack
  t->QueueTimeout();
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
};

TEST_F(HandleButtonEventTest, FileFuncUsesFileParamsAndSavesJpeg) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,16,427,8,"));

  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);

  Config cfg = DefaultConfig();  // file_params: color, 300dpi, flatbed.
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("FILE", "1001");
  std::string saved_path;
  const Status status = HandleButtonEvent(event, cfg, t, &saved_path);

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
}

TEST_F(HandleButtonEventTest, ImageFuncUsesImageParamsDistinctFromFile) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  // width_px=4, height_px=3.
  t.QueueRead(EncodeOfferFrame("300,300,2,292,4,427,3,"));

  auto payload = EncodeBlockHeader(4);
  const std::vector<uint8_t> raw(4 * 3, 0x42);
  payload.insert(payload.end(), raw.begin(), raw.end());
  t.QueueRead(payload);

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
  const Status status = HandleButtonEvent(event, cfg, t, &saved_path);

  ASSERT_EQ(status, Status::kOk);
  EXPECT_TRUE(saved_path.size() >= 4 &&
              saved_path.substr(saved_path.size() - 4) == ".pgm");
  EXPECT_NE(saved_path.find("IMAGE"), std::string::npos);
  EXPECT_NE(saved_path.find("2002"), std::string::npos);

  ASSERT_TRUE(std::filesystem::exists(saved_path));
  const std::vector<uint8_t> written_file = ReadWholeFile(saved_path);
  const std::string want_header = "P5\n4 3\n255\n";
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
}

TEST_F(HandleButtonEventTest, ScanFailurePropagatesStatusAndSavesNothing) {
  brscan::FakeTransport t;
  t.QueueRead(std::string("-NG 401\r\n"));  // Busy greeting.

  Config cfg = DefaultConfig();
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("FILE", "3003");
  std::string saved_path;
  const Status status = HandleButtonEvent(event, cfg, t, &saved_path);

  EXPECT_EQ(status, Status::kBusy);
  EXPECT_TRUE(saved_path.empty());
  // No file should have been written for a scan that never happened: the
  // save_dir is either never created, or created but left empty.
  EXPECT_TRUE(!std::filesystem::exists(save_dir_) ||
              std::filesystem::is_empty(save_dir_));
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
  const Status status = HandleButtonEvent(event, cfg, t, &saved_path);

  EXPECT_EQ(status, Status::kProtocolError);
  EXPECT_TRUE(saved_path.empty());
  EXPECT_TRUE(t.written().empty());
  EXPECT_TRUE(!std::filesystem::exists(save_dir_) ||
              std::filesystem::is_empty(save_dir_));
}

TEST_F(HandleButtonEventTest, SanitizesPathTraversalInRegidAndStaysInsideSaveDir) {
  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,16,427,8,"));
  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);

  Config cfg = DefaultConfig();
  cfg.save_dir = save_dir_;

  // A forged REGID (straight off an untrusted UDP notification in real
  // use) trying to escape save_dir. FUNC is a valid, known value, so this
  // exercises BuildOutputPath's REGID sanitization and
  // HandleButtonEvent's save_dir-containment check specifically, distinct
  // from the FUNC-rejection test above.
  const ButtonEvent event = MakeEvent("FILE", "../../../../../../tmp/evil");
  std::string saved_path;
  const Status status = HandleButtonEvent(event, cfg, t, &saved_path);

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
}

TEST_F(HandleButtonEventTest, UnwritableSaveDirReturnsIoErrorNotCrash) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "running as root: permission bits don't block writes";
  }

  brscan::FakeTransport t;
  QueuePreamble(&t);
  t.QueueRead(EncodeOfferFrame("300,300,2,292,16,427,8,"));
  const auto jpeg = MakeSyntheticJpeg(16, 8);
  auto payload = EncodeBlockHeader(static_cast<uint16_t>(jpeg.size()));
  payload.insert(payload.end(), jpeg.begin(), jpeg.end());
  t.QueueRead(payload);

  ASSERT_EQ(::mkdir(save_dir_.c_str(), 0700), 0);
  ASSERT_EQ(::chmod(save_dir_.c_str(), 0000), 0);

  Config cfg = DefaultConfig();
  cfg.save_dir = save_dir_;

  const ButtonEvent event = MakeEvent("FILE", "6006");
  std::string saved_path;
  const Status status = HandleButtonEvent(event, cfg, t, &saved_path);

  // Restore permissions so TearDown's remove_all can clean up.
  ::chmod(save_dir_.c_str(), 0700);

  EXPECT_EQ(status, Status::kIoError);
  EXPECT_TRUE(saved_path.empty());
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

}  // namespace
}  // namespace brscan::scand
