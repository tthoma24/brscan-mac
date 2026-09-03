#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include "brscan/types.h"
#include "command.h"

namespace {

std::vector<uint8_t> ReadFixture(const std::string& name) {
  const std::string path =
      std::string(BRSCAN_FIXTURES_DIR) + "/requests/" + name;
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
}

}  // namespace

TEST(Command, EncodeQuery) {
  const std::vector<uint8_t> want = {0x1b, 0x51, 0x0a, 0x80};
  EXPECT_EQ(brscan::EncodeQuery(), want);
}

TEST(Command, EncodeReset) {
  // ESC R is the one exception to the standard framing: a bare two-byte
  // sequence with no trailing LF and no 0x80 terminator. See the doc
  // comment on EncodeReset() in command.h.
  const std::vector<uint8_t> want = {0x1b, 0x52};
  EXPECT_EQ(brscan::EncodeReset(), want);
}

TEST(Command, EncodeSelectFlatbed) {
  // 1b 53 0a 46 42 0a 80  =  ESC S \n "FB" \n 0x80
  const std::vector<uint8_t> want = {0x1b, 0x53, 0x0a, 0x46,
                                      0x42, 0x0a, 0x80};
  EXPECT_EQ(brscan::EncodeSelectFlatbed(), want);
}

TEST(Command, EncodeSelectAdf) {
  // 1b 44 0a 41 44 46 0a 80  =  ESC D \n "ADF" \n 0x80
  const std::vector<uint8_t> want = {0x1b, 0x44, 0x0a, 0x41, 0x44,
                                      0x46, 0x0a, 0x80};
  EXPECT_EQ(brscan::EncodeSelectAdf(), want);
}

namespace {

// Fixtures are captured as ESC X (execute) bytes only; these tests assert
// EncodeExecute() reproduces them byte-for-byte. Areas/resolutions below are
// read directly out of the fixture bytes (see the task's xxd dump), not
// invented.

brscan::Params ColorParams(int x_dpi, int y_dpi, brscan::Area area,
                            bool duplex = false) {
  brscan::Params p;
  p.mode = brscan::ScanMode::kColor;
  p.x_dpi = x_dpi;
  p.y_dpi = y_dpi;
  p.area = area;
  p.duplex = duplex;
  return p;
}

}  // namespace

TEST(Command, EncodeExecuteColor300A3) {
  const auto p = ColorParams(300, 300, {0, 0, 3472, 4961});
  EXPECT_EQ(brscan::EncodeExecute(p), ReadFixture("color-300-a3.bin"));
}

TEST(Command, EncodeExecuteGray300A3) {
  brscan::Params p;
  p.mode = brscan::ScanMode::kGray;
  p.x_dpi = 300;
  p.y_dpi = 300;
  p.area = {0, 0, 3472, 4961};
  const auto got = brscan::EncodeExecute(p);
  const auto want = ReadFixture("gray-300-a3.bin");
  EXPECT_EQ(got, want);
  // Belt and suspenders: grayscale must use GRAY64/NONE and must NOT emit a
  // J= line.
  const std::string body(reinterpret_cast<const char*>(got.data()),
                          got.size());
  EXPECT_NE(body.find("M=GRAY64\n"), std::string::npos);
  EXPECT_NE(body.find("C=NONE\n"), std::string::npos);
  EXPECT_EQ(body.find("J="), std::string::npos);
}

TEST(Command, EncodeExecuteColor300Crop) {
  const auto p = ColorParams(300, 300, {1572, 1575, 2948, 2634});
  EXPECT_EQ(brscan::EncodeExecute(p), ReadFixture("color-300-crop.bin"));
}

TEST(Command, EncodeExecuteColor150) {
  const auto p = ColorParams(150, 150, {786, 787, 1474, 1317});
  EXPECT_EQ(brscan::EncodeExecute(p), ReadFixture("color-150.bin"));
}

TEST(Command, EncodeExecuteColor200) {
  const auto p = ColorParams(200, 200, {1048, 1050, 1976, 1756});
  EXPECT_EQ(brscan::EncodeExecute(p), ReadFixture("color-200.bin"));
}

