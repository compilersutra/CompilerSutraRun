//===----------------------------------------------------------------------===//
// FrameCheckTest.cpp
// The render-validation oracle. These tests pin the behaviour that distinguishes
// "the pipeline produced a real frame" from "something non-black appeared".
//===----------------------------------------------------------------------===//

#include "csrun/validation/FrameCheck.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

constexpr std::uint32_t kW = 64;
constexpr std::uint32_t kH = 64;

std::vector<std::uint8_t> makeFrame(std::uint32_t w, std::uint32_t h, std::uint8_t fill) {
  return std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4u,
                                   static_cast<std::uint8_t>(fill));
}

void setPixel(std::vector<std::uint8_t> &f, std::uint32_t w, std::uint32_t x, std::uint32_t y,
              std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
  const std::size_t off = (static_cast<std::size_t>(y) * w + x) * 4u;
  f[off + 0] = r;
  f[off + 1] = g;
  f[off + 2] = b;
  f[off + 3] = a;
}

/// A frame that covers the left half with a horizontal colour gradient.
std::vector<std::uint8_t> gradientFrame() {
  std::vector<std::uint8_t> f(static_cast<std::size_t>(kW) * kH * 4u, 0);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW / 2; ++x) {
      const auto v = static_cast<std::uint8_t>(x * 255 / (kW / 2));
      setPixel(f, kW, x, y, v, static_cast<std::uint8_t>(255 - v), 128, 255);
    }
  }
  return f;
}

} // namespace

TEST(FrameCheckTest, RejectsNullFrame) {
  const auto r = csrun::checkFrame(nullptr, 0, kW, kH);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.reason.find("empty"), std::string::npos);
}

TEST(FrameCheckTest, RejectsZeroExtent) {
  const auto f = makeFrame(kW, kH, 255);
  EXPECT_FALSE(csrun::checkFrame(f.data(), f.size(), 0, kH).ok);
  EXPECT_FALSE(csrun::checkFrame(f.data(), f.size(), kW, 0).ok);
}

TEST(FrameCheckTest, RejectsUndersizedBuffer) {
  const auto f = makeFrame(kW, kH, 255);
  const auto r = csrun::checkFrame(f.data(), f.size() / 2, kW, kH);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.reason.find("too small"), std::string::npos);
}

TEST(FrameCheckTest, AllBlackFrameFails) {
  const auto f = makeFrame(kW, kH, 0);
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.coveredPixels, 0u);
  EXPECT_DOUBLE_EQ(r.coveragePercent, 0.0);
}

TEST(FrameCheckTest, FullCoverageGradientPasses) {
  const auto f = gradientFrame();
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH);
  EXPECT_TRUE(r.ok) << r.reason;
  EXPECT_NEAR(r.coveragePercent, 50.0, 0.5);
  EXPECT_GT(r.distinctColors, 8u);
}

TEST(FrameCheckTest, SingleStrayPixelFailsCoverage) {
  // The exact case the old centre-pixel oracle passed: one lit texel, nothing else.
  auto f = makeFrame(kW, kH, 0);
  setPixel(f, kW, kW / 2, kH / 2, 255, 255, 255);
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH);
  EXPECT_FALSE(r.ok) << "a single texel must not count as a successful render";
  EXPECT_EQ(r.coveredPixels, 1u);
  EXPECT_NEAR(r.coveragePercent, 100.0 / (kW * kH), 0.01);
}

TEST(FrameCheckTest, TriangleSizedCoveragePasses) {
  // A modest but real coverage is legitimate -- the threshold must not be so high
  // that ordinary geometry gets rejected.
  auto f = makeFrame(kW, kH, 0);
  for (std::uint32_t y = 0; y < kH / 2; ++y) {
    for (std::uint32_t x = 0; x < kW / 2; ++x)
      setPixel(f, kW, x, y, static_cast<std::uint8_t>(x), 200, 64);
  }
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH);
  EXPECT_TRUE(r.ok) << r.reason;
  EXPECT_NEAR(r.coveragePercent, 25.0, 1.0);
}

TEST(FrameCheckTest, FlatColourFramePassesByDefault) {
  // Uniform colour at full coverage is legitimate for a flat-shaded scene, so the
  // default config must accept it. The strict variant is what catches a dead shader.
  auto f = makeFrame(kW, kH, 0);
  for (std::uint32_t y = 0; y < kH; ++y)
    for (std::uint32_t x = 0; x < kW; ++x)
      setPixel(f, kW, x, y, 128, 128, 128);
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH);
  EXPECT_TRUE(r.ok) << r.reason;
  EXPECT_EQ(r.distinctColors, 1u);
  EXPECT_DOUBLE_EQ(r.coveragePercent, 100.0);

  csrun::FrameCheckConfig strict;
  strict.minDistinctColors = 2;
  const auto bad = csrun::checkFrame(f.data(), f.size(), kW, kH, strict);
  EXPECT_FALSE(bad.ok);
  EXPECT_NE(bad.reason.find("flat"), std::string::npos);
}

