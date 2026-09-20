//===----------------------------------------------------------------------===//
// PipelineStats.h - Portable Vulkan counters + vendor HW samples + host timing
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BENCHMARK_PIPELINESTATS_H
#define CSRUN_BENCHMARK_PIPELINESTATS_H

#include <cstdint>
#include <string>
#include <vector>

namespace csrun {

/// One sample from VK_KHR_performance_query (RADV/AMDVLK/etc.).
struct VendorCounter {
  std::string name;
  std::string category;
  std::string description;
  std::string unit;
  double value = 0.0;
};

/// Portable GPU counters + host instrumentation + optional vendor HW counters.
struct PipelineStats {
  bool available = false;
  bool occlusionAvailable = false;
  bool gpuTimestampAvailable = false;

  // Input assembly
  std::uint64_t iaVertices = 0;
  std::uint64_t iaPrimitives = 0;

  // Shader invocations
  std::uint64_t vsInvocations = 0; // vertex (VS)
  std::uint64_t tcsPatches = 0;
  std::uint64_t tesInvocations = 0;
  std::uint64_t gsInvocations = 0;
  std::uint64_t gsPrimitives = 0;
  std::uint64_t fsInvocations = 0; // fragment / pixel (PS)
  std::uint64_t csInvocations = 0; // compute

  // Clipping
  std::uint64_t clipInvocations = 0;
  std::uint64_t clipPrimitives = 0;

  // Occlusion (samples that pass depth/stencil tests)
  std::uint64_t occlusionSamplesPassed = 0;

  // Workload bookkeeping
  std::uint64_t drawCount = 0;
  std::uint64_t dispatchCount = 0;

  // GPU elapsed (timestamp delta), when available
  double gpuElapsedMs = 0.0;

  /// Multi-marker GPU timestamps (pipeline-stage brackets). Overlap means these
  /// are progress markers, not exclusive exclusive VS/FS wall times.
  struct GpuStages {
    bool available = false;
    std::string kind; // "graphics" | "compute"
    double totalMs = 0.0;
    // graphics: TOP → VS → FS → COLOR → BOTTOM
    double topToVsMs = 0.0;
    double vsToFsMs = 0.0;
    double fsToColorMs = 0.0;
    double colorToBottomMs = 0.0;
    // compute: TOP → CS → BOTTOM
    double topToCsMs = 0.0;
    double csToBottomMs = 0.0;
  } gpuStages;

  static constexpr std::uint32_t kGraphicsTimestampCount = 5;
  static constexpr std::uint32_t kComputeTimestampCount = 3;

  /// Fill gpuStages from raw timestamp query results (tick units).
  static bool fillGraphicsGpuStages(GpuStages &out, const std::uint64_t *stamps,
                                    std::uint32_t count, double timestampPeriodNs);
  static bool fillComputeGpuStages(GpuStages &out, const std::uint64_t *stamps,
                                   std::uint32_t count, double timestampPeriodNs);

  // Host-side (CPU) instrumentation
  double hostRecordMs = 0.0;    // command buffer record
  double hostSubmitMs = 0.0;    // queue submit through return
  double hostQueueWaitMs = 0.0; // fence / queue wait idle (sync)
  double hostH2dMs = 0.0;       // host→device upload (mapped memcpy today)
  double hostD2hMs = 0.0;       // device→host readback

  // Vendor HW counters (VK_KHR_performance_query when present)
  std::string vendorApi; // e.g. "VK_KHR_performance_query"
  std::string vendorNote;
  std::vector<VendorCounter> vendor;

  double fsPerVs() const {
    return vsInvocations ? static_cast<double>(fsInvocations) / static_cast<double>(vsInvocations)
                         : 0.0;
  }
  double fsPerPrimitive() const {
    return iaPrimitives ? static_cast<double>(fsInvocations) / static_cast<double>(iaPrimitives)
                        : 0.0;
  }
  double clipRejectRatio() const {
    if (clipInvocations == 0 || clipPrimitives >= clipInvocations)
      return 0.0;
    return 1.0 - (static_cast<double>(clipPrimitives) / static_cast<double>(clipInvocations));
  }
  /// Approx overdraw vs framebuffer samples (when occlusion known) or vs IA prims.
  double overdrawEstimate() const {
    if (occlusionAvailable && occlusionSamplesPassed > 0)
      return static_cast<double>(fsInvocations) / static_cast<double>(occlusionSamplesPassed);
    if (iaPrimitives > 0)
      return fsPerPrimitive();
    return 0.0;
  }

  /// One-line summary (CLI header).
  std::string summaryLine() const;

  /// Multi-section checklist report (portable + timing + AMD/vendor).
  std::string checklistReport() const;

  /// Per-field median across samples (for multi-iter benchmarks).
  static PipelineStats medianOf(const std::vector<PipelineStats> &samples);
};

} // namespace csrun

#endif
