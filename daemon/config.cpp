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

std::optional<OutputFormat> ParseFormatString(const std::string& s) {
  if (s == "pdf") return OutputFormat::kPdf;
  if (s == "tiff") return OutputFormat::kTiff;
  if (s == "jpeg") return OutputFormat::kJpeg;
  if (s == "png") return OutputFormat::kPng;
  if (s == "native") return OutputFormat::kNative;
  return std::nullopt;
}

std::optional<TiffCompression> ParseTiffCompressionString(const std::string& s) {
  if (s == "lzw") return TiffCompression::kLzw;
  if (s == "g3") return TiffCompression::kG3;
  if (s == "g4") return TiffCompression::kG4;
  return std::nullopt;
}

// Parses a `<dest>.remove_background` value into the level int that maps
// directly onto brscan::Params::remove_background_level (and onto
// remove_background(bool) as `level != 0`) -- see reference/protocol-notes-
// button-options.md's decode of the button config command's G=/L=: off has
// no L= at all (level 0), low/medium/high are L=64/128/192. Returns
// nullopt for anything else, so the caller leaves the default (0, off) in
// place, per ParseConfig()'s tolerant-parse contract.
std::optional<int> ParseRemoveBackgroundString(const std::string& s) {
  if (s == "off") return 0;
  if (s == "low") return 64;
  if (s == "medium") return 128;
  if (s == "high") return 192;
  return std::nullopt;
}