TEST(Command, EncodeExecuteColor400) {
  const auto p = ColorParams(400, 400, {2096, 2100, 3936, 3512});
  EXPECT_EQ(brscan::EncodeExecute(p), ReadFixture("color-400.bin"));
}

TEST(Command, EncodeExecuteColor600) {
  const auto p = ColorParams(600, 600, {3144, 3150, 5896, 5268});
  EXPECT_EQ(brscan::EncodeExecute(p), ReadFixture("color-600.bin"));
}

TEST(Command, EncodeExecuteColor1200) {
  const auto p = ColorParams(1200, 1200, {6288, 6300, 11792, 10537});
  EXPECT_EQ(brscan::EncodeExecute(p), ReadFixture("color-1200.bin"));
}

TEST(Command, EncodeExecuteAdfColor300Duplex) {
  auto p = ColorParams(300, 300, {456, 0, 3016, 3252}, /*duplex=*/true);
  p.source = brscan::Source::kAdf;
  const auto got = brscan::EncodeExecute(p);
  EXPECT_EQ(got, ReadFixture("adf-color-300-duplex.bin"));
  const std::string body(reinterpret_cast<const char*>(got.data()),
                          got.size());
  EXPECT_NE(body.find("D=DUP\n"), std::string::npos);
}

TEST(Command, EncodeExecuteAdfColor300Simplex) {
  auto p = ColorParams(300, 300, {456, 0, 3016, 3252}, /*duplex=*/false);
  p.source = brscan::Source::kAdf;
  EXPECT_EQ(brscan::EncodeExecute(p),
            ReadFixture("adf-color-300-simplex.bin"));
}

TEST(Command, EncodeExecuteColor100A3) {
  const auto p = ColorParams(100, 100, {0, 0, 1168, 1654});
  EXPECT_EQ(brscan::EncodeExecute(p), ReadFixture("color-100-a3.bin"));
}

TEST(Command, EncodeInfoMaxProbe) {
  // R=19200,19200 is the capability probe: the device replies with its
  // hardware maximum rather than granting the requested resolution.
  const auto got =
      brscan::EncodeInfo(19200, 19200, brscan::ScanMode::kColor,
                          /*duplex=*/false);
  EXPECT_EQ(got, ReadFixture("info-maxprobe.bin"));
}

TEST(Command, EncodeInfoColor300) {
  const auto got = brscan::EncodeInfo(300, 300, brscan::ScanMode::kColor,
                                       /*duplex=*/false);
  EXPECT_EQ(got, ReadFixture("info-300.bin"));
}

// --- RLENGTH modes (TEXT, ERRDIF, GRAY256) ------------------------------
//
// Expected bytes below are transcribed directly from the ESC X and ESC I
// captures in reference/streams/modes_{text,errdif,gray256}_out.bin (see
// reference/protocol-notes-modes.md); those streams are git-ignored (they
// sit alongside the *_in.bin scan payloads under the same privacy rule),
// so the tests assert against literal byte strings rather than fixture
// files, mirroring EncodeInfoGrayUsesGray64Token below.

TEST(Command, EncodeExecuteTextMatchesCapturedBytes) {
  brscan::Params p;
  p.mode = brscan::ScanMode::kBlackWhite;
  p.x_dpi = 300;
  p.y_dpi = 300;
  p.brightness = 50;
  p.contrast = 50;
  p.area = {0, 0, 3472, 4913};
  p.duplex = false;

  const std::string want =
      "\x1b" "X\n"
      "R=300,300\n"
      "M=TEXT\n"
      "C=RLENGTH\n"
      "J=MID\n"
      "B=50\n"
      "N=50\n"
      "A=0,0,3472,4913\n"
      "D=SIN\n"
      "S=NORMAL_SCAN\n"
      "P=0\n"
      "E=0\n"
      "G=0\n"
      "L=0\n"
      "\x80";
  const std::vector<uint8_t> want_bytes(want.begin(), want.end());
  EXPECT_EQ(brscan::EncodeExecute(p), want_bytes);
}

