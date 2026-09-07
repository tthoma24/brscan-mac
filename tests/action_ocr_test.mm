// Tests for daemon/action_ocr.h/.mm: the OCR destination action's core.
//
// The important test here is OcrImageToSearchablePdfTest.RecognizesTextAndProducesSearchablePdf:
// it renders a synthetic grayscale bitmap containing a known, distinctive
// string at runtime (no scanned content is ever committed to this repo),
// saves it as a PGM (P5) -- exercising the PNM-loading path too -- runs
// it through the real Vision-backed OcrImageToSearchablePdf, and then
// reads the resulting PDF back with PDFKit to confirm the text layer is
// actually searchable. This is the real Vision -> PDF path end to end,
// not a stub.

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <PDFKit/PDFKit.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "action_ocr.h"

namespace brscan {
namespace {

std::filesystem::path TempPath(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("brscan_action_ocr_test_" + name);
}

void WriteFile(const std::filesystem::path& path, const uint8_t* data,
               size_t len) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(f) << "could not open " << path;
  f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
  ASSERT_TRUE(f);
}

// ---------------------------------------------------------------------
// PNM-loading tests: small synthetic P5 and P4 files, checked only for
// the dimensions LoadImageAsCGImage() reports back -- this is the helper
// OcrImageToSearchablePdf relies on to read a gray/bw scan (as opposed to
// a JPEG, which goes through ImageIO instead).
// ---------------------------------------------------------------------

TEST(LoadImageAsCGImageTest, LoadsSyntheticP5Gray) {
  const int width = 4, height = 3;
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height);
  for (size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<uint8_t>(i * 17);
  }

  std::ostringstream header;
  header << "P5\n" << width << " " << height << "\n255\n";
  const std::string header_str = header.str();

  const std::filesystem::path path = TempPath("p5.pgm");
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(f);
  f << header_str;
  f.write(reinterpret_cast<const char*>(pixels.data()),
          static_cast<std::streamsize>(pixels.size()));
  f.close();

  CGImageRef image = LoadImageAsCGImage(path.string());
  std::filesystem::remove(path);

  ASSERT_NE(image, nullptr);
  EXPECT_EQ(static_cast<int>(CGImageGetWidth(image)), width);
  EXPECT_EQ(static_cast<int>(CGImageGetHeight(image)), height);
  CGImageRelease(image);
}

TEST(LoadImageAsCGImageTest, LoadsSyntheticP4Bitonal) {
  const int width = 10, height = 2;
  const size_t row_bytes = (static_cast<size_t>(width) + 7) / 8;  // 2
  std::vector<uint8_t> packed(row_bytes * height, 0xAA);

  std::ostringstream header;
  header << "P4\n" << width << " " << height << "\n";
  const std::string header_str = header.str();

  const std::filesystem::path path = TempPath("p4.pbm");
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(f);
  f << header_str;
  f.write(reinterpret_cast<const char*>(packed.data()),
          static_cast<std::streamsize>(packed.size()));
  f.close();

  CGImageRef image = LoadImageAsCGImage(path.string());
  std::filesystem::remove(path);

  ASSERT_NE(image, nullptr);
  EXPECT_EQ(static_cast<int>(CGImageGetWidth(image)), width);
  EXPECT_EQ(static_cast<int>(CGImageGetHeight(image)), height);
  CGImageRelease(image);
}

TEST(LoadImageAsCGImageTest, ReturnsNullForMissingFile) {
  CGImageRef image =
      LoadImageAsCGImage("/nonexistent/does-not-exist-brscan-test.pgm");
  EXPECT_EQ(image, nullptr);
}

TEST(LoadImageAsCGImageTest, ReturnsNullForTruncatedP5Raster) {
  // Header claims 100x100 (10000 bytes) but the file has almost none of
  // that data -- LoadImageAsCGImage must notice and refuse, not read
  // past the buffer.
  const std::string content = "P5\n100 100\n255\n\x01\x02\x03";
  const std::filesystem::path path = TempPath("truncated.pgm");
  WriteFile(path, reinterpret_cast<const uint8_t*>(content.data()),
            content.size());

  CGImageRef image = LoadImageAsCGImage(path.string());
  std::filesystem::remove(path);

  EXPECT_EQ(image, nullptr);
}

// ---------------------------------------------------------------------
// The real Vision -> searchable-PDF path.
// ---------------------------------------------------------------------

