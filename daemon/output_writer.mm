// Objective-C++ implementation of the library-level output writer (see
// output_writer.h). ARC is enabled for this translation unit only (see
// CMakeLists.txt's COMPILE_OPTIONS on this source file) to manage the
// Objective-C objects below (NSData, NS*); Core Foundation and
// CoreGraphics/ImageIO opaque types (CGImageRef, CFDataRef,
// CGImageDestinationRef, CGColorSpaceRef, CGDataProviderRef, ...) are never
// ARC-managed regardless, and are retained/released explicitly throughout.

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "action_ocr.h"
#include "output_writer.h"
#include "scan_output.h"

namespace brscan::scand {

namespace {

// NSTIFFCompression codec numbers, as written into the TIFF Compression
// tag (see output_writer.h): LZW = 5, CCITT Group 3 = 3, Group 4 = 4.
constexpr int kTiffCompressionLzw = 5;
constexpr int kTiffCompressionG3 = 3;
constexpr int kTiffCompressionG4 = 4;

// Splits `path` into its stem (directory + basename without extension) and
// its extension (including the leading dot, or empty). A dot only counts
// as the extension separator if it falls after the last path separator, so
// a dot inside a directory name isn't mistaken for an extension.
void SplitExtension(const std::string& path, std::string* stem,
                    std::string* ext) {
  const size_t slash = path.find_last_of('/');
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    *stem = path.substr(0, dot);
    *ext = path.substr(dot);
  } else {
    *stem = path;
    *ext = "";
  }
}

// Returns `base` with its extension replaced by `new_ext` (which includes
// the leading dot), keeping its directory and stem.
std::string ReplaceExtension(const std::string& base,
                             const std::string& new_ext) {
  std::string stem, ext;
  SplitExtension(base, &stem, &ext);
  return stem + new_ext;
}

// The path for document `doc_index_1based` of `num_docs` split containers:
// `base` unchanged for a single document, otherwise `base` with a
// `-docNNN` suffix (1-based, zero-padded to 3 digits) inserted before its
// extension -- e.g. `scan.pdf` -> `scan-doc001.pdf`, `scan-doc002.pdf`.
std::string DocPath(const std::string& base, int doc_index_1based,
                    int num_docs) {
  if (num_docs <= 1) return base;
  std::string stem, ext;
  SplitExtension(base, &stem, &ext);
  std::ostringstream s;
  s << stem << "-doc" << std::setfill('0') << std::setw(3) << doc_index_1based
    << ext;
  return s.str();
}

// Wraps a kBitonal page's packed 1-bit-per-pixel raster (MSB-first,
// 1=black per PixelFormat::kBitonal) in a genuine 1-bpc bilevel CGImageRef
// -- the form the CCITT Group 3/4 TIFF codecs require. The `decode` array
// {1,0} maps sample 1 to black so the packed 1=black convention is honored
// without copying/inverting the data. Returns nullptr if `page` is not a
// bitonal page or its data is too short. The caller owns the image.
CGImageRef CreateBilevelCGImageFromBitonal(const brscan::ScanResult& page) {
  if (page.format != brscan::PixelFormat::kBitonal) return nullptr;
  if (page.width <= 0 || page.height <= 0) return nullptr;
  const size_t row_bytes = (static_cast<size_t>(page.width) + 7) / 8;
  const size_t needed = row_bytes * static_cast<size_t>(page.height);
  if (page.data.size() < needed) return nullptr;

  CGColorSpaceRef colorspace = CGColorSpaceCreateDeviceGray();
  CFDataRef cfdata = CFDataCreate(kCFAllocatorDefault, page.data.data(),
                                  static_cast<CFIndex>(needed));
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(cfdata);
  CFRelease(cfdata);  // provider retains its own reference.

  const CGFloat decode[2] = {1.0, 0.0};
  CGImageRef image = CGImageCreate(
      static_cast<size_t>(page.width), static_cast<size_t>(page.height),
      /*bitsPerComponent=*/1, /*bitsPerPixel=*/1,
      /*bytesPerRow=*/row_bytes, colorspace,
      kCGBitmapByteOrderDefault | kCGImageAlphaNone, provider, decode,
      /*shouldInterpolate=*/false, kCGRenderingIntentDefault);

  CGDataProviderRelease(provider);
  CGColorSpaceRelease(colorspace);
  return image;
}

// The TIFF Compression code to actually write for `compression` given
// `page`: G3/G4 for a bilevel (kBitonal) page, but LZW for a kGray/kRgb
// page (which isn't bilevel and so can't use a fax codec -- falling back
// to LZW keeps it lossless rather than thresholding). LZW is always LZW.
int EffectiveTiffCompression(TiffCompression compression,
                             const brscan::ScanResult& page) {
  const bool bilevel = page.format == brscan::PixelFormat::kBitonal;
  switch (compression) {
    case TiffCompression::kG3:
      return bilevel ? kTiffCompressionG3 : kTiffCompressionLzw;
    case TiffCompression::kG4:
      return bilevel ? kTiffCompressionG4 : kTiffCompressionLzw;
    case TiffCompression::kLzw:
      break;
  }
  return kTiffCompressionLzw;
}

// Builds the CGImageRef used to encode `page` into a TIFF at the given
// `compression_code`: a 1-bpc bilevel image for a fax-coded (G3/G4)
// bitonal page, otherwise the ordinary 8-bit/decoded image. Returns
// nullptr on decode failure. The caller owns the image.
CGImageRef MakeTiffPageImage(const brscan::ScanResult& page,
                             int compression_code) {
  if (compression_code == kTiffCompressionG3 ||
      compression_code == kTiffCompressionG4) {
    return CreateBilevelCGImageFromBitonal(page);
  }
  return CreateCGImageFromScanResult(page);
}

NSURL* FileUrl(const std::string& path) {
  return [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
}

// Writes `pages` as one multi-page TIFF at `path`, each page carrying its
// own Compression tag per EffectiveTiffCompression. Returns kIoError if the
// destination can't be created, a page can't be decoded, or finalizing
// fails.
brscan::Status WriteMultipageTiff(
    const std::vector<const brscan::ScanResult*>& pages,
    TiffCompression compression, const std::string& path) {
  @autoreleasepool {
    if (pages.empty()) return brscan::Status::kIoError;

    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)FileUrl(path), CFSTR("public.tiff"),
        pages.size(), nullptr);
    if (dest == nullptr) return brscan::Status::kIoError;

    bool ok = true;
    for (const brscan::ScanResult* page : pages) {
      const int compression_code = EffectiveTiffCompression(compression, *page);
      CGImageRef image = MakeTiffPageImage(*page, compression_code);
      if (image == nullptr) {
        ok = false;
        break;
      }
      NSDictionary* props = @{
        (__bridge NSString*)kCGImagePropertyTIFFDictionary : @{
          (__bridge NSString*)
          kCGImagePropertyTIFFCompression : @(compression_code)
        }
      };
      CGImageDestinationAddImage(dest, image, (__bridge CFDictionaryRef)props);
      CGImageRelease(image);
    }

    if (!ok) {
      CFRelease(dest);
      return brscan::Status::kIoError;
    }
    const bool finalized = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    return finalized ? brscan::Status::kOk : brscan::Status::kIoError;
  }
}