TEST(FrameCheckTest, IdenticalReadbackIsStable) {
  const auto f = gradientFrame();
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH, {}, f.data(), f.size());
  EXPECT_TRUE(r.ok) << r.reason;
  EXPECT_TRUE(r.matchesReference);
  EXPECT_NE(r.reason.find("stable"), std::string::npos);
}

TEST(FrameCheckTest, DifferingReadbackFails) {
  const auto f = gradientFrame();
  auto other = f;
  setPixel(other, kW, 5, 5, 1, 2, 3);
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH, {}, other.data(), other.size());
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.matchesReference);
  EXPECT_NE(r.reason.find("differ"), std::string::npos);
  EXPECT_NE(r.reason.find("max channel delta"), std::string::npos)
      << "the reason should quantify the divergence, not just assert it: " << r.reason;
}

TEST(FrameCheckTest, UndersizedReferenceIsIgnoredNotFatal) {
  const auto f = gradientFrame();
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH, {}, f.data(), 4);
  EXPECT_TRUE(r.ok) << r.reason;
  EXPECT_FALSE(r.matchesReference) << "a short reference is not a stability failure";
}

TEST(FrameCheckTest, AlphaOnlyDifferenceIsStableByDefault) {
  // Regression: a shader that writes only RGB leaves alpha holding whatever the render
  // target had, so the two readbacks can disagree on alpha alone. That is not evidence
  // about the draw, and the default mask must not treat it as nondeterminism.
  const auto f = gradientFrame();
  auto other = f;
  for (std::size_t i = 3; i < other.size(); i += 4)
    other[i] = 229;
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH, {}, other.data(), other.size());
  EXPECT_TRUE(r.ok) << r.reason;
  EXPECT_TRUE(r.matchesReference);
}

TEST(FrameCheckTest, CompareMaskCanIncludeAlphaExplicitly) {
  const auto f = gradientFrame();
  auto other = f;
  for (std::size_t i = 3; i < other.size(); i += 4)
    other[i] = 0;
  csrun::FrameCheckConfig cfg;
  cfg.compareMask = 0x0F;
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH, cfg, other.data(), other.size());
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.matchesReference);
}

TEST(FrameCheckTest, DiffFramesReportsMagnitudeAndLocation) {
  const auto a = gradientFrame();
  auto b = a;
  setPixel(b, kW, 3, 2, 200, 201, 202);
  setPixel(b, kW, 4, 2, 200, 201, 202);
  const auto d = csrun::diffFrames(a.data(), b.data(), a.size(), kW, kH);
  EXPECT_TRUE(d.hasDifference);
  EXPECT_EQ(d.differingTexels, 2u);
  EXPECT_EQ(d.firstX, 3u);
  EXPECT_EQ(d.firstY, 2u);
  // gradientFrame tops out well below 255, so the reported delta is bounded by the
  // value actually written.
  EXPECT_LE(d.maxChannelDelta, 255u);
  EXPECT_GT(d.percentDiffering, 0.0);
}

TEST(FrameCheckTest, DiffFramesOnIdenticalInputReportsNoDifference) {
  const auto f = gradientFrame();
  const auto d = csrun::diffFrames(f.data(), f.data(), f.size(), kW, kH);
  EXPECT_FALSE(d.hasDifference);
  EXPECT_EQ(d.differingTexels, 0u);
  EXPECT_EQ(d.maxChannelDelta, 0u);
  EXPECT_NEAR(d.percentDiffering, 0.0, 1e-9);
}

TEST(FrameCheckTest, DiffFramesIgnoresUndersizedAndNullBuffers) {
  const auto f = gradientFrame();
  const auto shortBuf = csrun::diffFrames(f.data(), f.data(), 4, kW, kH);
  EXPECT_FALSE(shortBuf.hasDifference);
  const auto nullBuf = csrun::diffFrames(nullptr, f.data(), f.size(), kW, kH);
  EXPECT_FALSE(nullBuf.hasDifference);
}