// Renders `text` in black on a white background into an 8-bit grayscale
// raster of `width`x`height` pixels (row-major, top-down -- the same
// layout tools/scan_output.cpp's WriteOutput uses for a PGM/P5 file,
// which is exactly what this writes out as) using CoreText, so the test
// exercises the real Vision text-recognition path against a runtime
// image containing no committed scan content.
std::vector<uint8_t> RenderTextToGray8(NSString* text, int width, int height) {
  std::vector<uint8_t> buf(static_cast<size_t>(width) * height, 255);

  CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceGray();
  CGContextRef ctx = CGBitmapContextCreate(
      buf.data(), static_cast<size_t>(width), static_cast<size_t>(height),
      /*bitsPerComponent=*/8, /*bytesPerRow=*/static_cast<size_t>(width),
      colorspace, kCGImageAlphaNone);
  CGColorSpaceRelease(colorspace);
  if (ctx == nullptr) return buf;

  CGContextSetGrayFillColor(ctx, 0.0, 1.0);  // black text.
  CGContextSetTextDrawingMode(ctx, kCGTextFill);

  CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica-Bold"), 60, nullptr);
  NSDictionary* attrs = @{(__bridge NSString*)kCTFontAttributeName :
                               (__bridge id)font};
  NSAttributedString* attr_str =
      [[NSAttributedString alloc] initWithString:text attributes:attrs];
  CTLineRef line =
      CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attr_str);

  CGContextSetTextPosition(ctx, 30, static_cast<CGFloat>(height) / 2 - 20);
  CTLineDraw(line, ctx);

  CFRelease(line);
  CFRelease(font);
  CGContextRelease(ctx);
  return buf;
}

void WritePgm(const std::filesystem::path& path, const std::vector<uint8_t>& gray8,
              int width, int height) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(f) << "could not open " << path;
  f << "P5\n" << width << " " << height << "\n255\n";
  f.write(reinterpret_cast<const char*>(gray8.data()),
          static_cast<std::streamsize>(gray8.size()));
  ASSERT_TRUE(f);
}

TEST(OcrImageToSearchablePdfTest, RecognizesTextAndProducesSearchablePdf) {
  const int width = 1000, height = 220;
  NSString* known_text = @"HELLO BRSCAN 12345";
  const std::vector<uint8_t> gray8 = RenderTextToGray8(known_text, width, height);

  const std::filesystem::path image_path = TempPath("ocr_source.pgm");
  const std::filesystem::path pdf_path = TempPath("ocr_output.pdf");
  std::filesystem::remove(pdf_path);
  WritePgm(image_path, gray8, width, height);

  const Status status =
      OcrImageToSearchablePdf(image_path.string(), pdf_path.string());

  ASSERT_EQ(status, Status::kOk)
      << "OcrImageToSearchablePdf failed -- see stderr for the Vision "
         "error, if any. If this fails in a headless/CI-like environment "
         "with no other diagnostic, Vision's text recognition may not be "
         "available there.";
  ASSERT_TRUE(std::filesystem::exists(pdf_path));

  NSURL* pdf_url =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:pdf_path.c_str()]];
  PDFDocument* doc = [[PDFDocument alloc] initWithURL:pdf_url];
  ASSERT_NE(doc, nil) << "produced file is not a valid PDF";
  EXPECT_EQ(static_cast<int>(doc.pageCount), 1);

  NSString* extracted = doc.string;
  ASSERT_NE(extracted, nil);
  const std::string extracted_str = extracted.UTF8String != nullptr
                                         ? std::string(extracted.UTF8String)
                                         : std::string();

  // Allow for minor OCR variance (spacing, case) by checking for two
  // distinctive tokens rather than an exact match.
  EXPECT_NE(extracted_str.find("BRSCAN"), std::string::npos)
      << "extracted text was: " << extracted_str;
  EXPECT_NE(extracted_str.find("12345"), std::string::npos)
      << "extracted text was: " << extracted_str;

  std::filesystem::remove(image_path);
  std::filesystem::remove(pdf_path);
}

// ---------------------------------------------------------------------
// WriteRecognizedText: TXT / HTML / RTF text sinks.
// ---------------------------------------------------------------------

