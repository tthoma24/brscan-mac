// Tests for the Scan-button config-command parser (daemon/button_config.h).
//
// PRIVACY: the captured config-command frames
// (reference/brscan-button-options.pcap, git-ignored) carry no device
// identity and no scan image content -- see PROVENANCE.md -- so the
// payload strings below are the verbatim captured bytes, not synthetic
// stand-ins (unlike tests/button_listener_test.cpp and
// tests/snmp_register_test.cpp, whose fixtures do carry a real capturing
// Mac's identity and so use placeholders instead).

#include "button_config.h"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace brscan::scand {
namespace {

// Builds a well-formed config-command frame: {0x30, payload.size(), 0x00,
// payload...}. Every captured payload here is well under 250 bytes (the
// brief documents 59-78), so the length byte never overflows uint8_t.
std::vector<uint8_t> BuildFrame(const std::string& payload) {
  std::vector<uint8_t> frame;
  frame.push_back(0x30);
  frame.push_back(static_cast<uint8_t>(payload.size()));
  frame.push_back(0x00);
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

std::optional<ButtonConfig> ParseFrame(const std::string& payload) {
  const std::vector<uint8_t> frame = BuildFrame(payload);
  return ParseButtonConfig(frame.data(), frame.size());
}

// ---------------------------------------------------------------------
// Fixture 1: baseline File/Color/PDF/Letter -- full struct asserted.
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, ParsesBaselineFileColorPdfLetter) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=0\nX=0\n");

  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->func, "FILE");
  EXPECT_FALSE(cfg->duplex);
  EXPECT_EQ(cfg->duplex_edge, "LON");
  EXPECT_EQ(cfg->dpi, 300);
  EXPECT_EQ(cfg->mode, "CGRAY");
  EXPECT_EQ(cfg->paper, "LETTER");
  EXPECT_EQ(cfg->area_flag, 0);
  EXPECT_EQ(cfg->output_type, "PDF(Image)");
  EXPECT_FALSE(cfg->skip_blank);
  EXPECT_FALSE(cfg->remove_background);
  EXPECT_EQ(cfg->remove_background_level, 0);
  EXPECT_FALSE(cfg->high_speed);
}

// ---------------------------------------------------------------------
// Fixtures 2-9: every distinct P= (paper) token, incl. PHOTO and BCARD.
// ---------------------------------------------------------------------

std::string PaperPayload(const std::string& paper) {
  return "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=" + paper +
         "\nA=0\nT=PDF(Image)\nW=0\nG=0\nX=0\n";
}

TEST(ParseButtonConfigTest, ParsesEveryPaperToken) {
  const std::vector<std::string> papers = {
      "LETTER", "LEGAL", "A4", "LEDGER", "A3", "A5", "EXECUTIVE", "PHOTO",
      "BCARD"};
  for (const std::string& paper : papers) {
    const std::optional<ButtonConfig> cfg = ParseFrame(PaperPayload(paper));
    ASSERT_TRUE(cfg.has_value()) << "paper=" << paper;
    EXPECT_EQ(cfg->paper, paper) << "paper=" << paper;
  }
}

// ---------------------------------------------------------------------
// Fixtures 10-11, 14-16: every distinct T= (output type) token, and
// M=TEXT vs M=CGRAY.
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, ParsesBwMultiTiff) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=TEXT\nP=LETTER\nA=0\n"
      "T=MULTI-TIFF\nW=0\nG=0\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->mode, "TEXT");
  EXPECT_EQ(cfg->output_type, "MULTI-TIFF");
}

TEST(ParseButtonConfigTest, ParsesColorJpeg) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=JPEG\nW=0\nG=0\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->mode, "CGRAY");
  EXPECT_EQ(cfg->output_type, "JPEG");
}

// ---------------------------------------------------------------------
// Fixtures 12-13: the IMAGE and EMAIL routes (F= besides FILE).
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, ParsesImageRoute) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=IMAGE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=0\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->func, "IMAGE");
}

TEST(ParseButtonConfigTest, ParsesEmailRoute) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=EMAIL\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=0\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->func, "EMAIL");
}

// ---------------------------------------------------------------------
// Fixtures 14-16: OCR routes. #14 is the OCR-without-G case: G is
// entirely absent from the payload, so remove_background/_level must
// still come back at their defaults.
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, ParsesOcrTextWithoutGKey) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=OCR\nD=SIN\nE=LON\nR=300\nM=TEXT\nP=LETTER\nA=0\n"
      "T=TXT\nW=0\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->func, "OCR");
  EXPECT_EQ(cfg->mode, "TEXT");
  EXPECT_EQ(cfg->output_type, "TXT");
  EXPECT_FALSE(cfg->remove_background);
  EXPECT_EQ(cfg->remove_background_level, 0);
}

