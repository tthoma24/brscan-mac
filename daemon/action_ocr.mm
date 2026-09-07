// Objective-C++ implementation of the OCR destination action's core (see
// action_ocr.h). ARC is enabled for this translation unit only (see
// CMakeLists.txt's COMPILE_OPTIONS on this source file) to manage the
// Objective-C objects below (NSData, VN*, NS*); Core Foundation and
// CoreGraphics/CoreText opaque types (CGImageRef, CFDataRef, CGContextRef,
// CTFontRef, CTLineRef, ...) are never ARC-managed regardless, and are
// retained/released explicitly throughout.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import <CoreText/CoreText.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "action_ocr.h"

namespace brscan {

namespace {

// ---------------------------------------------------------------------
// PNM (P4/P5) parsing.
//
// Both PNM variants this project's own writer produces (tools/
// scan_output.cpp's WriteOutput) have a whitespace/comment-tolerant
// ASCII header: magic, width, height, and (P5 only) maxval, each
// separated by whitespace, followed by exactly one whitespace byte and
// then the raw raster. The parsing below is intentionally a little more
// tolerant than strictly necessary (comments, arbitrary whitespace)
// since the PNM spec allows it and it costs nothing -- but it is not a
// general-purpose PNM reader (no P1/P2/P3/P6, no maxval other than
// single-byte samples), only what WriteOutput ever emits.
// ---------------------------------------------------------------------

bool IsPnmSpace(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Reads the next whitespace/comment-delimited token from `data` starting
// at `*pos`, advancing `*pos` to just past the token (NOT past any
// trailing whitespace -- the caller decides how much of that to skip,
// since PNM only guarantees a single separator byte before the raster).
// Returns false if no token remains before EOF.
bool NextPnmToken(const std::vector<uint8_t>& data, size_t* pos,
                   std::string* token) {
  size_t i = *pos;
  for (;;) {
    while (i < data.size() && IsPnmSpace(data[i])) ++i;
    if (i < data.size() && data[i] == '#') {
      while (i < data.size() && data[i] != '\n') ++i;
      continue;
    }
    break;
  }
  const size_t start = i;
  while (i < data.size() && !IsPnmSpace(data[i]) && data[i] != '#') ++i;
  *pos = i;
  if (i == start) return false;
  *token = std::string(data.begin() + static_cast<long>(start),
                        data.begin() + static_cast<long>(i));
  return true;
}

struct PnmHeader {
  char type;  // '4' (PBM) or '5' (PGM).
  int width;
  int height;
  size_t data_offset;  // Byte offset of the raw raster within `data`.
};

bool ParseInt(const std::string& s, int* out) {
  if (s.empty()) return false;
  try {
    size_t consumed = 0;
    *out = std::stoi(s, &consumed);
    return consumed == s.size();
  } catch (...) {
    return false;
  }
}

bool ParsePnmHeader(const std::vector<uint8_t>& data, PnmHeader* out) {
  if (data.size() < 2 || data[0] != 'P') return false;
  const char type = static_cast<char>(data[1]);
  if (type != '4' && type != '5') return false;

  size_t pos = 2;
  std::string width_tok, height_tok, maxval_tok;
  if (!NextPnmToken(data, &pos, &width_tok)) return false;
  if (!NextPnmToken(data, &pos, &height_tok)) return false;
  if (type == '5') {
    // maxval is required for P5 but this project's writer only ever
    // emits 255 (one byte per sample); parse and ignore rather than
    // trust it for anything other than "did a token exist here".
    if (!NextPnmToken(data, &pos, &maxval_tok)) return false;
  }

  int width = 0, height = 0;
  if (!ParseInt(width_tok, &width) || !ParseInt(height_tok, &height)) {
    return false;
  }
  if (width <= 0 || height <= 0) return false;

  // Exactly one whitespace byte separates the last header token from the
  // raster (the PNM spec's requirement, and exactly what WriteOutput
  // emits). `pos` currently points at that byte (NextPnmToken stops
  // right after the token, before consuming trailing whitespace).
  if (pos >= data.size() || !IsPnmSpace(data[pos])) return false;
  pos += 1;

  out->type = type;
  out->width = width;
  out->height = height;
  out->data_offset = pos;
  return true;
}

// Expands a PBM (P4) 1-bit-per-pixel raster (MSB-first, 1=black --
// PixelFormat::kBitonal's convention, see brscan/types.h) to 8-bit
// grayscale (0=black, 255=white), one byte per pixel, row-major.
std::vector<uint8_t> ExpandBitonalToGray8(const uint8_t* packed, int width,
                                           int height) {
  const size_t row_bytes = (static_cast<size_t>(width) + 7) / 8;
  std::vector<uint8_t> out(static_cast<size_t>(width) *
                            static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = packed + static_cast<size_t>(y) * row_bytes;
    for (int x = 0; x < width; ++x) {
      const uint8_t byte = row[static_cast<size_t>(x) / 8];
      const int bit = (byte >> (7 - (x % 8))) & 1;
      out[static_cast<size_t>(y) * static_cast<size_t>(width) +
          static_cast<size_t>(x)] = bit ? 0 : 255;
    }
  }
  return out;
}

// Wraps `pixels` (width*height 8-bit grayscale samples, row-major, one
// byte per pixel) in a CGImageRef. `pixels` is copied (CFDataCreate), so
// the caller's buffer need not outlive the call.
CGImageRef CreateCGImageFromGray8(const uint8_t* pixels, int width,
                                   int height) {
  CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceGray();
  CFDataRef cfdata = CFDataCreate(
      kCFAllocatorDefault, pixels,
      static_cast<CFIndex>(static_cast<size_t>(width) *
                            static_cast<size_t>(height)));
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(cfdata);
  CFRelease(cfdata);  // provider retains its own reference.

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

// Decodes an encoded image byte stream (in practice the baseline JPEG a
// color scan is delivered as) to a CGImageRef via ImageIO, from an
// in-memory buffer -- no file round-trip. Returns nullptr if ImageIO
// cannot make a source or decode the first image. Shared by both
// LoadImageAsCGImage (its non-PNM branch) and CreateCGImageFromScanResult
// (its kRgb branch) so the ImageIO decode lives in exactly one place.
CGImageRef CreateCGImageFromEncodedBytes(const uint8_t* data, size_t len) {
  NSData* ns_data = [NSData dataWithBytes:data length:len];
  CGImageSourceRef source =
      CGImageSourceCreateWithData((__bridge CFDataRef)ns_data, nullptr);
  if (source == nullptr) return nullptr;
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  return image;
}

CGImageRef LoadPnmAsCGImage(const std::vector<uint8_t>& data) {
  PnmHeader hdr;
  if (!ParsePnmHeader(data, &hdr)) return nullptr;

  if (hdr.type == '5') {
    const size_t needed =
        static_cast<size_t>(hdr.width) * static_cast<size_t>(hdr.height);
    if (data.size() - hdr.data_offset < needed) return nullptr;
    return CreateCGImageFromGray8(data.data() + hdr.data_offset, hdr.width,
                                   hdr.height);
  }

  // '4' (PBM).
  const size_t row_bytes = (static_cast<size_t>(hdr.width) + 7) / 8;
  const size_t needed = row_bytes * static_cast<size_t>(hdr.height);
  if (data.size() - hdr.data_offset < needed) return nullptr;
  const std::vector<uint8_t> gray8 = ExpandBitonalToGray8(
      data.data() + hdr.data_offset, hdr.width, hdr.height);
  return CreateCGImageFromGray8(gray8.data(), hdr.width, hdr.height);
}

}  // namespace

CGImageRef LoadImageAsCGImage(const std::string& image_path) {
  std::ifstream f(image_path, std::ios::binary);
  if (!f) return nullptr;
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
  if (data.empty()) return nullptr;

  if (data.size() >= 2 && data[0] == 'P' &&
      (data[1] == '4' || data[1] == '5')) {
    return LoadPnmAsCGImage(data);
  }

  // Anything else (in practice, the JPEG a color scan was saved as) goes
  // through ImageIO.
  return CreateCGImageFromEncodedBytes(data.data(), data.size());
}

CGImageRef CreateCGImageFromScanResult(const brscan::ScanResult& page) {
  const size_t width = static_cast<size_t>(page.width);
  const size_t height = static_cast<size_t>(page.height);

  switch (page.format) {
    case brscan::PixelFormat::kRgb:
      // `data` is a baseline JPEG stream, exactly what the non-PNM branch
      // of LoadImageAsCGImage decodes from a file.
      if (page.data.empty()) return nullptr;
      return CreateCGImageFromEncodedBytes(page.data.data(), page.data.size());

    case brscan::PixelFormat::kGray:
      // `data` is 8-bit grayscale, one byte per pixel, row-major.
      if (page.width <= 0 || page.height <= 0) return nullptr;
      if (page.data.size() < width * height) return nullptr;
      return CreateCGImageFromGray8(page.data.data(), page.width, page.height);

    case brscan::PixelFormat::kBitonal: {
      // `data` is packed 1-bit-per-pixel (MSB-first, 1=black); expand to
      // 8-bit grayscale, the same route LoadPnmAsCGImage takes for a P4.
      if (page.width <= 0 || page.height <= 0) return nullptr;
      const size_t row_bytes = (width + 7) / 8;
      if (page.data.size() < row_bytes * height) return nullptr;
      const std::vector<uint8_t> gray8 =
          ExpandBitonalToGray8(page.data.data(), page.width, page.height);
      return CreateCGImageFromGray8(gray8.data(), page.width, page.height);
    }
  }
  return nullptr;
}

namespace {

// Runs Vision's accurate-level text recognition on `image`. Returns nil
// (not an empty array) if the request itself failed; an empty array
// (not nil) if it succeeded but found no text.
NSArray<VNRecognizedTextObservation*>* RecognizeText(CGImageRef image) {
  VNImageRequestHandler* handler =
      [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
  VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
  request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
  request.usesLanguageCorrection = YES;

  NSError* error = nil;
  const BOOL ok = [handler performRequests:@[ request ] error:&error];
  if (!ok) {
    std::cerr << "[action_ocr] Vision text recognition failed: "
               << (error != nil ? error.localizedDescription.UTF8String
                                 : "unknown error")
               << "\n";
    return nil;
  }
  return request.results;
}

// Draws one page into the already-open PDF context `ctx`: begins a page
// sized to `image`'s pixels, draws `image` as its visible content, then
// (for each of `observations`' recognized strings) draws that string as
// invisible text scaled/positioned over its normalized bounding box, and
// ends the page. Passing an empty or nil `observations` produces a page
// with no text layer (the plain, non-searchable case). This is the shared
// per-page body driven once by OcrImageToSearchablePdf and in a loop by
// WriteSearchablePdf, so the invisible-text logic lives in one place.
void DrawSearchablePdfPage(
    CGContextRef ctx, CGImageRef image,
    NSArray<VNRecognizedTextObservation*>* observations) {
  const size_t width = CGImageGetWidth(image);
  const size_t height = CGImageGetHeight(image);
  CGRect page_rect =
      CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height));

  // A per-page media box, so pages of differing pixel sizes each get their
  // own correctly sized PDF page rather than the context's default box.
  NSData* box_data = [NSData dataWithBytes:&page_rect length:sizeof(page_rect)];
  NSDictionary* page_info =
      @{(__bridge NSString*)kCGPDFContextMediaBox : box_data};
  CGPDFContextBeginPage(ctx, (__bridge CFDictionaryRef)page_info);
  CGContextDrawImage(ctx, page_rect, image);

  // Invisible text layer: same visible page, but every recognized string
  // is drawn again (in Quartz's "invisible" text rendering mode) roughly
  // over the location Vision found it, so the page is selectable and
  // searchable without changing how it looks.
  CGContextSetTextDrawingMode(ctx, kCGTextInvisible);

  for (VNRecognizedTextObservation* obs in observations) {
    VNRecognizedText* candidate = [[obs topCandidates:1] firstObject];
    if (candidate == nil || candidate.string.length == 0) continue;

    // Vision's boundingBox is normalized (0..1) with the origin at the
    // bottom-left of the image -- the same convention CGPDFContext's
    // page space uses, so no coordinate flip is needed: it's a direct
    // scale by the page's pixel dimensions.
    const CGRect bbox = obs.boundingBox;
    const CGRect rect =
        CGRectMake(bbox.origin.x * static_cast<CGFloat>(width),
                   bbox.origin.y * static_cast<CGFloat>(height),
                   bbox.size.width * static_cast<CGFloat>(width),
                   bbox.size.height * static_cast<CGFloat>(height));

    CGFloat font_size = rect.size.height;
    if (font_size < 1) font_size = 1;
    CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), font_size, nullptr);

    NSDictionary* attrs = @{(__bridge NSString*)kCTFontAttributeName :
                                 (__bridge id)font};
    NSAttributedString* attr_str =
        [[NSAttributedString alloc] initWithString:candidate.string
                                         attributes:attrs];
    CTLineRef line =
        CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attr_str);