// Wraps `gray8` (width*height 8-bit grayscale, row-major) in a CGImageRef
// for the Vision-backed WriteRecognizedText path. The caller owns the
// image.
CGImageRef MakeGray8CGImage(const std::vector<uint8_t>& gray8, int width,
                            int height) {
  CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceGray();
  CFDataRef cfdata = CFDataCreate(kCFAllocatorDefault, gray8.data(),
                                  static_cast<CFIndex>(gray8.size()));
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(cfdata);
  CFRelease(cfdata);
  CGImageRef image = CGImageCreate(
      static_cast<size_t>(width), static_cast<size_t>(height),
      /*bitsPerComponent=*/8, /*bitsPerPixel=*/8,
      /*bytesPerRow=*/static_cast<size_t>(width), colorspace,
      kCGBitmapByteOrderDefault | kCGImageAlphaNone, provider,
      /*decode=*/nullptr, /*shouldInterpolate=*/false,
      kCGRenderingIntentDefault);
  CGDataProviderRelease(provider);
  CGColorSpaceRelease(colorspace);
  return image;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

TEST(WriteRecognizedTextTest, PlainTextContainsRecognizedString) {
  const int width = 1000, height = 220;
  NSString* known_text = @"HELLO BRSCAN 12345";
  const std::vector<uint8_t> gray8 = RenderTextToGray8(known_text, width, height);
  CGImageRef image = MakeGray8CGImage(gray8, width, height);
  ASSERT_NE(image, nullptr);

  const std::filesystem::path path = TempPath("recognized.txt");
  std::filesystem::remove(path);
  const Status status =
      WriteRecognizedText({image}, OcrTextFormat::kPlain, path.string());
  CGImageRelease(image);

  ASSERT_EQ(status, Status::kOk)
      << "WriteRecognizedText failed -- Vision may be unavailable in a "
         "headless/CI-like environment.";
  ASSERT_TRUE(std::filesystem::exists(path));
  const std::string contents = ReadFile(path);
  EXPECT_NE(contents.find("BRSCAN"), std::string::npos)
      << "text file was: " << contents;
  EXPECT_NE(contents.find("12345"), std::string::npos)
      << "text file was: " << contents;
  std::filesystem::remove(path);
}

TEST(WriteRecognizedTextTest, HtmlDocumentParsesAndContainsRecognizedString) {
  const int width = 1000, height = 220;
  NSString* known_text = @"HELLO BRSCAN 12345";
  const std::vector<uint8_t> gray8 = RenderTextToGray8(known_text, width, height);
  CGImageRef image = MakeGray8CGImage(gray8, width, height);
  ASSERT_NE(image, nullptr);

  const std::filesystem::path path = TempPath("recognized.html");
  std::filesystem::remove(path);
  const Status status =
      WriteRecognizedText({image}, OcrTextFormat::kHtml, path.string());
  CGImageRelease(image);

  ASSERT_EQ(status, Status::kOk);
  ASSERT_TRUE(std::filesystem::exists(path));

  // Round-trip: Foundation must be able to parse the file back as HTML, and
  // its plain-text content must carry the recognized tokens.
  NSData* data =
      [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:
                                                     path.string().c_str()]];
  ASSERT_NE(data, nil);
  NSError* error = nil;
  NSAttributedString* parsed = [[NSAttributedString alloc]
        initWithData:data
             options:@{NSDocumentTypeDocumentAttribute : NSHTMLTextDocumentType}
      documentAttributes:nil
               error:&error];
  ASSERT_NE(parsed, nil) << "produced file did not parse as HTML";
  const std::string text = parsed.string.UTF8String;
  EXPECT_NE(text.find("BRSCAN"), std::string::npos) << "html text was: " << text;
  EXPECT_NE(text.find("12345"), std::string::npos) << "html text was: " << text;
  std::filesystem::remove(path);
}

TEST(WriteRecognizedTextTest, RtfDocumentRoundTripsRecognizedString) {
  const int width = 1000, height = 220;
  NSString* known_text = @"HELLO BRSCAN 12345";
  const std::vector<uint8_t> gray8 = RenderTextToGray8(known_text, width, height);
  CGImageRef image = MakeGray8CGImage(gray8, width, height);
  ASSERT_NE(image, nullptr);

  const std::filesystem::path path = TempPath("recognized.rtf");
  std::filesystem::remove(path);
  const Status status =
      WriteRecognizedText({image}, OcrTextFormat::kRtf, path.string());
  CGImageRelease(image);

  ASSERT_EQ(status, Status::kOk);
  ASSERT_TRUE(std::filesystem::exists(path));

  NSData* data =
      [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:
                                                     path.string().c_str()]];
  ASSERT_NE(data, nil);
  NSError* error = nil;
  NSAttributedString* parsed = [[NSAttributedString alloc]
        initWithData:data
             options:@{NSDocumentTypeDocumentAttribute : NSRTFTextDocumentType}
      documentAttributes:nil
               error:&error];
  ASSERT_NE(parsed, nil) << "produced file did not parse as RTF";
  const std::string text = parsed.string.UTF8String;
  EXPECT_NE(text.find("BRSCAN"), std::string::npos) << "rtf text was: " << text;
  EXPECT_NE(text.find("12345"), std::string::npos) << "rtf text was: " << text;
  std::filesystem::remove(path);
}

TEST(WriteRecognizedTextTest, EmptyImagesIsIoError) {
  const std::filesystem::path path = TempPath("recognized_empty.txt");
  std::filesystem::remove(path);
  const Status status =
      WriteRecognizedText({}, OcrTextFormat::kPlain, path.string());
  EXPECT_EQ(status, Status::kIoError);
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(OcrImageToSearchablePdfTest, MissingSourceImageIsIoError) {
  const std::filesystem::path pdf_path = TempPath("ocr_missing_output.pdf");
  std::filesystem::remove(pdf_path);

  const Status status = OcrImageToSearchablePdf(
      "/nonexistent/does-not-exist-brscan-test.jpg", pdf_path.string());

  EXPECT_EQ(status, Status::kIoError);
  EXPECT_FALSE(std::filesystem::exists(pdf_path));
}

}  // namespace
}  // namespace brscan
