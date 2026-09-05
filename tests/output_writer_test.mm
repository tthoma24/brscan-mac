// Tests for daemon/output_writer.h/.mm: the library-level output writer
// that turns a multi-page scan (a list of brscan::ScanResult) into the
// configured file format (PDF / multi-page TIFF / numbered JPEG-PNG /
// native). Every input page here is a synthetic image built at runtime --
// a solid-color JPEG via libturbojpeg, a small gray buffer, a small
// bitonal buffer, or a CoreText-rendered gray raster of a known string --
// so no scanned content is ever committed to this repo. Outputs are read
// back with PDFKit (page counts, searchable text) and ImageIO (TIFF page
// count and compression tag, JPEG/PNG dimensions).

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <PDFKit/PDFKit.h>

#include <turbojpeg.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "brscan/scanner.h"
#include "brscan/types.h"
#include "output_writer.h"

namespace brscan::scand {
namespace {

std::filesystem::path TempPath(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("brscan_output_writer_test_" + name);
}

// A tiny solid-color baseline JPEG, generated at runtime with libturbojpeg
// (the same approach tests/scanner_test.cpp's MakeSyntheticJpeg uses, which
// lives in another translation unit's anonymous namespace and so isn't
// reachable from here). Carries no real image content.
std::vector<uint8_t> MakeSyntheticJpeg(int width, int height) {
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3, 128);
  tjhandle handle = tjInitCompress();
  unsigned char* jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;
  const int rc = tjCompress2(handle, rgb.data(), width, 0, height, TJPF_RGB,
                             &jpeg_buf, &jpeg_size, TJSAMP_444, 90,
                             TJFLAG_ACCURATEDCT);
  EXPECT_EQ(rc, 0);
  tjDestroy(handle);
  std::vector<uint8_t> out(jpeg_buf, jpeg_buf + jpeg_size);
  tjFree(jpeg_buf);
  return out;
}

brscan::ScanResult MakeRgbPage(int width, int height) {
  return brscan::ScanResult{brscan::PixelFormat::kRgb, width, height,
                            MakeSyntheticJpeg(width, height)};
}

brscan::ScanResult MakeGrayPage(int width, int height, uint8_t fill) {
  std::vector<uint8_t> data(static_cast<size_t>(width) * height, fill);
  return brscan::ScanResult{brscan::PixelFormat::kGray, width, height, data};
}

// A bitonal page whose packed 1-bit raster is a plausible black/white
// pattern (0xAA = alternating bits). Rows are byte-padded per
// PixelFormat::kBitonal's stride convention.
brscan::ScanResult MakeBitonalPage(int width, int height) {
  const size_t row_bytes = (static_cast<size_t>(width) + 7) / 8;
  std::vector<uint8_t> data(row_bytes * static_cast<size_t>(height), 0xAA);
  return brscan::ScanResult{brscan::PixelFormat::kBitonal, width, height, data};
}

// Renders `text` in black on white into an 8-bit grayscale kGray page, so
// the searchable-PDF path can be exercised against a runtime image
// containing a known, distinctive string (no committed scan content).
brscan::ScanResult RenderTextGrayPage(NSString* text, int width, int height) {
  std::vector<uint8_t> buf(static_cast<size_t>(width) * height, 255);
  CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceGray();
  CGContextRef ctx = CGBitmapContextCreate(
      buf.data(), static_cast<size_t>(width), static_cast<size_t>(height),
      /*bitsPerComponent=*/8, /*bytesPerRow=*/static_cast<size_t>(width),
      colorspace, kCGImageAlphaNone);
  CGColorSpaceRelease(colorspace);
  if (ctx != nullptr) {
    CGContextSetGrayFillColor(ctx, 0.0, 1.0);
    CGContextSetTextDrawingMode(ctx, kCGTextFill);
    CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica-Bold"), 60, nullptr);
    NSDictionary* attrs =
        @{(__bridge NSString*)kCTFontAttributeName : (__bridge id)font};
    NSAttributedString* attr_str =
        [[NSAttributedString alloc] initWithString:text attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString(
        (__bridge CFAttributedStringRef)attr_str);
    CGContextSetTextPosition(ctx, 30, static_cast<CGFloat>(height) / 2 - 20);
    CTLineDraw(line, ctx);
    CFRelease(line);
    CFRelease(font);
    CGContextRelease(ctx);
  }
  return brscan::ScanResult{brscan::PixelFormat::kGray, width, height, buf};
}

// The number of images in a TIFF (or any container ImageIO reads).
int ImageCount(const std::filesystem::path& path) {
  NSURL* url =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  CGImageSourceRef src =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
  if (src == nullptr) return -1;
  const int count = static_cast<int>(CGImageSourceGetCount(src));
  CFRelease(src);
  return count;
}

// The TIFF Compression tag of image `index` in `path`, or -1 if absent.
int TiffCompressionAt(const std::filesystem::path& path, size_t index) {
  NSURL* url =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  CGImageSourceRef src =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
  if (src == nullptr) return -1;
  NSDictionary* props = (__bridge_transfer NSDictionary*)
      CGImageSourceCopyPropertiesAtIndex(src, index, nullptr);
  CFRelease(src);
  NSDictionary* tiff =
      props[(__bridge NSString*)kCGImagePropertyTIFFDictionary];
  NSNumber* compression =
      tiff[(__bridge NSString*)kCGImagePropertyTIFFCompression];
  return compression != nil ? compression.intValue : -1;
}

// Decodes the single image at `path` and reports its pixel dimensions.
bool ImageDims(const std::filesystem::path& path, int* width, int* height) {
  NSURL* url =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  CGImageSourceRef src =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
  if (src == nullptr) return false;
  CGImageRef image = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
  CFRelease(src);
  if (image == nullptr) return false;
  *width = static_cast<int>(CGImageGetWidth(image));
  *height = static_cast<int>(CGImageGetHeight(image));
  CGImageRelease(image);
  return true;
}

int PdfPageCount(const std::filesystem::path& path) {
  NSURL* url =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  PDFDocument* doc = [[PDFDocument alloc] initWithURL:url];
  return doc != nil ? static_cast<int>(doc.pageCount) : -1;
}

void RemoveAll(const std::vector<std::string>& paths) {
  for (const std::string& p : paths) std::filesystem::remove(p);
}

// ---------------------------------------------------------------------
// PDF.
// ---------------------------------------------------------------------

TEST(WriteConfiguredOutputTest, PdfHasOnePagePerScanResult) {
  const std::vector<brscan::ScanResult> pages = {
      MakeGrayPage(20, 10, 200), MakeRgbPage(16, 8), MakeBitonalPage(24, 12)};
  OutputSettings settings;
  settings.format = OutputFormat::kPdf;

  const std::filesystem::path base = TempPath("multi.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 1u);
  EXPECT_EQ(written[0], TempPath("multi.pdf").string());
  EXPECT_EQ(PdfPageCount(written[0]), 3);
  RemoveAll(written);
}

TEST(WriteConfiguredOutputTest, SearchablePdfHasSelectableText) {
  const std::vector<brscan::ScanResult> pages = {
      RenderTextGrayPage(@"HELLO BRSCAN 12345", 1000, 220)};
  OutputSettings settings;
  settings.format = OutputFormat::kPdf;
  settings.searchable = true;

  const std::filesystem::path base = TempPath("searchable.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk)
      << "searchable PDF failed -- if this fails in a headless/CI-like "
         "environment, Vision text recognition may be unavailable there.";
  ASSERT_EQ(written.size(), 1u);

  NSURL* url = [NSURL
      fileURLWithPath:[NSString stringWithUTF8String:written[0].c_str()]];
  PDFDocument* doc = [[PDFDocument alloc] initWithURL:url];
  ASSERT_NE(doc, nil);
  EXPECT_EQ(static_cast<int>(doc.pageCount), 1);
  NSString* extracted = doc.string;
  ASSERT_NE(extracted, nil);
  const std::string text =
      extracted.UTF8String != nullptr ? extracted.UTF8String : "";
  EXPECT_NE(text.find("BRSCAN"), std::string::npos) << "extracted: " << text;
  EXPECT_NE(text.find("12345"), std::string::npos) << "extracted: " << text;
  RemoveAll(written);
}

// ---------------------------------------------------------------------
// TIFF.
// ---------------------------------------------------------------------

TEST(WriteConfiguredOutputTest, TiffHasOneImagePerPageAndLzwCompression) {
  const std::vector<brscan::ScanResult> pages = {MakeGrayPage(20, 10, 128),
                                                 MakeGrayPage(20, 10, 64)};
  OutputSettings settings;
  settings.format = OutputFormat::kTiff;
  settings.tiff_compression = TiffCompression::kLzw;

  const std::filesystem::path base = TempPath("tiff_lzw.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 1u);
  EXPECT_EQ(written[0], TempPath("tiff_lzw.tif").string());
  EXPECT_EQ(ImageCount(written[0]), 2);
  EXPECT_EQ(TiffCompressionAt(written[0], 0), 5);  // NSTIFFCompressionLZW.
  RemoveAll(written);
}

TEST(WriteConfiguredOutputTest, TiffG4OnBitonalPageUsesGroup4) {
  const std::vector<brscan::ScanResult> pages = {MakeBitonalPage(32, 16)};
  OutputSettings settings;
  settings.format = OutputFormat::kTiff;
  settings.tiff_compression = TiffCompression::kG4;

  const std::filesystem::path base = TempPath("tiff_g4.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 1u);
  EXPECT_EQ(ImageCount(written[0]), 1);
  EXPECT_EQ(TiffCompressionAt(written[0], 0), 4);  // CCITT Group 4.
  RemoveAll(written);
}

TEST(WriteConfiguredOutputTest, TiffG4OnGrayPageFallsBackToLzw) {
  // A kGray page is not bilevel, so a fax codec can't encode it: the writer
  // falls back to LZW for that page (documented in output_writer.h).
  const std::vector<brscan::ScanResult> pages = {MakeGrayPage(20, 10, 200)};
  OutputSettings settings;
  settings.format = OutputFormat::kTiff;
  settings.tiff_compression = TiffCompression::kG4;

  const std::filesystem::path base = TempPath("tiff_g4_fallback.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 1u);
  EXPECT_EQ(TiffCompressionAt(written[0], 0), 5);  // Fell back to LZW.
  RemoveAll(written);
}

// ---------------------------------------------------------------------
// JPEG / PNG (one numbered file per page).
// ---------------------------------------------------------------------

TEST(WriteConfiguredOutputTest, JpegWritesOneNumberedFilePerPage) {
  const std::vector<brscan::ScanResult> pages = {
      MakeGrayPage(20, 10, 200), MakeGrayPage(30, 15, 100)};
  OutputSettings settings;
  settings.format = OutputFormat::kJpeg;

  const std::filesystem::path base = TempPath("imgs.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 2u);
  EXPECT_EQ(written[0], TempPath("imgs-001.jpg").string());
  EXPECT_EQ(written[1], TempPath("imgs-002.jpg").string());

  int w = 0, h = 0;
  ASSERT_TRUE(ImageDims(written[0], &w, &h));
  EXPECT_EQ(w, 20);
  EXPECT_EQ(h, 10);
  ASSERT_TRUE(ImageDims(written[1], &w, &h));
  EXPECT_EQ(w, 30);
  EXPECT_EQ(h, 15);
  RemoveAll(written);
}

TEST(WriteConfiguredOutputTest, PngSinglePageIsNotNumbered) {
  const std::vector<brscan::ScanResult> pages = {MakeBitonalPage(24, 12)};
  OutputSettings settings;
  settings.format = OutputFormat::kPng;

  const std::filesystem::path base = TempPath("single.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 1u);
  EXPECT_EQ(written[0], TempPath("single.png").string());
  int w = 0, h = 0;
  ASSERT_TRUE(ImageDims(written[0], &w, &h));
  EXPECT_EQ(w, 24);
  EXPECT_EQ(h, 12);
  RemoveAll(written);
}

// ---------------------------------------------------------------------
// Document separation (kEveryImage / kEveryPage).
// ---------------------------------------------------------------------

TEST(WriteConfiguredOutputTest, PdfSeparationByImageCountSplitsIntoDocuments) {
  const std::vector<brscan::ScanResult> pages = {
      MakeGrayPage(20, 10, 10), MakeGrayPage(20, 10, 20),
      MakeGrayPage(20, 10, 30)};
  OutputSettings settings;
  settings.format = OutputFormat::kPdf;
  settings.separation = OutputSeparation::kEveryImage;
  settings.separate_n = 2;

  const std::filesystem::path base = TempPath("split.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 2u);
  EXPECT_EQ(written[0], TempPath("split-doc001.pdf").string());
  EXPECT_EQ(written[1], TempPath("split-doc002.pdf").string());
  EXPECT_EQ(PdfPageCount(written[0]), 2);  // First two pages.
  EXPECT_EQ(PdfPageCount(written[1]), 1);  // Remaining page.
  RemoveAll(written);
}

TEST(WriteConfiguredOutputTest, PdfSeparationByPageCountSplitsIntoDocuments) {
  // kEveryPage currently behaves identically to kEveryImage (split every N
  // ScanResults) -- see output_writer.h's OutputSeparation doc comment on
  // why duplex page-vs-sheet grouping is deliberately left unimplemented.
  const std::vector<brscan::ScanResult> pages = {
      MakeGrayPage(20, 10, 10), MakeGrayPage(20, 10, 20),
      MakeGrayPage(20, 10, 30)};
  OutputSettings settings;
  settings.format = OutputFormat::kPdf;
  settings.separation = OutputSeparation::kEveryPage;
  settings.separate_n = 2;

  const std::filesystem::path base = TempPath("split_page.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 2u);
  EXPECT_EQ(written[0], TempPath("split_page-doc001.pdf").string());
  EXPECT_EQ(written[1], TempPath("split_page-doc002.pdf").string());
  EXPECT_EQ(PdfPageCount(written[0]), 2);  // First two pages.
  EXPECT_EQ(PdfPageCount(written[1]), 1);  // Remaining page.
  RemoveAll(written);
}

TEST(WriteConfiguredOutputTest, TiffSeparationByImageCountSplitsIntoDocuments) {
  const std::vector<brscan::ScanResult> pages = {
      MakeGrayPage(20, 10, 10), MakeGrayPage(20, 10, 20),
      MakeGrayPage(20, 10, 30)};
  OutputSettings settings;
  settings.format = OutputFormat::kTiff;
  settings.separation = OutputSeparation::kEveryImage;
  settings.separate_n = 2;

  const std::filesystem::path base = TempPath("split_tiff.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 2u);
  EXPECT_EQ(ImageCount(written[0]), 2);
  EXPECT_EQ(ImageCount(written[1]), 1);
  RemoveAll(written);
}

TEST(WriteConfiguredOutputTest, TiffSeparationByPageCountSplitsIntoDocuments) {
  const std::vector<brscan::ScanResult> pages = {
      MakeGrayPage(20, 10, 10), MakeGrayPage(20, 10, 20),
      MakeGrayPage(20, 10, 30)};
  OutputSettings settings;
  settings.format = OutputFormat::kTiff;
  settings.separation = OutputSeparation::kEveryPage;
  settings.separate_n = 2;

  const std::filesystem::path base = TempPath("split_tiff_page.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 2u);
  EXPECT_EQ(ImageCount(written[0]), 2);
  EXPECT_EQ(ImageCount(written[1]), 1);
  RemoveAll(written);
}

TEST(WriteConfiguredOutputTest, CombineIgnoresSeparateN) {
  // kCombine always puts every page in one container, regardless of
  // separate_n -- separate_n only takes effect under kEveryImage/
  // kEveryPage.
  const std::vector<brscan::ScanResult> pages = {
      MakeGrayPage(20, 10, 10), MakeGrayPage(20, 10, 20),
      MakeGrayPage(20, 10, 30)};
  OutputSettings settings;
  settings.format = OutputFormat::kPdf;
  settings.separation = OutputSeparation::kCombine;
  settings.separate_n = 2;

  const std::filesystem::path base = TempPath("combine.jpg");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 1u);
  EXPECT_EQ(PdfPageCount(written[0]), 3);
  RemoveAll(written);
}

// ---------------------------------------------------------------------
// Native (delegates to brscan::cli::WritePages).
// ---------------------------------------------------------------------

TEST(WriteConfiguredOutputTest, NativeWritesPerFormatFilesNumbered) {
  const std::vector<brscan::ScanResult> pages = {MakeGrayPage(20, 10, 200),
                                                 MakeBitonalPage(24, 12)};
  OutputSettings settings;  // Defaults: kNative.

  const std::filesystem::path base = TempPath("native.pnm");
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput(pages, settings, base.string(), &written);

  ASSERT_EQ(status, brscan::Status::kOk);
  ASSERT_EQ(written.size(), 2u);
  EXPECT_EQ(written[0], TempPath("native-001.pnm").string());
  EXPECT_EQ(written[1], TempPath("native-002.pnm").string());
  EXPECT_TRUE(std::filesystem::exists(written[0]));
  EXPECT_TRUE(std::filesystem::exists(written[1]));
  RemoveAll(written);
}

TEST(WriteConfiguredOutputTest, EmptyPagesIsIoError) {
  OutputSettings settings;
  settings.format = OutputFormat::kPdf;
  std::vector<std::string> written;
  const brscan::Status status =
      WriteConfiguredOutput({}, settings, TempPath("empty.jpg").string(),
                            &written);
  EXPECT_EQ(status, brscan::Status::kIoError);
  EXPECT_TRUE(written.empty());
}

}  // namespace
}  // namespace brscan::scand