    // Scale horizontally so the invisible line's width matches the
    // recognized word's bounding box width; the font size above already
    // matches its height. This is a best-effort visual alignment, not
    // load-bearing for searchability -- what matters for
    // OcrImageToSearchablePdf's contract is that the text is present and
    // roughly where it was found.
    const double line_width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    const CGFloat scale_x =
        (line_width > 0 && rect.size.width > 0)
            ? static_cast<CGFloat>(rect.size.width / line_width)
            : 1.0;

    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, rect.origin.x, rect.origin.y);
    CGContextScaleCTM(ctx, scale_x, 1.0);
    CGContextSetTextPosition(ctx, 0, 0);
    CTLineDraw(line, ctx);
    CGContextRestoreGState(ctx);

    CFRelease(line);
    CFRelease(font);
  }

  CGPDFContextEndPage(ctx);
}

}  // namespace

Status WriteSearchablePdf(const std::vector<CGImageRef>& images,
                          bool searchable, const std::string& pdf_path) {
  @autoreleasepool {
    if (images.empty()) return Status::kIoError;

    // The context needs a default media box up front; give it the first
    // page's rect. Every page then overrides it per-page in
    // DrawSearchablePdfPage, so differently sized pages stay correct.
    const CGRect first_rect =
        CGRectMake(0, 0, static_cast<CGFloat>(CGImageGetWidth(images.front())),
                   static_cast<CGFloat>(CGImageGetHeight(images.front())));
    CGRect default_box = first_rect;

    NSString* ns_path = [NSString stringWithUTF8String:pdf_path.c_str()];
    CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:ns_path];
    CGContextRef ctx = CGPDFContextCreateWithURL(url, &default_box, nullptr);
    if (ctx == nullptr) {
      std::cerr << "[action_ocr] could not create PDF: " << pdf_path << "\n";
      return Status::kIoError;
    }

    for (CGImageRef image : images) {
      NSArray<VNRecognizedTextObservation*>* observations = nil;
      if (searchable) {
        observations = RecognizeText(image);
        if (observations == nil) {
          CGPDFContextClose(ctx);
          CGContextRelease(ctx);
          return Status::kProtocolError;
        }
      }
      DrawSearchablePdfPage(ctx, image, observations);
    }

    CGPDFContextClose(ctx);
    CGContextRelease(ctx);
    return Status::kOk;
  }
}

