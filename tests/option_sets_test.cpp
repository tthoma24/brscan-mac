// Task 1e.1's C++ anti-drift guard: reads config/option-sets.json (the
// shared source of truth also consumed by scripts/gen-option-sets.py for
// the Swift side -- see gui/BrscanConfigCore/Sources/BrscanConfigCore/
// GeneratedOptionSets.swift) and asserts that the daemon's real parsers
// (daemon/config.cpp's Parse*String helpers, reached through the public
// ParseConfig() entry point, and daemon/paper_size.h's IsKnownPaper) still
// accept every token it lists, and reject a sentinel that isn't one of
// them. If config/option-sets.json is edited to add/remove/rename a token
// without a matching daemon change (or vice versa), this test is the one
// that's supposed to fail.
//
// JSON is parsed with tests/json_mini.h's small hand-rolled reader rather
// than a third-party JSON library -- see task-1e1anti-brief.md's Part 3.

#include "config.h"
#include "paper_size.h"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "json_mini.h"

namespace brscan::scand {
namespace {

// BRSCAN_OPTION_SETS_JSON is set by CMakeLists.txt to the absolute path of
// config/option-sets.json, so this test doesn't depend on ctest's working
// directory.
minijson::Value LoadOptionSets() {
  std::ifstream f(BRSCAN_OPTION_SETS_JSON, std::ios::binary);
  if (!f) {
    ADD_FAILURE() << "Could not open " << BRSCAN_OPTION_SETS_JSON;
    return minijson::Value();
  }
  std::ostringstream contents;
  contents << f.rdbuf();
  return minijson::Parse(contents.str());
}

// The "tokens" array under `root[setName]`, or an empty vector (with a
// test failure) if that shape isn't present -- a missing/renamed set in
// config/option-sets.json should fail loudly, not silently test nothing.
std::vector<std::string> TokensFor(const minijson::Value& root,
                                    const std::string& set_name) {
  const minijson::Value* const set = root.Get(set_name);
  if (set == nullptr) {
    ADD_FAILURE() << "config/option-sets.json has no '" << set_name
                  << "' entry";
    return {};
  }
  const minijson::Value* const tokens = set->Get("tokens");
  if (tokens == nullptr) {
    ADD_FAILURE() << "config/option-sets.json's '" << set_name
                  << "' entry has no 'tokens' array";
    return {};
  }
  std::vector<std::string> out = tokens->StringArray();
  EXPECT_FALSE(out.empty())
      << "config/option-sets.json's '" << set_name << "' has no tokens";
  return out;
}

// ---------------------------------------------------------------------
// paper (daemon/paper_size.h's IsKnownPaper).
// ---------------------------------------------------------------------

TEST(OptionSetsDriftTest, EveryPaperTokenIsKnownAndSentinelIsNot) {
  const minijson::Value root = LoadOptionSets();
  for (const std::string& token : TokensFor(root, "paper")) {
    EXPECT_TRUE(IsKnownPaper(token))
        << "config/option-sets.json lists paper token '" << token
        << "' that daemon/paper_size.h's IsKnownPaper() rejects";
  }
  EXPECT_FALSE(IsKnownPaper("ZZZ"));
}

// ---------------------------------------------------------------------
// mode (<dest>.mode -- daemon/config.cpp's ParseModeString, reached via
// the public ParseConfig()).
// ---------------------------------------------------------------------

std::optional<brscan::ScanMode> ExpectedMode(const std::string& token) {
  if (token == "color") return brscan::ScanMode::kColor;
  if (token == "gray") return brscan::ScanMode::kGray;
  if (token == "bw") return brscan::ScanMode::kBlackWhite;
  if (token == "errdiff") return brscan::ScanMode::kErrorDiffusion;
  if (token == "truegray") return brscan::ScanMode::kTrueGray;
  return std::nullopt;
}

TEST(OptionSetsDriftTest, EveryModeTokenMapsToItsScanMode) {
  const minijson::Value root = LoadOptionSets();
  for (const std::string& token : TokensFor(root, "mode")) {
    const std::optional<brscan::ScanMode> expected = ExpectedMode(token);
    ASSERT_TRUE(expected.has_value())
        << "config/option-sets.json lists mode token '" << token
        << "' that this test doesn't know how to verify -- update "
           "ExpectedMode() in tests/option_sets_test.cpp";
    const Config cfg = ParseConfig("file.mode=" + token + "\n");
    EXPECT_EQ(cfg.file_params.mode, *expected) << token;
  }
}

TEST(OptionSetsDriftTest, UnknownModeTokenFallsBackToDefault) {
  const Config cfg = ParseConfig("file.mode=ZZZ\n");
  EXPECT_EQ(cfg.file_params.mode, brscan::ScanMode::kColor);  // documented default
}

// ---------------------------------------------------------------------
// source (<dest>.source -- daemon/config.cpp's ParseSourceString).
// ---------------------------------------------------------------------

struct ExpectedSource {
  brscan::Source source;
  bool duplex;
};

std::optional<ExpectedSource> ExpectedForSourceToken(const std::string& token) {
  if (token == "flatbed") return ExpectedSource{brscan::Source::kFlatbed, false};
  if (token == "adf") return ExpectedSource{brscan::Source::kAdf, false};
  if (token == "adf-duplex") return ExpectedSource{brscan::Source::kAdf, true};
  return std::nullopt;
}

TEST(OptionSetsDriftTest, EverySourceTokenMapsToItsSourceAndDuplex) {
  const minijson::Value root = LoadOptionSets();
  for (const std::string& token : TokensFor(root, "source")) {
    const std::optional<ExpectedSource> expected = ExpectedForSourceToken(token);
    ASSERT_TRUE(expected.has_value())
        << "config/option-sets.json lists source token '" << token
        << "' that this test doesn't know how to verify -- update "
           "ExpectedForSourceToken() in tests/option_sets_test.cpp";
    const Config cfg = ParseConfig("file.source=" + token + "\n");
    EXPECT_EQ(cfg.file_params.source, expected->source) << token;
    EXPECT_EQ(cfg.file_params.duplex, expected->duplex) << token;
  }
}

TEST(OptionSetsDriftTest, UnknownSourceTokenFallsBackToDefault) {
  const Config cfg = ParseConfig("file.source=ZZZ\n");
  EXPECT_EQ(cfg.file_params.source, brscan::Source::kFlatbed);  // documented default
  EXPECT_FALSE(cfg.file_params.duplex);
}

// ---------------------------------------------------------------------
// format (<dest>.format -- daemon/config.cpp's ParseFormatString).
// ---------------------------------------------------------------------

std::optional<OutputFormat> ExpectedFormat(const std::string& token) {
  if (token == "native") return OutputFormat::kNative;
  if (token == "pdf") return OutputFormat::kPdf;
  if (token == "tiff") return OutputFormat::kTiff;
  if (token == "jpeg") return OutputFormat::kJpeg;
  if (token == "png") return OutputFormat::kPng;
  return std::nullopt;
}

TEST(OptionSetsDriftTest, EveryFormatTokenMapsToItsOutputFormat) {
  const minijson::Value root = LoadOptionSets();
  for (const std::string& token : TokensFor(root, "format")) {
    const std::optional<OutputFormat> expected = ExpectedFormat(token);
    ASSERT_TRUE(expected.has_value())
        << "config/option-sets.json lists format token '" << token
        << "' that this test doesn't know how to verify -- update "
           "ExpectedFormat() in tests/option_sets_test.cpp";
    const Config cfg = ParseConfig("file.format=" + token + "\n");
    EXPECT_EQ(cfg.file_output.format, *expected) << token;
  }
}

TEST(OptionSetsDriftTest, UnknownFormatTokenFallsBackToDefault) {
  const Config cfg = ParseConfig("file.format=ZZZ\n");
  EXPECT_EQ(cfg.file_output.format, OutputFormat::kNative);  // documented default
}

// ---------------------------------------------------------------------
// tiff_compression (<dest>.tiff_compression -- daemon/config.cpp's
// ParseTiffCompressionString).
// ---------------------------------------------------------------------

std::optional<TiffCompression> ExpectedTiffCompression(const std::string& token) {
  if (token == "lzw") return TiffCompression::kLzw;
  if (token == "g3") return TiffCompression::kG3;
  if (token == "g4") return TiffCompression::kG4;
  return std::nullopt;
}

TEST(OptionSetsDriftTest, EveryTiffCompressionTokenMapsToItsCompression) {
  const minijson::Value root = LoadOptionSets();
  for (const std::string& token : TokensFor(root, "tiff_compression")) {
    const std::optional<TiffCompression> expected = ExpectedTiffCompression(token);
    ASSERT_TRUE(expected.has_value())
        << "config/option-sets.json lists tiff_compression token '" << token
        << "' that this test doesn't know how to verify -- update "
           "ExpectedTiffCompression() in tests/option_sets_test.cpp";
    const Config cfg = ParseConfig("file.tiff_compression=" + token + "\n");
    EXPECT_EQ(cfg.file_output.tiff_compression, *expected) << token;
  }
}

TEST(OptionSetsDriftTest, UnknownTiffCompressionTokenFallsBackToDefault) {
  const Config cfg = ParseConfig("file.tiff_compression=ZZZ\n");
  EXPECT_EQ(cfg.file_output.tiff_compression, TiffCompression::kLzw);  // default
}

// ---------------------------------------------------------------------
// separation_mode (<dest>.separation -- daemon/config.cpp's
// ParseSeparationString). config/option-sets.json lists the *modes*
// (combine/off/image/page); image and page take a positive-int ':N'
// suffix, exercised here with N=5. "every:N" is a documented backward-
// compat alias for "image:N", not a separate mode, so it isn't in the
// JSON's token list -- it still gets its own coverage in
// tests/config_test.cpp.
// ---------------------------------------------------------------------

struct ExpectedSeparation {
  OutputSeparation separation;
  int separate_n;
};

// Returns the expected (separation, separate_n) for `mode`, and writes the
// `<dest>.separation` value to actually parse (adding the ':5' suffix for
// image/page) into `*config_value`.
std::optional<ExpectedSeparation> ExpectedForSeparationMode(
    const std::string& mode, std::string* config_value) {
  if (mode == "combine") {
    *config_value = "combine";
    return ExpectedSeparation{OutputSeparation::kCombine, 1};
  }
  if (mode == "off") {
    *config_value = "off";
    return ExpectedSeparation{OutputSeparation::kCombine, 1};
  }
  if (mode == "image") {
    *config_value = "image:5";
    return ExpectedSeparation{OutputSeparation::kEveryImage, 5};
  }
  if (mode == "page") {
    *config_value = "page:5";
    return ExpectedSeparation{OutputSeparation::kEveryPage, 5};
  }
  return std::nullopt;
}

TEST(OptionSetsDriftTest, EverySeparationModeMapsToItsOutputSeparation) {
  const minijson::Value root = LoadOptionSets();
  for (const std::string& mode : TokensFor(root, "separation_mode")) {
    std::string config_value;
    const std::optional<ExpectedSeparation> expected =
        ExpectedForSeparationMode(mode, &config_value);
    ASSERT_TRUE(expected.has_value())
        << "config/option-sets.json lists separation_mode token '" << mode
        << "' that this test doesn't know how to verify -- update "
           "ExpectedForSeparationMode() in tests/option_sets_test.cpp";
    const Config cfg = ParseConfig("file.separation=" + config_value + "\n");
    EXPECT_EQ(cfg.file_output.separation, expected->separation) << mode;
    EXPECT_EQ(cfg.file_output.separate_n, expected->separate_n) << mode;
  }
}

TEST(OptionSetsDriftTest, UnknownSeparationModeFallsBackToDefault) {
  const Config cfg = ParseConfig("file.separation=ZZZ:5\n");
  EXPECT_EQ(cfg.file_output.separation, OutputSeparation::kCombine);  // default
  EXPECT_EQ(cfg.file_output.separate_n, 1);
}

// ---------------------------------------------------------------------
// dpi (<dest>.dpi -- daemon/config.cpp's ParsePositiveInt). Not an
// enumerated token set: config/option-sets.json documents it as a rule
// ("positive_integer") rather than a "tokens" array.
// ---------------------------------------------------------------------

TEST(OptionSetsDriftTest, DpiIsDocumentedAsAPositiveIntegerRule) {
  const minijson::Value root = LoadOptionSets();
  const minijson::Value* const dpi = root.Get("dpi");
  ASSERT_NE(dpi, nullptr) << "config/option-sets.json has no 'dpi' entry";
  const minijson::Value* const rule = dpi->Get("rule");
  ASSERT_NE(rule, nullptr) << "config/option-sets.json's 'dpi' entry has no 'rule'";
  EXPECT_EQ(rule->string_value, "positive_integer");

  const minijson::Value* const default_value = dpi->Get("default");
  ASSERT_NE(default_value, nullptr)
      << "config/option-sets.json's 'dpi' entry has no 'default'";
  const int documented_default = static_cast<int>(default_value->number_value);

  const Config defaults = DefaultConfig();
  EXPECT_EQ(defaults.file_params.x_dpi, documented_default);
  EXPECT_EQ(defaults.file_params.y_dpi, documented_default);
}

TEST(OptionSetsDriftTest, AnyPositiveDpiIsAccepted) {
  const Config cfg = ParseConfig("file.dpi=1234\n");
  EXPECT_EQ(cfg.file_params.x_dpi, 1234);
  EXPECT_EQ(cfg.file_params.y_dpi, 1234);
}

TEST(OptionSetsDriftTest, NonPositiveOrNonNumericDpiFallsBackToDefault) {
  const Config defaults = DefaultConfig();
  const Config zero = ParseConfig("file.dpi=0\n");
  const Config negative = ParseConfig("file.dpi=-5\n");
  const Config non_numeric = ParseConfig("file.dpi=abc\n");
  EXPECT_EQ(zero.file_params.x_dpi, defaults.file_params.x_dpi);
  EXPECT_EQ(negative.file_params.x_dpi, defaults.file_params.x_dpi);
  EXPECT_EQ(non_numeric.file_params.x_dpi, defaults.file_params.x_dpi);
}

}  // namespace
}  // namespace brscan::scand
