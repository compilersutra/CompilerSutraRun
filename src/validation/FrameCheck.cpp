//===----------------------------------------------------------------------===//
// FrameCheck.cpp
//===----------------------------------------------------------------------===//

#include "csrun/validation/FrameCheck.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace csrun {
namespace {

/// Quantise to 5 bits per channel so that imperceptible dithering does not inflate
/// the distinct-colour count, while genuine shading gradients still register.
constexpr int kQuantShift = 3;
constexpr int kQuantLevels = 1 << (8 - kQuantShift);

std::uint32_t quantizeColor(const std::uint8_t *px) {
  return (static_cast<std::uint32_t>(px[0] >> kQuantShift) << 10) |
         (static_cast<std::uint32_t>(px[1] >> kQuantShift) << 5) |
         static_cast<std::uint32_t>(px[2] >> kQuantShift);
}

std::string format(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buffer[256];
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  return std::string(buffer);
}

} // namespace

FrameCheckResult checkFrame(const std::uint8_t *rgba, std::size_t bytes, std::uint32_t width,
                            std::uint32_t height, const FrameCheckConfig &config,
                            const std::uint8_t *reference, std::size_t referenceBytes) {
  FrameCheckResult result;
  if (rgba == nullptr || width == 0 || height == 0) {
    result.reason = "empty framebuffer -- nothing to validate";
    return result;
  }
  const std::size_t needed = static_cast<std::size_t>(width) * height * 4u;
  if (bytes < needed) {
    result.reason = format("framebuffer too small: have %zu bytes, need %zu", bytes, needed);
    return result;
  }

  result.totalPixels = static_cast<std::uint64_t>(width) * height;
  std::vector<bool> seen(static_cast<std::size_t>(kQuantLevels) * kQuantLevels * kQuantLevels,
                         false);
  for (std::size_t i = 0; i + 3 < needed; i += 4) {
    const bool covered = rgba[i + 3] >= config.minAlpha && (rgba[i] | rgba[i + 1] | rgba[i + 2]) != 0;
    if (covered) {
      ++result.coveredPixels;
      const std::uint32_t q = quantizeColor(rgba + i);
      seen[q] = true;
    }
  }
  for (const bool s : seen) {
    if (s)
      ++result.distinctColors;
  }
  result.coveragePercent =
      result.totalPixels == 0
          ? 0.0
          : 100.0 * static_cast<double>(result.coveredPixels) / static_cast<double>(result.totalPixels);

  if (reference != nullptr && referenceBytes >= needed)
    result.matchesReference = !diffFrames(rgba, reference, needed, width, height, config).hasDifference;

  // Coverage first: a frame with effectively no visible output is the failure that
  // matters most. This measures visible output, not rasterisation -- the caller can
  // cross-reference an occlusion query to tell the two apart.
  if (result.coveragePercent < config.minCoveragePercent) {
    result.ok = false;
    result.reason = format("visible coverage %.3f%% is below the %.3f%% floor -- frame is blank",
                           result.coveragePercent, config.minCoveragePercent);
    return result;
  }
  if (result.distinctColors < config.minDistinctColors) {
    result.ok = false;
    result.reason = format("only %u distinct colour(s) -- frame is flat, shader likely did not run",
                           result.distinctColors);
    return result;
  }
  if (reference != nullptr && referenceBytes >= needed && !result.matchesReference) {
    const FrameDiff diff = diffFrames(rgba, reference, needed, width, height, config);
    result.ok = false;
    result.reason = format(
        "two readbacks of the same draw differ -- %llu texel(s) (%.3f%%) changed, max channel "
        "delta %u, first at (%u,%u)",
        static_cast<unsigned long long>(diff.differingTexels), diff.percentDiffering,
        diff.maxChannelDelta, diff.firstX, diff.firstY);
    return result;
  }

  result.ok = true;
  result.reason = format("coverage %.3f%%, %u distinct colours%s", result.coveragePercent,
                         result.distinctColors,
                         (reference != nullptr && referenceBytes >= needed) ? ", stable" : "");
  return result;
}

FrameDiff diffFrames(const std::uint8_t *a, const std::uint8_t *b, std::size_t bytes,
                     std::uint32_t width, std::uint32_t height, const FrameCheckConfig &config) {
  FrameDiff diff;
  if (a == nullptr || b == nullptr || width == 0 || height == 0)
    return diff;
  const std::size_t needed = static_cast<std::size_t>(width) * height * 4u;
  if (bytes < needed)
    return diff;
  const std::uint32_t total = width * height;
  for (std::size_t i = 0; i + 3 < needed; i += 4) {
    std::uint32_t texelDelta = 0;
    for (std::uint32_t c = 0; c < 4; ++c) {
      if ((config.compareMask & (1u << c)) == 0)
        continue;
      texelDelta = std::max(texelDelta, static_cast<std::uint32_t>(
                                             std::abs(static_cast<int>(a[i + c]) - b[i + c])));
    }
    if (texelDelta == 0)
      continue;
    if (!diff.hasDifference) {
      diff.hasDifference = true;
      diff.firstX = static_cast<std::uint32_t>((i / 4) % width);
      diff.firstY = static_cast<std::uint32_t>((i / 4) / width);
    }
    ++diff.differingTexels;
    diff.maxChannelDelta = std::max(diff.maxChannelDelta, texelDelta);
  }
  if (total != 0)
    diff.percentDiffering = 100.0 * static_cast<double>(diff.differingTexels) / total;
  return diff;
}

void frameCenterPixel(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,                      std::uint8_t *outRgba) {
  if (rgba == nullptr || outRgba == nullptr || width == 0 || height == 0) {
    if (outRgba != nullptr) {
      outRgba[0] = outRgba[1] = outRgba[2] = outRgba[3] = 0;
    }
    return;
  }
  const std::size_t off = (static_cast<std::size_t>(height / 2) * width + width / 2) * 4u;
  std::memcpy(outRgba, rgba + off, 4);
}

} // namespace csrun