namespace {

// Collects the recognized text of one page into `out_lines` (one entry per
// VNRecognizedTextObservation, in the order Vision returns them -- already
// per-line, top-to-bottom). Returns false if Vision's request itself failed
// on this page (RecognizeText returned nil), true otherwise (including a
// page with no recognized text, which appends nothing).
bool CollectPageLines(CGImageRef image, NSMutableArray<NSString*>* out_lines) {
  NSArray<VNRecognizedTextObservation*>* observations = RecognizeText(image);
  if (observations == nil) return false;
  for (VNRecognizedTextObservation* obs in observations) {
    VNRecognizedText* candidate = [[obs topCandidates:1] firstObject];
    if (candidate == nil || candidate.string.length == 0) continue;
    [out_lines addObject:candidate.string];
  }
  return true;
}

// Serializes `text` to `path` as an HTML or RTF document via
// NSAttributedString's -dataFromRange:documentAttributes:, letting
// Foundation own all escaping/encoding (no hand-rolled entities or control
// words). `doc_type` is NSHTMLTextDocumentType or NSRTFTextDocumentType.
// Returns kIoError if the document can't be serialized or written.
Status WriteAttributedDocument(NSString* text, NSString* doc_type,
                               const std::string& path) {
  NSAttributedString* attr =
      [[NSAttributedString alloc] initWithString:text];
  NSError* error = nil;
  NSData* data =
      [attr dataFromRange:NSMakeRange(0, attr.length)
           documentAttributes:@{NSDocumentTypeDocumentAttribute : doc_type}
                        error:&error];
  if (data == nil) {
    std::cerr << "[action_ocr] could not serialize OCR text document: "
               << (error != nil ? error.localizedDescription.UTF8String
                                 : "unknown error")
               << "\n";
    return Status::kIoError;
  }
  NSString* ns_path = [NSString stringWithUTF8String:path.c_str()];
  if (![data writeToFile:ns_path atomically:YES]) {
    std::cerr << "[action_ocr] could not write OCR text document: " << path
               << "\n";
    return Status::kIoError;
  }
  return Status::kOk;
}

}  // namespace

