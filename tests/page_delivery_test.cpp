// Tests for the in-memory (in-app) page-boundary splitter
// (ica-module/page_delivery.h).
//
// A pure, hermetic unit: no ICADevices, no framework, no device. It maps a
// stream of scan-band page indexes (the order RunScan hands bands back) to the
// per-page ScannerPageDone boundaries the ICA module must post so a multi-page
// or duplex ADF scan viewed IN-APP lands each page as its own image instead of
// collapsing every page onto the first (PR B). Only the boundary DECISION lives
// here; posting the notification stays in module_main.mm.

#include "page_delivery.h"

#include <gtest/gtest.h>

#include <vector>

namespace brscan::ica {
namespace {

// Runs a whole page-index stream through the splitter and returns the finished
// page index reported at each boundary (in order). The size of the returned
// vector is the boundary count; each element is the PREVIOUS (finished) page.
std::vector<int> BoundariesFor(const std::vector<int>& page_indexes) {
  InMemoryPageSplitter splitter;
  std::vector<int> finalized;
  for (int page_index : page_indexes) {
    PageBoundary b = splitter.Observe(page_index);
    if (b.finalize_previous) finalized.push_back(b.page_index);
  }
  EXPECT_EQ(static_cast<int>(finalized.size()), splitter.boundaries());
  return finalized;
}

// ---------------------------------------------------------------------
// Single page / flatbed: no boundary, no regression.
// ---------------------------------------------------------------------

TEST(InMemoryPageSplitter, EmptyStreamHasNoBoundary) {
  EXPECT_TRUE(BoundariesFor({}).empty());
}

TEST(InMemoryPageSplitter, FirstBandNeverFinalizes) {
  InMemoryPageSplitter splitter;
  PageBoundary b = splitter.Observe(0);
  EXPECT_FALSE(b.finalize_previous);
  EXPECT_EQ(splitter.boundaries(), 0);
}

TEST(InMemoryPageSplitter, SinglePageManyBandsHasNoBoundary) {
  // A flatbed page decodes as hundreds of bands, all page_index 0. This must
  // emit zero boundaries so the MEMORY path stays byte-identical to pre-fix.
  EXPECT_TRUE(BoundariesFor({0, 0, 0, 0, 0, 0}).empty());
}

TEST(InMemoryPageSplitter, SinglePageNonZeroIndexHasNoBoundary) {
  EXPECT_TRUE(BoundariesFor({7, 7, 7}).empty());
}

// ---------------------------------------------------------------------
// Multi-page: N pages -> N-1 boundaries, each naming the FINISHED page.
// ---------------------------------------------------------------------

TEST(InMemoryPageSplitter, TwoPagesOneBoundaryNamesFirstPage) {
  // Page 0's bands, then page 1's: one boundary, reported as page 0 (finished),
  // fired on the first band of page 1 -- before that band is delivered.
  EXPECT_EQ(BoundariesFor({0, 0, 1, 1}), std::vector<int>({0}));
}

TEST(InMemoryPageSplitter, BoundaryReportsPreviousNotNewPage) {
  InMemoryPageSplitter splitter;
  splitter.Observe(0);              // first band of page 0
  PageBoundary b = splitter.Observe(1);  // first band of page 1
  ASSERT_TRUE(b.finalize_previous);
  EXPECT_EQ(b.page_index, 0);       // finished page is 0, not the new 1
}

TEST(InMemoryPageSplitter, ThreePagesTwoBoundaries) {
  EXPECT_EQ(BoundariesFor({0, 0, 1, 1, 2, 2}), std::vector<int>({0, 1}));
}

TEST(InMemoryPageSplitter, OneBandPerPage) {
  // Degenerate but valid: a single band per page still splits cleanly.
  EXPECT_EQ(BoundariesFor({0, 1, 2, 3}), std::vector<int>({0, 1, 2}));
}

// ---------------------------------------------------------------------
// Duplex ADF: front/back arrive as distinct, incrementing page indexes.
// ---------------------------------------------------------------------

TEST(InMemoryPageSplitter, DuplexInterleavedFrontBackPages) {
  // Two physical sheets duplex = four sides, page_index 0..3, each with a few
  // bands. Every side lands as its own image: three boundaries (0,1,2).
  EXPECT_EQ(BoundariesFor({0, 0, 1, 1, 2, 2, 3, 3}),
            std::vector<int>({0, 1, 2}));
}

// ---------------------------------------------------------------------
// Robustness: repeated and non-contiguous indexes.
// ---------------------------------------------------------------------

TEST(InMemoryPageSplitter, RepeatedIndexWithinPageIsNotABoundary) {
  // Only a CHANGE is a boundary; the run of equal indexes in the middle is not.
  EXPECT_EQ(BoundariesFor({0, 0, 0, 1}), std::vector<int>({0}));
}

TEST(InMemoryPageSplitter, NonContiguousJumpIsSingleBoundaryOfOpenPage) {
  // A skipped index (0 -> 2) still finalizes only the page that was open (0);
  // the tracker never invents a page it did not see.
  EXPECT_EQ(BoundariesFor({0, 0, 2, 2}), std::vector<int>({0}));
}

TEST(InMemoryPageSplitter, BoundaryCountMatchesReportedBoundaries) {
  InMemoryPageSplitter splitter;
  for (int p : {0, 0, 1, 2, 2, 2, 3}) splitter.Observe(p);
  // Boundaries at 0->1, 1->2, 2->3 == 3.
  EXPECT_EQ(splitter.boundaries(), 3);
}

}  // namespace
}  // namespace brscan::ica
