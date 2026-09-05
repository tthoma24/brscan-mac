// Tests for the scan-button precedence planner (daemon/button_plan.h).
// PlanButtonScan is pure: no transport, no filesystem -- these tests drive
// it directly with captured-shaped config-command frames (see
// tests/button_config_test.cpp and reference/protocol-notes-button-
// options.md) and a Config.

#include "button_plan.h"

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "config.h"
#include "output_writer.h"
#include "paper_size.h"

namespace brscan::scand {
namespace {

// Builds a well-formed config-command frame: {0x30, payload.size(), 0x00,
// payload...} -- same shape as tests/button_config_test.cpp's BuildFrame
// and tests/scanner_test.cpp's EncodeButtonConfigFrame.
std::vector<uint8_t> BuildFrame(const std::string& payload) {
  std::vector<uint8_t> frame;
  frame.push_back(0x30);
  frame.push_back(static_cast<uint8_t>(payload.size()));
  frame.push_back(0x00);
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

// The full LCD-set (Touch-Panel-ON) baseline payload: File/Color/PDF/
// Letter/300dpi, verbatim shape from
// reference/protocol-notes-button-options.md (same fixture
// tests/button_config_test.cpp and tests/scanner_test.cpp use).
const char* const kColorLetterPayload =
    "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
    "T=PDF(Image)\nW=0\nG=0\nX=0\n";

void ExpectArea(const brscan::Area& area, int x0, int y0, int x1, int y1) {
  EXPECT_EQ(area.x0, x0);
  EXPECT_EQ(area.y0, y0);
  EXPECT_EQ(area.x1, x1);
  EXPECT_EQ(area.y1, y1);
}

// ---------------------------------------------------------------------
// Touch-Panel-ON cases.
// ---------------------------------------------------------------------

TEST(PlanButtonScanTest, TouchPanelOnColorLetterPdf) {
  const Config cfg = DefaultConfig();
  const auto plan =
      PlanButtonScan(BuildFrame(kColorLetterPayload), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_TRUE(plan->touch_panel_on);
  EXPECT_EQ(plan->params.mode, brscan::ScanMode::kColor);
  EXPECT_EQ(plan->params.x_dpi, 300);
  EXPECT_EQ(plan->params.y_dpi, 300);
  ExpectArea(plan->params.area, 478, 0, 2990, 3253);  // AreaForPaper(LETTER,300)
  EXPECT_TRUE(plan->params.button_flow);
  EXPECT_FALSE(plan->params.duplex);
  EXPECT_FALSE(plan->params.remove_background);

  EXPECT_EQ(plan->output.format, OutputFormat::kPdf);
  EXPECT_FALSE(plan->output.searchable);
}

TEST(PlanButtonScanTest, TouchPanelOnBlackWhiteMultiTiff) {
  const std::string payload =
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=TEXT\nP=LETTER\nA=0\n"
      "T=MULTI-TIFF\nW=0\nG=0\nX=0\n";
  const Config cfg = DefaultConfig();
  const auto plan = PlanButtonScan(BuildFrame(payload), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_TRUE(plan->touch_panel_on);
  EXPECT_EQ(plan->params.mode, brscan::ScanMode::kBlackWhite);
  EXPECT_EQ(plan->output.format, OutputFormat::kTiff);
}

TEST(PlanButtonScanTest, TouchPanelOnJpeg) {
  const std::string payload =
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=JPEG\nW=0\nG=0\nX=0\n";
  const Config cfg = DefaultConfig();
  const auto plan = PlanButtonScan(BuildFrame(payload), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_EQ(plan->output.format, OutputFormat::kJpeg);
}

TEST(PlanButtonScanTest, TouchPanelOnDuplexSetsParamsDuplexTrue) {
  const std::string payload =
      "F=FILE\nD=DUP\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=0\nX=0\n";
  const Config cfg = DefaultConfig();
  const auto plan = PlanButtonScan(BuildFrame(payload), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_TRUE(plan->params.duplex);
}

TEST(PlanButtonScanTest, TouchPanelOnRemoveBackgroundSetsFlagAndLevel) {
  const std::string payload =
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LETTER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=1\nL=128\nX=0\n";
  const Config cfg = DefaultConfig();
  const auto plan = PlanButtonScan(BuildFrame(payload), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_TRUE(plan->params.remove_background);
  EXPECT_EQ(plan->params.remove_background_level, 128);
}

TEST(PlanButtonScanTest, TouchPanelOnKnownPaperLegalUnchangedByDefaultGuard) {
  // A concrete, valid area from a known P= token must survive the final
  // zero-area safety-net guard untouched.
  const std::string payload =
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=LEGAL\nA=0\n"
      "T=PDF(Image)\nW=0\nG=0\nX=0\n";
  const Config cfg = DefaultConfig();
  const auto plan = PlanButtonScan(BuildFrame(payload), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  const auto want_area = AreaForPaper("LEGAL", 300);
  ASSERT_TRUE(want_area.has_value());
  ExpectArea(plan->params.area, want_area->x0, want_area->y0, want_area->x1,
             want_area->y1);
}

TEST(PlanButtonScanTest, TouchPanelOnUnknownPaperFallsBackToDefaultGuard) {
  // An unknown P= token leaves the area at zero mid-function (as before),
  // but the final safety-net guard now replaces that zero area with
  // kDefaultAutoPaper's ("LETTER") concrete area, rather than leaving it
  // zero for RunButtonScan's ADF-offer (ymax=0) fallback to mishandle.
  const std::string payload =
      "F=FILE\nD=SIN\nE=LON\nR=300\nM=CGRAY\nP=NOT-A-REAL-PAPER\nA=0\n"
      "T=PDF(Image)\nW=0\nG=0\nX=0\n";
  const Config cfg = DefaultConfig();
  const auto plan = PlanButtonScan(BuildFrame(payload), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  const auto want_area = AreaForPaper("LETTER", 300);
  ASSERT_TRUE(want_area.has_value());
  ExpectArea(plan->params.area, want_area->x0, want_area->y0, want_area->x1,
             want_area->y1);
}

TEST(PlanButtonScanTest, TouchPanelOnOcrFuncForcesSearchablePdfRegardlessOfT) {
  // Any T= (including the OCR-only sub-formats TXT/HTML/RTF) must still
  // yield a searchable PDF once func == OCR -- see button_plan.cpp's
  // deferred-fields note.
  for (const std::string& output_type : {"TXT", "HTML", "RTF"}) {
    const std::string payload =
        "F=OCR\nD=SIN\nE=LON\nR=300\nM=TEXT\nP=LETTER\nA=0\n"
        "T=" + output_type + "\nW=0\nX=0\n";
    const Config cfg = DefaultConfig();
    const auto plan = PlanButtonScan(BuildFrame(payload), "OCR", cfg);
    ASSERT_TRUE(plan.has_value()) << "T=" << output_type;

    EXPECT_TRUE(plan->touch_panel_on) << "T=" << output_type;
    EXPECT_EQ(plan->output.format, OutputFormat::kPdf) << "T=" << output_type;
    EXPECT_TRUE(plan->output.searchable) << "T=" << output_type;
  }
}

// ---------------------------------------------------------------------
// Touch-Panel-OFF case: the short "Auto" form (no R=).
// ---------------------------------------------------------------------

TEST(PlanButtonScanTest, TouchPanelOffShortFormUsesConfiguredParamsAndOutput) {
  const std::string short_form = "F=FILE\nD=SIN\nE=LON\n";
  Config cfg = DefaultConfig();
  cfg.file_params.mode = brscan::ScanMode::kGray;
  cfg.file_params.x_dpi = 150;
  cfg.file_params.y_dpi = 150;
  cfg.file_output.format = OutputFormat::kTiff;

  const auto plan = PlanButtonScan(BuildFrame(short_form), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_FALSE(plan->touch_panel_on);
  EXPECT_EQ(plan->params.mode, cfg.file_params.mode);
  EXPECT_EQ(plan->params.x_dpi, cfg.file_params.x_dpi);
  EXPECT_EQ(plan->params.y_dpi, cfg.file_params.y_dpi);
  EXPECT_EQ(plan->params.duplex, cfg.file_params.duplex);
  EXPECT_EQ(plan->params.brightness, cfg.file_params.brightness);
  EXPECT_EQ(plan->params.contrast, cfg.file_params.contrast);
  EXPECT_TRUE(plan->params.button_flow);
  // No <dest>.paper configured -> the OFF-branch default (kDefaultAutoPaper,
  // "LETTER") applies, at this FUNC's configured dpi (150) -- not the zero
  // area ParamsForFunc's own default would otherwise leave in place.
  const auto want_area = AreaForPaper("LETTER", cfg.file_params.x_dpi);
  ASSERT_TRUE(want_area.has_value());
  ExpectArea(plan->params.area, want_area->x0, want_area->y0, want_area->x1,
             want_area->y1);

  EXPECT_EQ(plan->output.format, cfg.file_output.format);
}

TEST(PlanButtonScanTest, TouchPanelOffNoPaperDefaultsToLetterAtDefaultDpi) {
  // Mirrors the brief's exact case: an unmodified DefaultConfig() (no
  // file.paper, default 300 dpi) must yield AreaForPaper("LETTER", 300),
  // not a zero area.
  const std::string short_form = "F=FILE\nD=SIN\nE=LON\n";
  const Config cfg = DefaultConfig();

  const auto plan = PlanButtonScan(BuildFrame(short_form), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_FALSE(plan->touch_panel_on);
  const auto want_area = AreaForPaper("LETTER", 300);
  ASSERT_TRUE(want_area.has_value());
  ExpectArea(plan->params.area, want_area->x0, want_area->y0, want_area->x1,
             want_area->y1);
}

TEST(PlanButtonScanTest, TouchPanelOffWithConfiguredPaperUsesAreaForPaper) {
  // file.paper = A4 at a non-default dpi (200) -- proves the area comes
  // from PaperForFunc(cfg, func) via AreaForPaper at the daemon's own
  // configured dpi, not the OFF-branch's kDefaultAutoPaper fallback.
  const std::string short_form = "F=FILE\nD=SIN\nE=LON\n";
  Config cfg = DefaultConfig();
  cfg.file_params.x_dpi = 200;
  cfg.file_params.y_dpi = 200;
  cfg.file_paper = "A4";

  const auto plan = PlanButtonScan(BuildFrame(short_form), "FILE", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_FALSE(plan->touch_panel_on);
  const auto want_area = AreaForPaper("A4", 200);
  ASSERT_TRUE(want_area.has_value());
  ExpectArea(plan->params.area, want_area->x0, want_area->y0, want_area->x1,
             want_area->y1);
}

TEST(PlanButtonScanTest, TouchPanelOffOcrFuncStillForcesSearchablePdf) {
  const std::string short_form = "F=OCR\nD=SIN\nE=LON\n";
  Config cfg = DefaultConfig();  // ocr_output defaults to native.
  const auto plan = PlanButtonScan(BuildFrame(short_form), "OCR", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_FALSE(plan->touch_panel_on);
  EXPECT_EQ(plan->output.format, OutputFormat::kPdf);
  EXPECT_TRUE(plan->output.searchable);
}

// Regression test: a configured OCR separation (or any other non-default
// OCR output setting) must survive the OCR format/searchable promotion --
// PlanButtonScan should start from OutputSettingsForFunc(cfg, "OCR") and
// override only format/searchable, not replace the whole OutputSettings
// with a default-constructed one (which would silently drop
// ocr.separation=every:N).
TEST(PlanButtonScanTest, OcrFuncPreservesConfiguredSeparation) {
  const std::string short_form = "F=OCR\nD=SIN\nE=LON\n";
  Config cfg = DefaultConfig();
  cfg.ocr_output.separation = OutputSeparation::kEveryN;
  cfg.ocr_output.separate_n = 3;

  const auto plan = PlanButtonScan(BuildFrame(short_form), "OCR", cfg);
  ASSERT_TRUE(plan.has_value());

  EXPECT_EQ(plan->output.format, OutputFormat::kPdf);
  EXPECT_TRUE(plan->output.searchable);
  EXPECT_EQ(plan->output.separation, OutputSeparation::kEveryN);
  EXPECT_EQ(plan->output.separate_n, 3);
}

// ---------------------------------------------------------------------
// button_flow is always true, in every branch.
// ---------------------------------------------------------------------

TEST(PlanButtonScanTest, ButtonFlowAlwaysTrue) {
  const Config cfg = DefaultConfig();
  const auto on_plan =
      PlanButtonScan(BuildFrame(kColorLetterPayload), "FILE", cfg);
  ASSERT_TRUE(on_plan.has_value());
  EXPECT_TRUE(on_plan->params.button_flow);

  const auto off_plan =
      PlanButtonScan(BuildFrame("F=FILE\nD=SIN\nE=LON\n"), "FILE", cfg);
  ASSERT_TRUE(off_plan.has_value());
  EXPECT_TRUE(off_plan->params.button_flow);
}

// ---------------------------------------------------------------------
// Malformed frame -> nullopt.
// ---------------------------------------------------------------------

TEST(PlanButtonScanTest, MalformedFrameReturnsNullopt) {
  // Header byte 0 is 0x31, not the required 0x30 -- malformed (same
  // shape tests/button_config_test.cpp's RejectsWrongMagic uses).
  std::vector<uint8_t> frame = BuildFrame("F=FILE\nD=SIN\n");
  frame[0] = 0x31;

  const Config cfg = DefaultConfig();
  const auto plan = PlanButtonScan(frame, "FILE", cfg);
  EXPECT_FALSE(plan.has_value());
}

}  // namespace
}  // namespace brscan::scand