TEST(ParseButtonConfigTest, ParsesOcrHtmlColor) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=OCR\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=HTML\nW=0\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->func, "OCR");
  EXPECT_EQ(cfg->mode, "CGRAY");
  EXPECT_EQ(cfg->output_type, "HTML");
  EXPECT_FALSE(cfg->remove_background);
  EXPECT_EQ(cfg->remove_background_level, 0);
}

TEST(ParseButtonConfigTest, ParsesOcrRtf) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=OCR\nD=SIN\nE=LON\nR=300\nM=TEXT\nP=LETTER\nA=0\n"
      "T=RTF\nW=0\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->func, "OCR");
  EXPECT_EQ(cfg->output_type, "RTF");
  EXPECT_FALSE(cfg->remove_background);
  EXPECT_EQ(cfg->remove_background_level, 0);
}

// ---------------------------------------------------------------------
// Fixture 17: duplex, short edge.
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, ParsesDuplexShortEdge) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=DUP\nE=SHO\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=0\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_TRUE(cfg->duplex);
  EXPECT_EQ(cfg->duplex_edge, "SHO");
}

// ---------------------------------------------------------------------
// Fixture 18: skip-blank on.
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, ParsesSkipBlankOn) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=1\nG=0\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_TRUE(cfg->skip_blank);
}

// ---------------------------------------------------------------------
// Fixtures 19-21: all three remove-background levels.
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, ParsesRemoveBackgroundMedium) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=1\nL=128\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_TRUE(cfg->remove_background);
  EXPECT_EQ(cfg->remove_background_level, 128);
}

TEST(ParseButtonConfigTest, ParsesRemoveBackgroundLow) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=DUP\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=1\nL=64\nX=1\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_TRUE(cfg->remove_background);
  EXPECT_EQ(cfg->remove_background_level, 64);
  EXPECT_TRUE(cfg->high_speed);
}

TEST(ParseButtonConfigTest, ParsesRemoveBackgroundHigh) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=DUP\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=1\nL=192\nX=1\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_TRUE(cfg->remove_background);
  EXPECT_EQ(cfg->remove_background_level, 192);
}

// ---------------------------------------------------------------------
// Fixture 22: high-speed on, 200 dpi.
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, ParsesHighSpeedOn200Dpi) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=SIN\nE=LON\nR=200\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=0\nX=1\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->dpi, 200);
  EXPECT_TRUE(cfg->high_speed);
}

// ---------------------------------------------------------------------
// Malformed-frame tests: nullopt.
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, RejectsWrongMagic) {
  std::vector<uint8_t> frame = BuildFrame("F=FILE\nD=SIN\n");
  frame[0] = 0x31;
  EXPECT_FALSE(ParseButtonConfig(frame.data(), frame.size()).has_value());
}

TEST(ParseButtonConfigTest, RejectsWrongReservedByte) {
  std::vector<uint8_t> frame = BuildFrame("F=FILE\nD=SIN\n");
  frame[2] = 0x01;
  EXPECT_FALSE(ParseButtonConfig(frame.data(), frame.size()).has_value());
}

TEST(ParseButtonConfigTest, RejectsLengthMismatch) {
  std::vector<uint8_t> frame = BuildFrame("F=FILE\nD=SIN\n");
  frame[1] += 1;  // Declared length no longer matches the actual payload.
  EXPECT_FALSE(ParseButtonConfig(frame.data(), frame.size()).has_value());
}

TEST(ParseButtonConfigTest, RejectsTruncatedFrame) {
  const uint8_t truncated[] = {0x30, 0x05};  // Fewer than 3 header bytes.
  EXPECT_FALSE(ParseButtonConfig(truncated, sizeof(truncated)).has_value());
}

TEST(ParseButtonConfigTest, RejectsEmptyFunc) {
  const std::optional<ButtonConfig> cfg = ParseFrame("F=\nD=SIN\n");
  EXPECT_FALSE(cfg.has_value());
}

TEST(ParseButtonConfigTest, RejectsMissingFunc) {
  const std::optional<ButtonConfig> cfg = ParseFrame("D=SIN\nR=300\n");
  EXPECT_FALSE(cfg.has_value());
}

// ---------------------------------------------------------------------
// Tolerant test: an unknown key is ignored and the rest still parses.
// ---------------------------------------------------------------------

TEST(ParseButtonConfigTest, IgnoresUnknownKey) {
  const std::optional<ButtonConfig> cfg = ParseFrame(
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=0\nZ=99\nX=0\n");
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->func, "FILE");
  EXPECT_EQ(cfg->dpi, 300);
  EXPECT_EQ(cfg->paper, "LETTER");
  EXPECT_EQ(cfg->output_type, "PDF(Image)");
  EXPECT_FALSE(cfg->high_speed);
}

}  // namespace
}  // namespace brscan::scand
