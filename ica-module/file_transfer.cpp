// Plan 2 Task 12 — pure filename / UTI plan for file-based transfer.
// See file_transfer.h for the contract and the clean-room note.

#include "file_transfer.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace brscan::ica {

namespace {

// Stable public ImageIO type identifiers for the panel's formats.
constexpr char kUtiTiff[] = "public.tiff";
constexpr char kUtiJpeg[] = "public.jpeg";
constexpr char kUtiPng[] = "public.png";

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// Trims ASCII whitespace and a single leading dot from an extension token.
std::string CleanExtension(const std::string& in) {
  std::string s = in;
  // Trim whitespace.
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  if (!s.empty() && s.front() == '.') s.erase(s.begin());
  return ToLower(s);
}

std::string Trim(const std::string& in) {
  std::string s = in;
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

// Maps a known filename extension to its UTI, or "" if unrecognised.
std::string UtiForExtension(const std::string& ext) {
  if (ext == "tif" || ext == "tiff") return kUtiTiff;
  if (ext == "jpg" || ext == "jpeg") return kUtiJpeg;
  if (ext == "png") return kUtiPng;
  return "";
}

// Maps a known UTI to its preferred extension, or "" if unrecognised.
std::string ExtensionForUti(const std::string& uti) {
  if (uti == kUtiTiff) return "tif";
  if (uti == kUtiJpeg) return "jpg";
  if (uti == kUtiPng) return "png";
  return "";
}

}  // namespace

TransferPlan PlanTransfer(const std::string& document_format,
                          const std::string& document_extension,
                          const std::string& document_name) {
  TransferPlan plan;
  const std::string uti = Trim(document_format);
  const std::string ext = CleanExtension(document_extension);

  // Resolve the UTI: honour the host's format; otherwise derive from the
  // extension; otherwise default to TIFF (the format the live panel showed).
  if (!uti.empty()) {
    plan.uti = uti;
  } else if (!ext.empty()) {
    const std::string derived = UtiForExtension(ext);
    plan.uti = derived.empty() ? std::string(kUtiTiff) : derived;
  } else {
    plan.uti = kUtiTiff;
  }

  // Resolve the extension: honour the host's extension; otherwise derive from
  // the resolved UTI; otherwise fall back to "tif".
  if (!ext.empty()) {
    plan.extension = ext;
  } else {
    const std::string derived = ExtensionForUti(plan.uti);
    plan.extension = derived.empty() ? std::string("tif") : derived;
  }

  // Resolve the stem: default to "Scan"; strip a trailing ".<extension>" so a
  // host that already put the extension in the name does not double it.
  std::string stem = Trim(document_name);
  if (stem.empty()) {
    stem = "Scan";
  } else {
    const std::string suffix = "." + plan.extension;
    if (stem.size() > suffix.size() &&
        ToLower(stem.substr(stem.size() - suffix.size())) == suffix) {
      stem.erase(stem.size() - suffix.size());
    }
  }
  plan.stem = stem;
  return plan;
}

std::string TransferFilenameForPage(const TransferPlan& plan, int page_index) {
  std::string name = plan.stem;
  if (page_index > 0) {
    name += " " + std::to_string(page_index);
  }
  name += "." + plan.extension;
  return name;
}

}  // namespace brscan::ica