// Writes `page` as a single-image file of type `uti` (e.g. "public.jpeg",
// "public.png") at `path`. Returns kIoError on decode/encode failure.
brscan::Status WriteSingleImageFile(const brscan::ScanResult& page,
                                    CFStringRef uti, const std::string& path) {
  @autoreleasepool {
    CGImageRef image = CreateCGImageFromScanResult(page);
    if (image == nullptr) return brscan::Status::kIoError;

    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)FileUrl(path), uti, 1, nullptr);
    if (dest == nullptr) {
      CGImageRelease(image);
      return brscan::Status::kIoError;
    }
    CGImageDestinationAddImage(dest, image, nullptr);
    const bool finalized = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(image);
    return finalized ? brscan::Status::kOk : brscan::Status::kIoError;
  }
}

// The number of pages each split container holds: `separate_n` (>= 1) when
// separating every N ScanResults (by image count or by page count -- they
// split identically for now, see OutputSeparation's doc comment),
// otherwise all pages in one container.
int PagesPerDocument(const OutputSettings& settings, int total) {
  const bool separating = settings.separation == OutputSeparation::kEveryImage ||
                          settings.separation == OutputSeparation::kEveryPage;
  if (separating && settings.separate_n >= 1) {
    return settings.separate_n;
  }
  return total > 0 ? total : 1;
}

