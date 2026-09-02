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
  tjDestroy(handle);
  if (rc != 0) return Status::kProtocolError;

  out->width = width;
  out->height = height;
  out->format = PixelFormat::kRgb;
  out->pixels = std::move(pixels);
  return Status::kOk;
}

}  // namespace brscan
