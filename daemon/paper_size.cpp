#include "paper_size.h"

#include <cmath>

namespace brscan::scand {

namespace {

// One paper size's captured scan area at 300 dpi (y0 is always 0 -- see
// the table below). AreaForPaper() scales x0/x1/y1 linearly by dpi/300.0
// and rounds to the nearest int; this reproduces the capture exactly at
// 300 dpi and lands within a few px elsewhere (Brother applies its own
// per-dpi rounding that this project doesn't chase -- the device clips to
// the physical page, so a few px of drift is harmless). This is
// deliberately NOT derived from each paper's public nominal dimensions
// plus a margin formula: the captured tuples are the ground truth (see
// PROVENANCE.md and the controller's Task 1d.2 design ruling).
struct PaperEntry {
  const char* token;
  int x0_at_300;
  int x1_at_300;
  int y1_at_300;
};

// All @300 dpi, y0 = 0. Tokens are exact, case-sensitive matches for a
// config command's `P=` value (see daemon/button_config.h).
//
// LETTER/LEGAL/A4/LEDGER center-register within the ADF sensor's full
// width (xmax = 3472 px @300) -- that centering is already baked into
// each row's x0 below, nothing here computes it. A3 fills that width
// (x0 = 0, x1 = xmax). The flatbed-only sizes (A5/EXECUTIVE/PHOTO/BCARD)
// corner-register at x0 = 0 instead. See the Task 1d.2 brief for the
// capture this table is transcribed from.
constexpr PaperEntry kPaperTable[] = {
    {"LETTER", 478, 2990, 3253},      // 8.5 x 11 in
    {"LEGAL", 478, 2990, 4153},       // 8.5 x 14 in
    {"A4", 513, 2961, 3461},          // 210 x 297 mm
    {"LEDGER", 103, 3367, 5053},      // 11 x 17 in
    {"A3", 0, 3472, 4913},            // 297 x 420 mm
    {"A5", 0, 1712, 2433},            // 148 x 210 mm
    {"EXECUTIVE", 0, 2128, 3103},     // 7.25 x 10.5 in
    {"PHOTO", 0, 1168, 1753},         // 4 x 6 in
    {"BCARD", 0, 1024, 661},          // business card
};

constexpr int kCaptureDpi = 300;

const PaperEntry* FindEntry(const std::string& paper_token) {
  for (const PaperEntry& entry : kPaperTable) {
    if (paper_token == entry.token) return &entry;
  }
  return nullptr;
}

// Scales a single @300dpi coordinate to `dpi`, rounding to the nearest
// int (lround) per the design's rounding contract.
int ScaleCoord(int coord_at_300, int dpi) {
  return static_cast<int>(
      std::lround(coord_at_300 * (static_cast<double>(dpi) / kCaptureDpi)));
}

}  // namespace

std::optional<brscan::Area> AreaForPaper(const std::string& paper_token,
                                          int dpi) {
  if (dpi <= 0) return std::nullopt;
  const PaperEntry* const entry = FindEntry(paper_token);
  if (entry == nullptr) return std::nullopt;

  brscan::Area area;
  area.x0 = ScaleCoord(entry->x0_at_300, dpi);
  area.y0 = 0;
  area.x1 = ScaleCoord(entry->x1_at_300, dpi);
  area.y1 = ScaleCoord(entry->y1_at_300, dpi);
  return area;
}

bool IsKnownPaper(const std::string& paper_token) {
  return FindEntry(paper_token) != nullptr;
}

}  // namespace brscan::scand
