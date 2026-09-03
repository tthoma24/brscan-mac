// Tests for the daemon's config-file parsing (daemon/config.h).

#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace brscan::scand {
namespace {

void ExpectDefaultParams(const brscan::Params& p) {
  EXPECT_EQ(p.mode, brscan::ScanMode::kColor);
  EXPECT_EQ(p.source, brscan::Source::kFlatbed);
  EXPECT_FALSE(p.duplex);
  EXPECT_EQ(p.x_dpi, 300);
  EXPECT_EQ(p.y_dpi, 300);
}

TEST(DefaultConfigTest, HasDocumentedDefaults) {
  const Config cfg = DefaultConfig();
  EXPECT_EQ(cfg.printer_host, kDefaultPrinterHost);
  // printer_host has no built-in default -- every printer's mDNS name is
  // device-specific, so there's nothing safe to ship (see config.h's
  // kDefaultPrinterHost comment). main() in tools/brscan-scand.cpp treats
  // an empty printer_host as a hard "not configured" error at startup.
  EXPECT_TRUE(cfg.printer_host.empty());
  EXPECT_FALSE(cfg.display_name.empty());
  EXPECT_EQ(cfg.save_dir, ExpandHome(kDefaultSaveDir));
  ExpectDefaultParams(cfg.file_params);
  ExpectDefaultParams(cfg.image_params);
  ExpectDefaultParams(cfg.ocr_params);
  ExpectDefaultParams(cfg.email_params);
}

TEST(ParseConfigTest, EmptyTextYieldsDefaults) {
  const Config cfg = ParseConfig("");
  const Config want = DefaultConfig();
  EXPECT_EQ(cfg.printer_host, want.printer_host);
  EXPECT_EQ(cfg.display_name, want.display_name);
  EXPECT_EQ(cfg.save_dir, want.save_dir);
  ExpectDefaultParams(cfg.file_params);
}

TEST(ParseConfigTest, IgnoresBlankLinesAndComments) {
  const std::string text =
      "\n"
      "  \n"
      "# a comment\n"
      "   # an indented comment\n"
      "\n";
  const Config cfg = ParseConfig(text);
  const Config want = DefaultConfig();
  EXPECT_EQ(cfg.printer_host, want.printer_host);
  EXPECT_EQ(cfg.display_name, want.display_name);
  ExpectDefaultParams(cfg.file_params);
}

TEST(ParseConfigTest, IgnoresLinesWithoutEquals) {
  const Config cfg = ParseConfig("this line has no equals sign\n");
  EXPECT_EQ(cfg.printer_host, kDefaultPrinterHost);
}

TEST(ParseConfigTest, AppliesTopLevelOverrides) {
  const std::string text =
      "printer_host = 192.0.2.20\n"
      "display_name=Office Mac\n"
      "save_dir = ~/MyScans\n";
  const Config cfg = ParseConfig(text);
  EXPECT_EQ(cfg.printer_host, "192.0.2.20");
  EXPECT_EQ(cfg.display_name, "Office Mac");
  EXPECT_EQ(cfg.save_dir, ExpandHome("~/MyScans"));
}

TEST(ParseConfigTest, AppliesPerFuncOverrides) {
  const std::string text =
      "file.mode=gray\n"
      "file.dpi=600\n"
      "file.source=adf-duplex\n"
      "image.mode=bw\n"
      "ocr.source=adf\n"
      "email.dpi=150\n";
  const Config cfg = ParseConfig(text);

  EXPECT_EQ(cfg.file_params.mode, brscan::ScanMode::kGray);
  EXPECT_EQ(cfg.file_params.x_dpi, 600);
  EXPECT_EQ(cfg.file_params.y_dpi, 600);
  EXPECT_EQ(cfg.file_params.source, brscan::Source::kAdf);
  EXPECT_TRUE(cfg.file_params.duplex);

  EXPECT_EQ(cfg.image_params.mode, brscan::ScanMode::kBlackWhite);
  // Untouched fields on image_params keep their defaults.
  EXPECT_EQ(cfg.image_params.x_dpi, 300);
  EXPECT_EQ(cfg.image_params.source, brscan::Source::kFlatbed);

  EXPECT_EQ(cfg.ocr_params.source, brscan::Source::kAdf);
  EXPECT_FALSE(cfg.ocr_params.duplex);

  EXPECT_EQ(cfg.email_params.x_dpi, 150);
  EXPECT_EQ(cfg.email_params.y_dpi, 150);
}

TEST(ParseConfigTest, IgnoresUnrecognizedKeys) {
  const Config cfg = ParseConfig("this_is_not_a_key=whatever\n"
                                  "bogus.mode=color\n");
  EXPECT_EQ(cfg.printer_host, kDefaultPrinterHost);
  ExpectDefaultParams(cfg.file_params);
}

TEST(ParseConfigTest, IgnoresUnparsableValuesLeavingDefaults) {
  const std::string text =
      "file.mode=not-a-mode\n"
      "file.dpi=not-a-number\n"
      "file.dpi=-5\n"
      "file.source=not-a-source\n";
  const Config cfg = ParseConfig(text);
  ExpectDefaultParams(cfg.file_params);
}

TEST(ParseConfigTest, LaterValueForSameKeyWins) {
  const Config cfg = ParseConfig("file.dpi=100\nfile.dpi=200\n");
  EXPECT_EQ(cfg.file_params.x_dpi, 200);
}

TEST(LoadConfigTest, MissingFileYieldsDefaults) {
  const Config cfg = LoadConfig("/nonexistent/path/does-not-exist.conf");
  const Config want = DefaultConfig();
  EXPECT_EQ(cfg.printer_host, want.printer_host);
  EXPECT_EQ(cfg.display_name, want.display_name);
  ExpectDefaultParams(cfg.file_params);
}

TEST(LoadConfigTest, ReadsAndParsesAnActualFile) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "brscan_scand_config_test.conf";
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << "# test config\n"
         "printer_host=198.51.100.7\n"
         "file.mode=truegray\n";
  }
  const Config cfg = LoadConfig(path.string());
  EXPECT_EQ(cfg.printer_host, "198.51.100.7");
  EXPECT_EQ(cfg.file_params.mode, brscan::ScanMode::kTrueGray);
  std::filesystem::remove(path);
}

