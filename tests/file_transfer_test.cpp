// Tests for the file-based transfer plan (ica-module/file_transfer.h).
//
// A pure, hermetic unit: no ICADevices, no ImageIO, no device. It maps the
// host's document format/extension/name strings to the {uti, extension, stem}
// plan and per-page file names used by module_main.mm's file-transfer path.

#include "file_transfer.h"

#include <string>

#include <gtest/gtest.h>

namespace brscan::ica {
namespace {

TEST(PlanTransferTest, DefaultsToTiffWhenEverythingEmpty) {
  const TransferPlan p = PlanTransfer("", "", "");
  EXPECT_EQ(p.uti, "public.tiff");
  EXPECT_EQ(p.extension, "tif");
  EXPECT_EQ(p.stem, "Scan");
}

TEST(PlanTransferTest, HonoursHostUtiAndExtension) {
  const TransferPlan p = PlanTransfer("public.jpeg", "jpg", "MyDoc");
  EXPECT_EQ(p.uti, "public.jpeg");
  EXPECT_EQ(p.extension, "jpg");
  EXPECT_EQ(p.stem, "MyDoc");
}

TEST(PlanTransferTest, DerivesUtiFromExtensionWhenFormatMissing) {
  const TransferPlan p = PlanTransfer("", "png", "Shot");
  EXPECT_EQ(p.uti, "public.png");
  EXPECT_EQ(p.extension, "png");
}

TEST(PlanTransferTest, DerivesExtensionFromUtiWhenExtensionMissing) {
  const TransferPlan p = PlanTransfer("public.png", "", "Shot");
  EXPECT_EQ(p.uti, "public.png");
  EXPECT_EQ(p.extension, "png");
}

TEST(PlanTransferTest, TiffUtiDerivesTifExtension) {
  const TransferPlan p = PlanTransfer("public.tiff", "", "");
  EXPECT_EQ(p.extension, "tif");
}

TEST(PlanTransferTest, JpegExtensionAliasDerivesJpegUti) {
  EXPECT_EQ(PlanTransfer("", "jpeg", "").uti, "public.jpeg");
  EXPECT_EQ(PlanTransfer("", "tiff", "").uti, "public.tiff");
}

TEST(PlanTransferTest, UnknownExtensionFallsBackToTiffUti) {
  const TransferPlan p = PlanTransfer("", "xyz", "");
  EXPECT_EQ(p.uti, "public.tiff");
  EXPECT_EQ(p.extension, "xyz");  // The host's own extension is preserved.
}

TEST(PlanTransferTest, UnknownUtiIsPassedThroughWithTifFallbackExtension) {
  const TransferPlan p = PlanTransfer("com.adobe.pdf", "", "");
  EXPECT_EQ(p.uti, "com.adobe.pdf");
  EXPECT_EQ(p.extension, "tif");  // No known mapping; safe fallback.
}

TEST(PlanTransferTest, StripsLeadingDotAndLowercasesExtension) {
  const TransferPlan p = PlanTransfer("", ".TIF", "Doc");
  EXPECT_EQ(p.extension, "tif");
  EXPECT_EQ(p.uti, "public.tiff");
}

TEST(PlanTransferTest, StripsDuplicatedExtensionFromName) {
  const TransferPlan p = PlanTransfer("public.tiff", "tif", "Scan.tif");
  EXPECT_EQ(p.stem, "Scan");
  EXPECT_EQ(TransferFilenameForPage(p, 0), "Scan.tif");
}

TEST(PlanTransferTest, DuplicatedExtensionStripIsCaseInsensitive) {
  const TransferPlan p = PlanTransfer("public.jpeg", "jpg", "Photo.JPG");
  EXPECT_EQ(p.stem, "Photo");
}

TEST(PlanTransferTest, TrimsWhitespaceInInputs) {
  const TransferPlan p = PlanTransfer("  public.png  ", " png ", "  Doc  ");
  EXPECT_EQ(p.uti, "public.png");
  EXPECT_EQ(p.extension, "png");
  EXPECT_EQ(p.stem, "Doc");
}

TEST(TransferFilenameForPageTest, PageZeroHasNoIndex) {
  const TransferPlan p = PlanTransfer("public.tiff", "tif", "Scan");
  EXPECT_EQ(TransferFilenameForPage(p, 0), "Scan.tif");
}

TEST(TransferFilenameForPageTest, LaterPagesGetSpaceIndexSuffix) {
  const TransferPlan p = PlanTransfer("public.tiff", "tif", "Scan");
  EXPECT_EQ(TransferFilenameForPage(p, 1), "Scan 1.tif");
  EXPECT_EQ(TransferFilenameForPage(p, 2), "Scan 2.tif");
}

TEST(TransferFilenameForPageTest, NegativeIndexTreatedAsZero) {
  const TransferPlan p = PlanTransfer("public.tiff", "tif", "Scan");
  EXPECT_EQ(TransferFilenameForPage(p, -5), "Scan.tif");
}

}  // namespace
}  // namespace brscan::ica