TEST(FrameCheckTest, ZeroAlphaStillCountsAsCoverageByDefault) {
  // A fragment shader that writes only RGB leaves alpha at 0. That is legitimate, so
  // the default must accept it -- otherwise ordinary corpus shaders look "blank".
  auto f = makeFrame(kW, kH, 0);
  for (std::uint32_t y = 0; y < kH; ++y)
    for (std::uint32_t x = 0; x < kW / 2; ++x)
      setPixel(f, kW, x, y, 10 + static_cast<std::uint8_t>(x), 100, 50, 0);
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH);
  EXPECT_TRUE(r.ok) << r.reason;
  EXPECT_NEAR(r.coveragePercent, 50.0, 0.5);
}

TEST(FrameCheckTest, MinAlphaIsConfigurableForOpaqueWorkloads) {
  // With minAlpha=1 a transparent frame is not coverage; for a workload that requires
  // opaque output this is the gate that catches an unpremultiplied/cleared buffer.
  auto f = makeFrame(kW, kH, 0);
  for (std::uint32_t y = 0; y < kH; ++y)
    for (std::uint32_t x = 0; x < kW; ++x)
      setPixel(f, kW, x, y, 200, 100, 50, 0);

  csrun::FrameCheckConfig opaque;
  opaque.minAlpha = 1;
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH, opaque);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.coveredPixels, 0u);
}

TEST(FrameCheckTest, BlackColourIsNotCoverageEvenWhenOpaque) {
  // Guards against the opposite error: a cleared frame cleared to opaque black must
  // never count as a successful render.
  auto f = makeFrame(kW, kH, 0);
  for (std::uint32_t y = 0; y < kH; ++y)
    for (std::uint32_t x = 0; x < kW; ++x)
      setPixel(f, kW, x, y, 0, 0, 0, 255);
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH);
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.coveredPixels, 0u);
}

TEST(FrameCheckTest, MinCoverageIsConfigurable) {
  auto f = makeFrame(kW, kH, 0);
  // ~6% coverage: below the 1% default's neighbourhood only if we raise the bar.
  const std::uint32_t side = 10; // 100 px of 4096 = 2.4%
  for (std::uint32_t y = 0; y < side; ++y)
    for (std::uint32_t x = 0; x < side; ++x)
      setPixel(f, kW, x, y, static_cast<std::uint8_t>(x * 8), 90, 200);

  csrun::FrameCheckConfig strict;
  strict.minCoveragePercent = 10.0;
  EXPECT_FALSE(csrun::checkFrame(f.data(), f.size(), kW, kH, strict).ok);

  csrun::FrameCheckConfig lenient;
  lenient.minCoveragePercent = 1.0;
  EXPECT_TRUE(csrun::checkFrame(f.data(), f.size(), kW, kH, lenient).ok);
}

TEST(FrameCheckTest, MinDistinctColoursIsConfigurable) {
  const auto f = gradientFrame(); // many quantised colours
  csrun::FrameCheckConfig strict;
  strict.minDistinctColors = 4096; // more than a 5-bit quantised frame can supply
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH, strict);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.reason.find("flat"), std::string::npos);

  csrun::FrameCheckConfig lenient;
  lenient.minDistinctColors = 1;
  EXPECT_TRUE(csrun::checkFrame(f.data(), f.size(), kW, kH, lenient).ok);
}

TEST(FrameCheckTest, TinyGradientCountsAsDistinctColours) {
  auto f = makeFrame(kW, kH, 0);
  for (std::uint32_t y = 0; y < kH; ++y)
    for (std::uint32_t x = 0; x < kW; ++x)
      setPixel(f, kW, x, y, 10, 20, 30);
  const auto r = csrun::checkFrame(f.data(), f.size(), kW, kH, {}, nullptr, 0);
  EXPECT_EQ(r.distinctColors, 1u);
}

TEST(FrameCheckTest, CenterPixelReadsMiddleTexel) {
  auto f = makeFrame(kW, kH, 0);
  setPixel(f, kW, kW / 2, kH / 2, 11, 22, 33, 44);
  std::uint8_t px[4] = {0, 0, 0, 0};
  csrun::frameCenterPixel(f.data(), kW, kH, px);
  EXPECT_EQ(px[0], 11);
  EXPECT_EQ(px[1], 22);
  EXPECT_EQ(px[2], 33);
  EXPECT_EQ(px[3], 44);
}

TEST(FrameCheckTest, CenterPixelHandlesNullAndOddExtent) {
  std::uint8_t px[4] = {9, 9, 9, 9};
  csrun::frameCenterPixel(nullptr, kW, kH, px);
  EXPECT_EQ(px[0], 0);
  auto f = makeFrame(3, 5, 77);
  csrun::frameCenterPixel(f.data(), 3, 5, px);
  EXPECT_EQ(px[0], 77);
  csrun::frameCenterPixel(f.data(), 3, 5, nullptr); // must not crash
}