Status WriteRecognizedText(const std::vector<CGImageRef>& images,
                           OcrTextFormat format, const std::string& path) {
  @autoreleasepool {
    if (images.empty()) return Status::kIoError;

    // Recognize every page first: a page separator (a blank line) goes
    // between pages, so each page's lines are collected as their own group.
    NSMutableArray<NSString*>* pages = [NSMutableArray array];
    for (CGImageRef image : images) {
      NSMutableArray<NSString*>* lines = [NSMutableArray array];
      if (!CollectPageLines(image, lines)) return Status::kProtocolError;
      [pages addObject:[lines componentsJoinedByString:@"\n"]];
    }

    // Join pages with a blank line between them (a page that recognized no
    // text contributes an empty string, so its separator still delimits it).
    NSString* text = [pages componentsJoinedByString:@"\n\n"];

    switch (format) {
      case OcrTextFormat::kPlain: {
        NSData* data = [text dataUsingEncoding:NSUTF8StringEncoding];
        NSString* ns_path = [NSString stringWithUTF8String:path.c_str()];
        if (data == nil || ![data writeToFile:ns_path atomically:YES]) {
          std::cerr << "[action_ocr] could not write OCR text file: " << path
                     << "\n";
          return Status::kIoError;
        }
        return Status::kOk;
      }
      case OcrTextFormat::kHtml:
        return WriteAttributedDocument(text, NSHTMLTextDocumentType, path);
      case OcrTextFormat::kRtf:
        return WriteAttributedDocument(text, NSRTFTextDocumentType, path);
    }
    return Status::kIoError;
  }
}

Status OcrImageToSearchablePdf(const std::string& image_path,
                                const std::string& pdf_path) {
  @autoreleasepool {
    CGImageRef image = LoadImageAsCGImage(image_path);
    if (image == nullptr) {
      std::cerr << "[action_ocr] could not load image: " << image_path << "\n";
      return Status::kIoError;
    }

    const Status status =
        WriteSearchablePdf({image}, /*searchable=*/true, pdf_path);
    CGImageRelease(image);
    return status;
  }
}

}  // namespace brscan
