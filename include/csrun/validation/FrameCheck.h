//===----------------------------------------------------------------------===//
// FrameCheck.h — evidence-based validation of a rendered frame.
//
// The previous oracle asked one question: "is the centre pixel non-black?". That
// accepts a single stray texel from a draw that otherwise rasterised nothing, and
// it cannot tell a finished render from uninitialised memory. This check reports
// what the frame actually contains, so a PASS means the pipeline produced a
// plausible, stable, non-degenerate image rather than merely "not black".
//===----------------------------------------------------------------------===//

#ifndef CSRUN_VALIDATION_FRAMECHECK_H
#define CSRUN_VALIDATION_FRAMECHECK_H

#include <cstdint>
#include <string>

namespace csrun {

struct FrameCheckConfig {
  /// Minimum share of the frame that must be covered by non-black texels (0-100).
  /// A single stray texel is far below this; a real triangle or fullscreen pass is above.
  double minCoveragePercent = 1.0;
  /// A flat-shaded scene legitimately renders as a single colour, so this defaults to 1
  /// (no requirement). Raise it for workloads known to produce shading gradients; it then
  /// catches a readback that never varied, i.e. the shader probably did not run.
  std::uint32_t minDistinctColors = 1;
  /// Alpha values below this are excluded from coverage. Defaults to 0 (ignore alpha):
  /// a fragment shader that writes only RGB is legitimate, and alpha is not meaningful
  /// unless the workload blends. Raise to 1 to require opaque output.
  std::uint8_t minAlpha = 0;
  /// Compare only these channels when checking two readbacks of the same draw.
  /// Defaults to RGB, deliberately excluding alpha: a fragment shader that assigns
  /// only `.rgb` (see `pushconstants.frag`) leaves `.a` unwritten, and an unwritten
  /// SPIR-V output component is undefined -- drivers emit register garbage there, so
  /// two identical draws legitimately disagree on it. Observed on RADV as alpha
  /// alternating between 229 and 0, neither of which is the clear value. Include bit 3
  /// only for workloads that are known to write all four channels.
  std::uint32_t compareMask = 0x07;
};

struct FrameCheckResult {
  bool ok = false;
  std::uint64_t totalPixels = 0;
  std::uint64_t coveredPixels = 0;
  double coveragePercent = 0.0;
  std::uint32_t distinctColors = 0;
  /// True when `candidate` is byte-identical to `reference` (a second readback of the
  /// same draw). Set to false when no reference frame was supplied.
  bool matchesReference = false;
  /// Human-readable summary, surfaced in the run note.
  std::string reason;
};

/// Analyse an RGBA8 framebuffer.
///
/// `rgba` must hold `width * height * 4` bytes. Returns a failed result (never throws)
/// if the buffer is too small. `reference` is an optional second readback of the same
/// draw; when supplied, equality across `config.compareMask` is reported as
/// `matchesReference`.
FrameCheckResult checkFrame(const std::uint8_t *rgba, std::size_t bytes, std::uint32_t width,
                            std::uint32_t height, const FrameCheckConfig &config = {},
                            const std::uint8_t *reference = nullptr, std::size_t referenceBytes = 0);

/// Per-texel divergence between two readbacks of the same draw, over the channels
/// selected by `config.compareMask`. Lets a caller report magnitude and location
/// instead of just "different".
struct FrameDiff {
  std::uint64_t differingTexels = 0;
  std::uint32_t maxChannelDelta = 0;
  double percentDiffering = 0.0;
  std::uint32_t firstX = 0;
  std::uint32_t firstY = 0;
  bool hasDifference = false;
};
FrameDiff diffFrames(const std::uint8_t *a, const std::uint8_t *b, std::size_t bytes,
                     std::uint32_t width, std::uint32_t height,
                     const FrameCheckConfig &config = {});

/// Centre texel of a frame, for the existing `centerR/G/B/A` reporting.
void frameCenterPixel(const std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,
                      std::uint8_t *outRgba);

} // namespace csrun

#endif // CSRUN_VALIDATION_FRAMECHECK_H
