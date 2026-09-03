#include "config.h"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <unistd.h>

namespace brscan::scand {

namespace {

std::string Trim(const std::string& s) {
  const size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::optional<brscan::ScanMode> ParseModeString(const std::string& s) {
  if (s == "color") return brscan::ScanMode::kColor;
  if (s == "gray") return brscan::ScanMode::kGray;
  if (s == "bw") return brscan::ScanMode::kBlackWhite;
  if (s == "errdiff") return brscan::ScanMode::kErrorDiffusion;
  if (s == "truegray") return brscan::ScanMode::kTrueGray;
  return std::nullopt;
}

struct SourceSetting {
  brscan::Source source;
  bool duplex;
};

std::optional<SourceSetting> ParseSourceString(const std::string& s) {
  if (s == "flatbed") return SourceSetting{brscan::Source::kFlatbed, false};
  if (s == "adf") return SourceSetting{brscan::Source::kAdf, false};
  if (s == "adf-duplex") return SourceSetting{brscan::Source::kAdf, true};
  return std::nullopt;
}

std::optional<int> ParsePositiveInt(const std::string& s) {
  if (s.empty()) return std::nullopt;
  try {
    size_t consumed = 0;
    const int value = std::stoi(s, &consumed);
    if (consumed != s.size() || value <= 0) return std::nullopt;
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

// Returns the Params this key's <dest> prefix (file/image/ocr/email) names,
// or nullptr for anything else -- the caller then ignores the key.
brscan::Params* ParamsForDestPrefix(Config* cfg, const std::string& dest) {
  if (dest == "file") return &cfg->file_params;
  if (dest == "image") return &cfg->image_params;
  if (dest == "ocr") return &cfg->ocr_params;
  if (dest == "email") return &cfg->email_params;
  return nullptr;
}

// Applies one already-trimmed, non-empty `key`/`value` pair to `cfg`.
// Unrecognized keys and values that fail to parse are silently ignored --
// see ParseConfig()'s doc comment for why.
void ApplyKey(Config* cfg, const std::string& key, const std::string& value) {
  if (key == "printer_host") {
    cfg->printer_host = value;
    return;
  }
  if (key == "display_name") {
    cfg->display_name = value;
    return;
  }
  if (key == "save_dir") {
    cfg->save_dir = ExpandHome(value);
    return;
  }

  const size_t dot = key.find('.');
  if (dot == std::string::npos) return;
  brscan::Params* const params =
      ParamsForDestPrefix(cfg, key.substr(0, dot));
  if (params == nullptr) return;
  const std::string field = key.substr(dot + 1);

  if (field == "mode") {
    if (const auto mode = ParseModeString(value)) params->mode = *mode;
  } else if (field == "dpi") {
    if (const auto dpi = ParsePositiveInt(value)) {
      params->x_dpi = *dpi;
      params->y_dpi = *dpi;
    }
  } else if (field == "source") {
    if (const auto src = ParseSourceString(value)) {
      params->source = src->source;
      params->duplex = src->duplex;
    }
  }
}

}  // namespace

std::string DefaultDisplayName() {
  char buf[256];
  if (gethostname(buf, sizeof(buf)) == 0) {
    buf[sizeof(buf) - 1] = '\0';
    if (buf[0] != '\0') return std::string(buf);
  }
  return "Mac";
}

std::string ExpandHome(const std::string& path) {
  if (path.empty() || path[0] != '~') return path;
  if (path.size() > 1 && path[1] != '/') {
    // "~otheruser/..." -- not supported, leave untouched rather than
    // guessing.
    return path;
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') return path;
  return std::string(home) + path.substr(1);
}

Config DefaultConfig() {
  Config cfg;
  cfg.printer_host = kDefaultPrinterHost;
  cfg.display_name = DefaultDisplayName();
  cfg.save_dir = ExpandHome(kDefaultSaveDir);
  // file_params/image_params/ocr_params/email_params keep brscan::Params's
  // own default constructor (color, 300dpi, flatbed).
  return cfg;
}

Config ParseConfig(const std::string& text) {
  Config cfg = DefaultConfig();

  std::istringstream lines(text);
  std::string raw_line;
  while (std::getline(lines, raw_line)) {
    const std::string line = Trim(raw_line);
    if (line.empty() || line[0] == '#') continue;

    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;

    const std::string key = Trim(line.substr(0, eq));
    const std::string value = Trim(line.substr(eq + 1));
    if (key.empty()) continue;

    ApplyKey(&cfg, key, value);
  }

  return cfg;
}

Config LoadConfig(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return DefaultConfig();

  std::ostringstream contents;
  contents << f.rdbuf();
  return ParseConfig(contents.str());
}

std::string DefaultConfigPath() {
  return ExpandHome("~/.config/brscan-scand.conf");
}

const brscan::Params& ParamsForFunc(const Config& cfg, const std::string& func) {
  if (func == kFuncImage) return cfg.image_params;
  if (func == kFuncOcr) return cfg.ocr_params;
  if (func == kFuncEmail) return cfg.email_params;
  return cfg.file_params;  // kFuncFile, and the safe fallback otherwise.
}

bool IsKnownFunc(const std::string& func) {
  return func == kFuncFile || func == kFuncImage || func == kFuncOcr ||
         func == kFuncEmail;
}

}  // namespace brscan::scand