// Writes a PDF or TIFF container per format, honoring document separation:
// ceil(total / per_doc) containers, each holding up to `per_doc`
// consecutive pages, named via DocPath. `container_base` already carries
// the target extension.
brscan::Status WriteContainers(const std::vector<brscan::ScanResult>& pages,
                               const OutputSettings& settings,
                               const std::string& container_base,
                               std::vector<std::string>* written) {
  const int total = static_cast<int>(pages.size());
  const int per_doc = PagesPerDocument(settings, total);
  const int num_docs = (total + per_doc - 1) / per_doc;

  for (int doc = 0; doc < num_docs; ++doc) {
    const int first = doc * per_doc;
    const int last = std::min(total, first + per_doc);

    const std::string path = DocPath(container_base, doc + 1, num_docs);

    brscan::Status status = brscan::Status::kOk;
    if (settings.format == OutputFormat::kPdf) {
      // Decode this document's pages to images, hand them to the shared
      // multi-page PDF writer (which also owns the searchable text layer),
      // then release them.
      std::vector<CGImageRef> images;
      images.reserve(static_cast<size_t>(last - first));
      bool decoded = true;
      for (int i = first; i < last; ++i) {
        CGImageRef image =
            CreateCGImageFromScanResult(pages[static_cast<size_t>(i)]);
        if (image == nullptr) {
          decoded = false;
          break;
        }
        images.push_back(image);
      }
      if (!decoded) {
        for (CGImageRef image : images) CGImageRelease(image);
        return brscan::Status::kIoError;
      }
      status = brscan::WriteSearchablePdf(images, settings.searchable, path);
      for (CGImageRef image : images) CGImageRelease(image);
    } else {  // kTiff.
      std::vector<const brscan::ScanResult*> slice;
      slice.reserve(static_cast<size_t>(last - first));
      for (int i = first; i < last; ++i) {
        slice.push_back(&pages[static_cast<size_t>(i)]);
      }
      status = WriteMultipageTiff(slice, settings.tiff_compression, path);
    }

    if (status != brscan::Status::kOk) return status;
    written->push_back(path);
  }
  return brscan::Status::kOk;
}

// Writes one JPEG or PNG file per page, numbered `-NNN` when there is more
// than one page (via brscan::cli::PagePath). Document separation does not
// apply -- the per-file numbering already keeps the pages distinct.
brscan::Status WritePerPageImages(const std::vector<brscan::ScanResult>& pages,
                                  CFStringRef uti,
                                  const std::string& image_base,
                                  std::vector<std::string>* written) {
  const int total = static_cast<int>(pages.size());
  for (int i = 0; i < total; ++i) {
    const std::string path = brscan::cli::PagePath(image_base, i + 1, total);
    const brscan::Status status =
        WriteSingleImageFile(pages[static_cast<size_t>(i)], uti, path);
    if (status != brscan::Status::kOk) return status;
    written->push_back(path);
  }
  return brscan::Status::kOk;
}

}  // namespace

brscan::Status WriteConfiguredOutput(
    const std::vector<brscan::ScanResult>& pages, const OutputSettings& settings,
    const std::string& base_path, std::vector<std::string>* written) {
  written->clear();
  if (pages.empty()) return brscan::Status::kIoError;

  switch (settings.format) {
    case OutputFormat::kNative:
      // Native per-PixelFormat files, numbered by WritePages exactly as the
      // CLI does. Document separation does not apply to native output.
      if (!brscan::cli::WritePages(pages, base_path)) {
        return brscan::Status::kIoError;
      }
      for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
        written->push_back(brscan::cli::PagePath(
            base_path, i + 1, static_cast<int>(pages.size())));
      }
      return brscan::Status::kOk;

    case OutputFormat::kPdf:
      return WriteContainers(pages, settings, ReplaceExtension(base_path, ".pdf"),
                             written);

    case OutputFormat::kTiff:
      return WriteContainers(pages, settings, ReplaceExtension(base_path, ".tif"),
                             written);

    case OutputFormat::kJpeg:
      return WritePerPageImages(pages, CFSTR("public.jpeg"),
                                ReplaceExtension(base_path, ".jpg"), written);

    case OutputFormat::kPng:
      return WritePerPageImages(pages, CFSTR("public.png"),
                                ReplaceExtension(base_path, ".png"), written);
  }
  return brscan::Status::kIoError;
}

}  // namespace brscan::scand
