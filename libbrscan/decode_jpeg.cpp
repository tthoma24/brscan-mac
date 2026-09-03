#include "decode_jpeg.h"

#include <turbojpeg.h>

namespace brscan {

Status DecodeJpeg(const uint8_t* jpeg, size_t len, Image* out) {
  if (jpeg == nullptr || len == 0 || out == nullptr) {
    return Status::kProtocolError;
  }

  tjhandle handle = tjInitDecompress();
  if (handle == nullptr) return Status::kProtocolError;

  int width = 0;
  int height = 0;
  int subsamp = 0;
  int colorspace = 0;
  if (tjDecompressHeader3(handle, jpeg, static_cast<unsigned long>(len),
                           &width, &height, &subsamp, &colorspace) != 0 ||
      width <= 0 || height <= 0) {
    tjDestroy(handle);
    return Status::kProtocolError;
  }

  std::vector<uint8_t> pixels(static_cast<size_t>(width) *
                               static_cast<size_t>(height) * 3);
  const int rc = tjDecompress2(handle, jpeg, static_cast<unsigned long>(len),
                                pixels.data(), width, /*pitch=*/0, height,
                                TJPF_RGB, TJFLAG_ACCURATEDCT);
  // libjpeg-turbo signals two kinds of trouble through the same rc != 0:
  // TJERR_FATAL, where the output is unusable, and TJERR_WARNING, where it
  // recovered and the pixel buffer is still fully populated. Real
  // MFC-J6920DW ADF pages hit the latter: the device streams a baseline
  // JPEG whose final entropy segment is a few MCUs short of the height it
  // declares in its SOF (it omits trailing all-white scan lines of blank
  // paper), so tjDecompress2 returns -1 with "premature end of data
  // segment" after decoding every row -- macOS ImageIO (`sips`) decodes
  // the same bytes without complaint. Rejecting that warning drops an
  // otherwise-perfect page (observed on ADF simplex page 1), so only a
  // fatal error is treated as a decode failure here. tjGetErrorCode must
  // be read before tjDestroy frees the handle.
  //
  // This widens acceptance to ALL libjpeg-turbo recoverable warnings, not
  // only "premature end" -- any warning still yields a fully-written pixel
  // buffer. The caller's structural checks are the backstop: ReadChunkedJpeg
  // only hands over a payload it has confirmed ends at the JPEG EOI marker
  // (ff d9), so a truncated or garbled stream is caught before it reaches
  // here. Requires libjpeg-turbo >= 2.0 for tjGetErrorCode / TJERR_WARNING.
  const bool fatal = rc != 0 && tjGetErrorCode(handle) != TJERR_WARNING;
  tjDestroy(handle);
  if (fatal) return Status::kProtocolError;

  out->width = width;
  out->height = height;
  out->format = PixelFormat::kRgb;
  out->pixels = std::move(pixels);
  return Status::kOk;
}

}  // namespace brscan