// Parses a `<dest>.separation` value into the vendor dialog's three-way
// Document Separation shape: "combine" or "off" (all pages in one
// container), "image:N" (a new document every N single-sided images), or
// "page:N" (a new document every N pages) -- N a positive int in both
// prefixed forms. "every:N" is also accepted, as a backward-compat alias
// for "image:N" (the sole mode this key offered before the vendor's
// image/page distinction was added). Returns nullopt for anything else, so
// the caller leaves the default (combine) in place, per ParseConfig()'s
// tolerant-parse contract.
//
// image:N and page:N currently produce the same behavior in
// WriteConfiguredOutput (split every N ScanResults) -- see
// output_writer.h's OutputSeparation doc comment for why the duplex
// page-vs-sheet question is deliberately left open rather than guessed at.
// Confirming the vendor's actual duplex grouping needs a capture: a duplex
// ADF scan-button press with "separate by page count = 1" set in the
// vendor driver (port 54921/54925), observing whether it groups two sides
// of one sheet into one document or starts a new document every side.
std::optional<OutputSeparation> ParseSeparationString(const std::string& s,
                                                      int* separate_n) {
  if (s == "combine" || s == "off") {
    *separate_n = 1;
    return OutputSeparation::kCombine;
  }
  constexpr char kImagePrefix[] = "image:";
  constexpr char kPagePrefix[] = "page:";
  constexpr char kEveryPrefix[] = "every:";  // Alias for image:N.

  const auto try_prefix = [&](const char* prefix, size_t prefix_len)
      -> std::optional<int> {
    if (s.compare(0, prefix_len, prefix) != 0) return std::nullopt;
    return ParsePositiveInt(s.substr(prefix_len));
  };

  if (const auto n = try_prefix(kImagePrefix, sizeof(kImagePrefix) - 1)) {
    *separate_n = *n;
    return OutputSeparation::kEveryImage;
  }
  if (const auto n = try_prefix(kPagePrefix, sizeof(kPagePrefix) - 1)) {
    *separate_n = *n;
    return OutputSeparation::kEveryPage;
  }
  if (const auto n = try_prefix(kEveryPrefix, sizeof(kEveryPrefix) - 1)) {
    *separate_n = *n;
    return OutputSeparation::kEveryImage;
  }
  return std::nullopt;
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

// Returns the OutputSettings this key's <dest> prefix names, or nullptr for
// anything else -- the counterpart to ParamsForDestPrefix for the output
// keys (format/tiff_compression/separation).
OutputSettings* OutputForDestPrefix(Config* cfg, const std::string& dest) {
  if (dest == "file") return &cfg->file_output;
  if (dest == "image") return &cfg->image_output;
  if (dest == "ocr") return &cfg->ocr_output;
  if (dest == "email") return &cfg->email_output;
  return nullptr;
}

// Returns the raw paper-token field this key's <dest> prefix names, or
// nullptr for anything else -- the counterpart to ParamsForDestPrefix/
// OutputForDestPrefix for the `<dest>.paper` key.
std::string* PaperForDestPrefix(Config* cfg, const std::string& dest) {
  if (dest == "file") return &cfg->file_paper;
  if (dest == "image") return &cfg->image_paper;
  if (dest == "ocr") return &cfg->ocr_paper;
  if (dest == "email") return &cfg->email_paper;
  return nullptr;
}

// Returns the raw remove-background-level field this key's <dest> prefix
// names, or nullptr for anything else -- the counterpart to
// PaperForDestPrefix for the `<dest>.remove_background` key.
int* RemoveBackgroundForDestPrefix(Config* cfg, const std::string& dest) {
  if (dest == "file") return &cfg->file_remove_background_level;
  if (dest == "image") return &cfg->image_remove_background_level;
  if (dest == "ocr") return &cfg->ocr_remove_background_level;
  if (dest == "email") return &cfg->email_remove_background_level;
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
  if (key == "image_app") {
    cfg->image_app = value;
    return;
  }
  if (key == "email_to") {
    cfg->email_to = value;
    return;
  }

  const size_t dot = key.find('.');
  if (dot == std::string::npos) return;
  const std::string dest = key.substr(0, dot);
  const std::string field = key.substr(dot + 1);

  brscan::Params* const params = ParamsForDestPrefix(cfg, dest);
  if (params != nullptr) {
    if (field == "mode") {
      if (const auto mode = ParseModeString(value)) params->mode = *mode;
      return;
    }
    if (field == "dpi") {
      if (const auto dpi = ParsePositiveInt(value)) {
        params->x_dpi = *dpi;
        params->y_dpi = *dpi;
      }
      return;
    }
    if (field == "source") {
      if (const auto src = ParseSourceString(value)) {
        params->source = src->source;
        params->duplex = src->duplex;
      }
      return;
    }
  }

  OutputSettings* const output = OutputForDestPrefix(cfg, dest);
  if (output != nullptr) {
    if (field == "format") {
      if (const auto fmt = ParseFormatString(value)) output->format = *fmt;
    } else if (field == "tiff_compression") {
      if (const auto comp = ParseTiffCompressionString(value)) {
        output->tiff_compression = *comp;
      }
    } else if (field == "separation") {
      int separate_n = output->separate_n;
      if (const auto sep = ParseSeparationString(value, &separate_n)) {
        output->separation = *sep;
        output->separate_n = separate_n;
      }
    }
  }

  if (field == "paper") {
    std::string* const paper = PaperForDestPrefix(cfg, dest);
    // Stored as-is, with no validation against daemon/paper_size.h's
    // table -- mapping/validating the token happens where it's
    // consumed (a later task), keeping this parser decoupled. An
    // explicit empty value (`file.paper=`) leaves the field empty,
    // same as its default.
    if (paper != nullptr) *paper = value;
    return;
  }

  if (field == "remove_background") {
    int* const level = RemoveBackgroundForDestPrefix(cfg, dest);
    if (level != nullptr) {
      if (const auto parsed_level = ParseRemoveBackgroundString(value)) {
        *level = *parsed_level;
      }
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

std::optional<Config> TryReloadConfig(const std::string& path) {
  Config cfg = LoadConfig(path);
  // Mirrors main()'s own startup check in tools/brscan-scand.cpp: an empty
  // printer_host (from a missing file, an emptied key, or a parse that
  // never set one) is never a usable Config to swap in.
  if (cfg.printer_host.empty()) return std::nullopt;
  return cfg;
}

const brscan::Params& ParamsForFunc(const Config& cfg, const std::string& func) {
  if (func == kFuncImage) return cfg.image_params;
  if (func == kFuncOcr) return cfg.ocr_params;
  if (func == kFuncEmail) return cfg.email_params;
  return cfg.file_params;  // kFuncFile, and the safe fallback otherwise.
}

const OutputSettings& OutputSettingsForFunc(const Config& cfg,
                                            const std::string& func) {
  if (func == kFuncImage) return cfg.image_output;
  if (func == kFuncOcr) return cfg.ocr_output;
  if (func == kFuncEmail) return cfg.email_output;
  return cfg.file_output;  // kFuncFile, and the safe fallback otherwise.
}

const std::string& PaperForFunc(const Config& cfg, const std::string& func) {
  if (func == kFuncImage) return cfg.image_paper;
  if (func == kFuncOcr) return cfg.ocr_paper;
  if (func == kFuncEmail) return cfg.email_paper;
  return cfg.file_paper;  // kFuncFile, and the safe fallback otherwise.
}

int RemoveBackgroundLevelForFunc(const Config& cfg, const std::string& func) {
  if (func == kFuncImage) return cfg.image_remove_background_level;
  if (func == kFuncOcr) return cfg.ocr_remove_background_level;
  if (func == kFuncEmail) return cfg.email_remove_background_level;
  return cfg.file_remove_background_level;  // kFuncFile, and the safe
                                             // fallback otherwise.
}

bool IsKnownFunc(const std::string& func) {
  return func == kFuncFile || func == kFuncImage || func == kFuncOcr ||
         func == kFuncEmail;
}

}  // namespace brscan::scand