TEST(ExpandHomeTest, ExpandsBareTilde) {
  const char* home = std::getenv("HOME");
  ASSERT_NE(home, nullptr);
  EXPECT_EQ(ExpandHome("~"), std::string(home));
}

TEST(ExpandHomeTest, ExpandsTildeSlashPath) {
  const char* home = std::getenv("HOME");
  ASSERT_NE(home, nullptr);
  EXPECT_EQ(ExpandHome("~/Scans"), std::string(home) + "/Scans");
}

TEST(ExpandHomeTest, LeavesAbsolutePathUnchanged) {
  EXPECT_EQ(ExpandHome("/var/tmp/x"), "/var/tmp/x");
}

TEST(ExpandHomeTest, LeavesRelativePathUnchanged) {
  EXPECT_EQ(ExpandHome("Scans"), "Scans");
}

TEST(ExpandHomeTest, LeavesOtherUserTildeUnchanged) {
  EXPECT_EQ(ExpandHome("~otheruser/x"), "~otheruser/x");
}

TEST(ParamsForFuncTest, MapsEachKnownFunc) {
  Config cfg = DefaultConfig();
  cfg.file_params.x_dpi = cfg.file_params.y_dpi = 111;
  cfg.image_params.x_dpi = cfg.image_params.y_dpi = 222;
  cfg.ocr_params.x_dpi = cfg.ocr_params.y_dpi = 333;
  cfg.email_params.x_dpi = cfg.email_params.y_dpi = 444;

  EXPECT_EQ(ParamsForFunc(cfg, "FILE").x_dpi, 111);
  EXPECT_EQ(ParamsForFunc(cfg, "IMAGE").x_dpi, 222);
  EXPECT_EQ(ParamsForFunc(cfg, "OCR").x_dpi, 333);
  EXPECT_EQ(ParamsForFunc(cfg, "EMAIL").x_dpi, 444);
}

TEST(ParamsForFuncTest, FallsBackToFileParamsForUnknownFunc) {
  Config cfg = DefaultConfig();
  cfg.file_params.x_dpi = cfg.file_params.y_dpi = 111;
  EXPECT_EQ(ParamsForFunc(cfg, "BOGUS").x_dpi, 111);
}

TEST(ParseConfigTest, ExplicitEmptyPrinterHostStaysEmpty) {
  // A config that explicitly sets `printer_host=` (no value) should leave
  // Config::printer_host empty, not silently fall back to anything --
  // main() in tools/brscan-scand.cpp treats an empty value as "not
  // configured" and refuses to start.
  const Config cfg = ParseConfig("printer_host=\n");
  EXPECT_TRUE(cfg.printer_host.empty());
}

TEST(IsKnownFuncTest, AcceptsExactlyTheFourKnownFuncs) {
  EXPECT_TRUE(IsKnownFunc("FILE"));
  EXPECT_TRUE(IsKnownFunc("IMAGE"));
  EXPECT_TRUE(IsKnownFunc("OCR"));
  EXPECT_TRUE(IsKnownFunc("EMAIL"));
}

TEST(IsKnownFuncTest, RejectsAnythingElse) {
  EXPECT_FALSE(IsKnownFunc(""));
  EXPECT_FALSE(IsKnownFunc("file"));  // Case-sensitive per the wire protocol.
  EXPECT_FALSE(IsKnownFunc("BOGUS"));
  EXPECT_FALSE(IsKnownFunc("../../../../etc/passwd"));
  EXPECT_FALSE(IsKnownFunc("FILE;rm -rf /"));
}

TEST(DefaultConfigPathTest, EndsWithExpectedFilename) {
  const std::string path = DefaultConfigPath();
  const std::string suffix = ".config/brscan-scand.conf";
  ASSERT_GE(path.size(), suffix.size());
  EXPECT_EQ(path.substr(path.size() - suffix.size()), suffix);
}

}  // namespace
}  // namespace brscan::scand
