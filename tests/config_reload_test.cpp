// Tests for the daemon's SIGHUP config-reload path (daemon/config.h's
// TryReloadConfig()). tools/brscan-scand.cpp's signal plumbing itself isn't
// unit-testable directly (the handler/flag/loop live in main()), so these
// tests exercise the reload *decision* function on its own: given a config
// path, does it return a fresh Config (success -- the loop should swap it
// in), or std::nullopt (keep-previous, per the brief's printer_host rule)?
//
// Synthetic values only throughout (clean-room: no real device identity) --
// BRW00AABBCCDDEE / ~/Scans, per the task brief.

#include "config.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace brscan::scand {
namespace {

constexpr char kSyntheticHost[] = "BRW00AABBCCDDEE.local";

std::filesystem::path TempConfigPath(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

void WriteConfig(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << contents;
}

TEST(TryReloadConfigTest, PicksUpChangedDestValuesWithoutRestart) {
  const auto path = TempConfigPath("brscan_scand_reload_test_dest.conf");
  WriteConfig(path, std::string("printer_host=") + kSyntheticHost +
                         "\n"
                         "save_dir=~/Scans\n"
                         "file.mode=color\n"
                         "file.dpi=300\n");

  const auto first = TryReloadConfig(path.string());
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->file_params.mode, brscan::ScanMode::kColor);
  EXPECT_EQ(first->file_params.x_dpi, 300);

  // Rewrite the same file with changed <dest>.* values, simulating a
  // GUI's "Save & apply" (task 1e.10) sending SIGHUP right after --
  // this must be picked up with no process restart involved.
  WriteConfig(path, std::string("printer_host=") + kSyntheticHost +
                         "\n"
                         "save_dir=~/Scans\n"
                         "file.mode=gray\n"
                         "file.dpi=600\n");

  const auto second = TryReloadConfig(path.string());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->file_params.mode, brscan::ScanMode::kGray);
  EXPECT_EQ(second->file_params.x_dpi, 600);

  std::filesystem::remove(path);
}

TEST(TryReloadConfigTest, InvalidPrinterHostKeepsPreviousSignal) {
  const auto path = TempConfigPath("brscan_scand_reload_test_invalid.conf");
  WriteConfig(path,
              "printer_host=\n"
              "save_dir=~/Scans\n");

  const auto reloaded = TryReloadConfig(path.string());
  EXPECT_FALSE(reloaded.has_value());

  std::filesystem::remove(path);
}

TEST(TryReloadConfigTest, MissingFileAlsoKeepsPreviousSignal) {
  // A config file that doesn't exist at reload time (deleted, or briefly
  // absent mid-rewrite) must not be treated as "valid, all defaults" --
  // LoadConfig()'s missing-file fallback has an empty printer_host, which
  // TryReloadConfig() rejects the same as any other invalid reload.
  const auto reloaded =
      TryReloadConfig("/nonexistent/path/brscan-reload-missing.conf");
  EXPECT_FALSE(reloaded.has_value());
}

TEST(TryReloadConfigTest, ValidReloadKeepsUnrelatedFieldsAtTheirDefaults) {
  const auto path = TempConfigPath("brscan_scand_reload_test_partial.conf");
  WriteConfig(path, std::string("printer_host=") + kSyntheticHost + "\n");

  const auto reloaded = TryReloadConfig(path.string());
  ASSERT_TRUE(reloaded.has_value());
  EXPECT_EQ(reloaded->printer_host, kSyntheticHost);
  EXPECT_EQ(reloaded->save_dir, ExpandHome(kDefaultSaveDir));

  std::filesystem::remove(path);
}

}  // namespace
}  // namespace brscan::scand