TEST(Command, EncodeExecuteErrdifMatchesCapturedBytes) {
  brscan::Params p;
  p.mode = brscan::ScanMode::kErrorDiffusion;
  p.x_dpi = 300;
  p.y_dpi = 300;
  p.brightness = 50;
  p.contrast = 50;
  p.area = {0, 0, 3472, 4913};
  p.duplex = false;

  const std::string want =
      "\x1b" "X\n"
      "R=300,300\n"
      "M=ERRDIF\n"
      "C=RLENGTH\n"
      "J=MID\n"
      "B=50\n"
      "N=50\n"
      "A=0,0,3472,4913\n"
      "D=SIN\n"
      "S=NORMAL_SCAN\n"
      "P=0\n"
      "E=0\n"
      "G=0\n"
      "L=0\n"
      "\x80";
  const std::vector<uint8_t> want_bytes(want.begin(), want.end());
  EXPECT_EQ(brscan::EncodeExecute(p), want_bytes);
}

TEST(Command, EncodeExecuteTrueGrayMatchesCapturedBytes) {
  brscan::Params p;
  p.mode = brscan::ScanMode::kTrueGray;
  p.x_dpi = 300;
  p.y_dpi = 300;
  p.brightness = 50;
  p.contrast = 50;
  p.area = {0, 0, 3472, 4913};
  p.duplex = false;

  const std::string want =
      "\x1b" "X\n"
      "R=300,300\n"
      "M=GRAY256\n"
      "C=RLENGTH\n"
      "J=MID\n"
      "B=50\n"
      "N=50\n"
      "A=0,0,3472,4913\n"
      "D=SIN\n"
      "S=NORMAL_SCAN\n"
      "P=0\n"
      "E=0\n"
      "G=0\n"
      "L=0\n"
      "\x80";
  const std::vector<uint8_t> want_bytes(want.begin(), want.end());
  EXPECT_EQ(brscan::EncodeExecute(p), want_bytes);
}

TEST(Command, EncodeInfoTextIncludesNormalScan) {
  // Captured ESC I for M=TEXT: "R=300,300\nM=TEXT\nD=SIN\nS=NORMAL_SCAN\n".
  const std::string want =
      "\x1b" "I\n"
      "R=300,300\n"
      "M=TEXT\n"
      "D=SIN\n"
      "S=NORMAL_SCAN\n"
      "\x80";
  const std::vector<uint8_t> want_bytes(want.begin(), want.end());
  EXPECT_EQ(brscan::EncodeInfo(300, 300, brscan::ScanMode::kBlackWhite,
                                /*duplex=*/false),
            want_bytes);
}

TEST(Command, EncodeInfoColorHasNoNormalScan) {
  // The older Image Capture flow (kColor/kGray) must not gain S= just
  // because the RLENGTH modes now do -- see UsesRlength in command.cpp.
  const auto got = brscan::EncodeInfo(300, 300, brscan::ScanMode::kColor,
                                       /*duplex=*/false);
  const std::string body(reinterpret_cast<const char*>(got.data()), got.size());
  EXPECT_EQ(body.find("S="), std::string::npos);
}

TEST(Command, EncodeInfoGrayUsesGray64Token) {
  // No ESC I fixture was captured for grayscale, so this asserts against
  // literal expected bytes (mirroring EncodeExecuteGray300A3's mode-token
  // check) rather than a fixture file.
  const std::vector<uint8_t> want = {
      0x1b, 'I', 0x0a,
      'R', '=', '3', '0', '0', ',', '3', '0', '0', 0x0a,
      'M', '=', 'G', 'R', 'A', 'Y', '6', '4', 0x0a,
      'D', '=', 'S', 'I', 'N', 0x0a,
      0x80};
  EXPECT_EQ(brscan::EncodeInfo(300, 300, brscan::ScanMode::kGray,
                                /*duplex=*/false),
            want);
}
