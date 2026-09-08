// Pure page-boundary decision for the ICA module's MEMORY / in-app delivery
// path. Split out of module_main.mm's RunScanSynchronous so the "where does one
// page end and the next begin" logic is unit-testable without the ICADevices
// framework or a live device (PR B).
//
// WHY THIS EXISTS. When a scan is viewed IN-APP -- in the Image Capture window,
// with no file destination (`fileTransfer == false`) -- the decoded bands ARE
// the delivery: each band is pushed as a kICANotificationTypeScanProgressStatus
// carrying the full page width/height plus this band's row offset/count, and the
// host accumulates those rows into ONE overview/preview image. Every page's
// bands restart at row 0, so a multi-page or duplex ADF scan streamed this way
// paints page 2..N on top of page 1 -- pages after the first are lost -- unless
// the module tells the host where each page ends. The FILE path never had this
// problem: it posts a ScannerPageDone (carrying the written file path) per page.
//
// This tracker is that boundary decision for the MEMORY path. Feed it each
// band's page_index in arrival order; when a band belongs to a different page
// than the previous band, the page that was being accumulated is complete, and
// the caller posts a (path-less) ScannerPageDone for it BEFORE delivering the
// new band -- so the host finalizes the current image and starts a fresh one.
//
// The LAST page is deliberately NOT finalized here: the terminating
// ScannerScanDone closes it. That keeps the single-page / flatbed case
// byte-identical to the pre-fix behavior (zero ScannerPageDone on the MEMORY
// path), so only a genuinely multi-page scan emits any boundary signal -- N
// pages produce exactly N-1 boundaries.
//
// Clean-room: written against libbrscan's public brscan::ScanBand page ordering
// (libbrscan/include/brscan/scanner.h) only. The ICA notification the caller
// posts on a boundary stays in module_main.mm.

#ifndef BRSCAN_ICA_PAGE_DELIVERY_H_
#define BRSCAN_ICA_PAGE_DELIVERY_H_

namespace brscan {
namespace ica {

// The decision for one observed band. `finalize_previous` is true when the page
// that was being accumulated must be finalized (a ScannerPageDone posted for
// `page_index`) before this band's rows are delivered; false for the very first
// band of a scan and for any band that continues the current page. `page_index`
// is meaningful only when `finalize_previous` is true, and it is the FINISHED
// (previous) page's index -- not the new page's.
struct PageBoundary {
  bool finalize_previous = false;
  int page_index = 0;
};

// Tracks the current in-memory page across a band stream (see file comment). A
// change in page_index between consecutive bands is a boundary; a repeated
// page_index (many bands per page) is not. Any change counts as one boundary,
// so a non-contiguous jump (e.g. a skipped page index) is still a single
// finalize of the page that was open -- the tracker never invents pages it did
// not see.
class InMemoryPageSplitter {
 public:
  // Observe the next band's page_index, in arrival order.
  PageBoundary Observe(int page_index) {
    PageBoundary boundary;
    if (have_current_ && page_index != current_page_index_) {
      boundary.finalize_previous = true;
      boundary.page_index = current_page_index_;
      ++boundaries_;
    }
    have_current_ = true;
    current_page_index_ = page_index;
    return boundary;
  }

  // Number of boundaries observed so far == ScannerPageDone posts emitted ==
  // pages finalized mid-scan. For N pages this settles at N-1 (the final page
  // is closed by ScannerScanDone), and stays 0 for a single-page scan.
  int boundaries() const { return boundaries_; }

 private:
  bool have_current_ = false;
  int current_page_index_ = 0;
  int boundaries_ = 0;
};

}  // namespace ica
}  // namespace brscan

#endif  // BRSCAN_ICA_PAGE_DELIVERY_H_
