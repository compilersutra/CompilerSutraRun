#include "csrun/benchmark/PipelineStats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <vector>

namespace csrun {
namespace {

std::uint64_t medianU64(std::vector<std::uint64_t> v) {
  if (v.empty())
    return 0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  if (n % 2 == 1)
    return v[n / 2];
  return v[n / 2];
}

double medianF64(std::vector<double> v) {
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  if (n % 2 == 1)
    return v[n / 2];
  return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double stampDeltaMs(std::uint64_t a, std::uint64_t b, double periodNs) {
  if (b < a || periodNs <= 0.0)
    return 0.0;
  return (static_cast<double>(b - a) * periodNs) / 1.0e6;
}

void line(std::ostringstream &o, const char *name, const std::string &value) {
  o << "  " << name << ": " << value << '\n';
}

void lineU(std::ostringstream &o, const char *name, bool ok, std::uint64_t v) {
  if (ok)
    o << "  " << name << ": " << v << '\n';
  else
    o << "  " << name << ": n/a\n";
}

void lineF(std::ostringstream &o, const char *name, bool ok, double v, const char *unit = "") {
  if (ok) {
    o << "  " << name << ": " << v;
    if (unit && unit[0])
      o << ' ' << unit;
    o << '\n';
  } else {
    o << "  " << name << ": n/a\n";
  }
}

bool vendorHas(const std::vector<VendorCounter> &v, const char *needle) {
  for (const auto &c : v) {
    const std::string hay = c.name + " " + c.category + " " + c.description;
    std::string lower;
    lower.reserve(hay.size());
    for (char ch : hay)
      lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    std::string n = needle;
    for (char &ch : n)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.find(n) != std::string::npos)
      return true;
  }
  return false;
}

void vendorLine(std::ostringstream &o, const PipelineStats &s, const char *label,
                const char *needle) {
  for (const auto &c : s.vendor) {
    const std::string hay = c.name + " " + c.category + " " + c.description;
    std::string lower;
    lower.reserve(hay.size());
    for (char ch : hay)
      lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    std::string n = needle;
    for (char &ch : n)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.find(n) != std::string::npos) {
      o << "  " << label << ": " << c.value;
      if (!c.unit.empty())
        o << ' ' << c.unit;
      o << " (" << c.name << ")\n";
      return;
    }
  }
  o << "  " << label << ": n/a";
  if (!s.vendorApi.empty() && s.vendor.empty())
    o << " [" << s.vendorNote << "]";
  else if (s.vendorApi.empty())
    o << " [needs VK_KHR_performance_query on AMD/RADV]";
  o << '\n';
}

} // namespace

bool PipelineStats::fillGraphicsGpuStages(GpuStages &out, const std::uint64_t *stamps,
                                          std::uint32_t count, double timestampPeriodNs) {
  out = GpuStages{};
  if (!stamps || count < kGraphicsTimestampCount || timestampPeriodNs <= 0.0)
    return false;
  for (std::uint32_t i = 1; i < kGraphicsTimestampCount; ++i) {
    if (stamps[i] < stamps[i - 1])
      return false;
  }
  out.available = true;
  out.kind = "graphics";
  out.topToVsMs = stampDeltaMs(stamps[0], stamps[1], timestampPeriodNs);
  out.vsToFsMs = stampDeltaMs(stamps[1], stamps[2], timestampPeriodNs);
  out.fsToColorMs = stampDeltaMs(stamps[2], stamps[3], timestampPeriodNs);
  out.colorToBottomMs = stampDeltaMs(stamps[3], stamps[4], timestampPeriodNs);
  out.totalMs = stampDeltaMs(stamps[0], stamps[4], timestampPeriodNs);
  return true;
}

bool PipelineStats::fillComputeGpuStages(GpuStages &out, const std::uint64_t *stamps,
                                         std::uint32_t count, double timestampPeriodNs) {
  out = GpuStages{};
  if (!stamps || count < kComputeTimestampCount || timestampPeriodNs <= 0.0)
    return false;
  for (std::uint32_t i = 1; i < kComputeTimestampCount; ++i) {
    if (stamps[i] < stamps[i - 1])
      return false;
  }
  out.available = true;
  out.kind = "compute";
  out.topToCsMs = stampDeltaMs(stamps[0], stamps[1], timestampPeriodNs);
  out.csToBottomMs = stampDeltaMs(stamps[1], stamps[2], timestampPeriodNs);
  out.totalMs = stampDeltaMs(stamps[0], stamps[2], timestampPeriodNs);
  return true;
}

std::string PipelineStats::summaryLine() const {
  if (!available && !occlusionAvailable && !gpuTimestampAvailable && vendor.empty())
    return "unavailable";
  std::ostringstream o;
  if (available) {
    o << "IA_V=" << iaVertices << " IA_P=" << iaPrimitives << " VS=" << vsInvocations
      << " TCS=" << tcsPatches << " TES=" << tesInvocations << " GS=" << gsInvocations
      << " GS_P=" << gsPrimitives << " FS/PS=" << fsInvocations << " CS=" << csInvocations
      << " CLIP_I=" << clipInvocations << " CLIP_P=" << clipPrimitives;
    if (vsInvocations)
      o << " FS/VS=" << fsPerVs();
    if (iaPrimitives)
      o << " FS/prim=" << fsPerPrimitive();
  }
  if (occlusionAvailable) {
    if (available)
      o << ' ';
    o << "OCCLUSION=" << occlusionSamplesPassed;
  }
  if (gpuTimestampAvailable)
    o << " gpu_ms=" << gpuElapsedMs;
  if (gpuStages.available) {
    if (gpuStages.kind == "graphics") {
      o << " stages[top→vs=" << gpuStages.topToVsMs << " vs→fs=" << gpuStages.vsToFsMs
        << " fs→color=" << gpuStages.fsToColorMs << " color→bot=" << gpuStages.colorToBottomMs
        << " ms]";
    } else if (gpuStages.kind == "compute") {
      o << " stages[top→cs=" << gpuStages.topToCsMs << " cs→bot=" << gpuStages.csToBottomMs
        << " ms]";
    }
  }
  if (hostRecordMs > 0.0 || hostSubmitMs > 0.0 || hostQueueWaitMs > 0.0 || hostH2dMs > 0.0 ||
      hostD2hMs > 0.0)
    o << " host_record_ms=" << hostRecordMs << " host_submit_ms=" << hostSubmitMs
      << " host_queue_wait_ms=" << hostQueueWaitMs << " host_h2d_ms=" << hostH2dMs
      << " host_d2h_ms=" << hostD2hMs;
  if (!vendor.empty())
    o << " vendor_hw=" << vendor.size();
  return o.str();
}

std::string PipelineStats::checklistReport() const {
  std::ostringstream o;
  o << "=== Portable pipeline statistics (VK_QUERY_TYPE_PIPELINE_STATISTICS) ===\n";
  lineU(o, "input_assembly_vertices", available, iaVertices);
  lineU(o, "input_assembly_primitives", available, iaPrimitives);
  lineU(o, "vertex_shader_invocations", available, vsInvocations);
  lineU(o, "geometry_shader_invocations", available, gsInvocations);
  lineU(o, "geometry_shader_primitives", available, gsPrimitives);
  lineU(o, "clipping_invocations", available, clipInvocations);
  lineU(o, "clipping_primitives", available, clipPrimitives);
  lineU(o, "fragment_shader_invocations (PS)", available, fsInvocations);
  lineU(o, "tessellation_control_patches", available, tcsPatches);
  lineU(o, "tessellation_evaluation_invocations", available, tesInvocations);
  lineU(o, "compute_shader_invocations", available, csInvocations);
  lineF(o, "fs_per_vs", available && vsInvocations > 0, fsPerVs());
  lineF(o, "fs_per_primitive", available && iaPrimitives > 0, fsPerPrimitive());
  lineF(o, "clip_reject_ratio", available, clipRejectRatio());
  lineF(o, "overdraw_estimate", available, overdrawEstimate());

  o << "=== Timing / rendering metrics ===\n";
  lineF(o, "gpu_elapsed_ms (timestamp delta)", gpuTimestampAvailable, gpuElapsedMs, "ms");
  if (gpuStages.available) {
    o << "=== GPU stage timestamps (vkCmdWriteTimestamp markers) ===\n";
    line(o, "kind", gpuStages.kind);
    lineF(o, "total_ms", true, gpuStages.totalMs, "ms");
    if (gpuStages.kind == "graphics") {
      lineF(o, "top_to_vs_ms", true, gpuStages.topToVsMs, "ms");
      lineF(o, "vs_to_fs_ms", true, gpuStages.vsToFsMs, "ms");
      lineF(o, "fs_to_color_ms", true, gpuStages.fsToColorMs, "ms");
      lineF(o, "color_to_bottom_ms", true, gpuStages.colorToBottomMs, "ms");
    } else if (gpuStages.kind == "compute") {
      lineF(o, "top_to_cs_ms", true, gpuStages.topToCsMs, "ms");
      lineF(o, "cs_to_bottom_ms", true, gpuStages.csToBottomMs, "ms");
    }
    o << "  note: markers can overlap on modern GPUs — not exclusive stage wall times\n";
  }
  lineU(o, "occlusion_samples_passed", occlusionAvailable, occlusionSamplesPassed);
  lineF(o, "cpu_command_record_ms", hostRecordMs > 0.0, hostRecordMs, "ms");
  lineF(o, "cpu_submit_ms", hostSubmitMs > 0.0, hostSubmitMs, "ms");
  lineF(o, "queue_sync_wait_ms", hostQueueWaitMs > 0.0, hostQueueWaitMs, "ms");
  lineF(o, "host_h2d_ms", hostH2dMs > 0.0, hostH2dMs, "ms");
  lineF(o, "host_d2h_ms", hostD2hMs > 0.0, hostD2hMs, "ms");
  lineU(o, "draw_count", drawCount > 0 || available, drawCount);
  lineU(o, "dispatch_count", dispatchCount > 0 || (available && csInvocations > 0),
        dispatchCount);

  o << "=== AMD / vendor HW counters (VK_KHR_performance_query) ===\n";
  if (!vendorApi.empty())
    line(o, "vendor_api", vendorApi);
  if (!vendorNote.empty())
    line(o, "vendor_note", vendorNote);
  if (vendor.empty()) {
    if (vendorNote.empty())
      o << "  (no vendor samples — use RADV on discrete AMD; enable performanceCounterQueryPools)\n";
  } else {
    for (const auto &c : vendor) {
      o << "  [" << c.category << "] " << c.name << " = " << c.value;
      if (!c.unit.empty())
        o << ' ' << c.unit;
      o << '\n';
    }
  }

  o << "=== Compute checklist mapping ===\n";
  lineU(o, "CS logical invocations", available, csInvocations);
  lineF(o, "GPU duration (timestamp)", gpuTimestampAvailable, gpuElapsedMs, "ms");
  vendorLine(o, *this, "compute_engine_busy", "busy");
  vendorLine(o, *this, "wave_count", "wave");
  vendorLine(o, *this, "wave_instructions", "instruction");
  vendorLine(o, *this, "VALU activity", "valu");
  vendorLine(o, *this, "SALU activity", "salu");
  vendorLine(o, *this, "VGPR/SGPR usage", "vgpr");
  vendorLine(o, *this, "occupancy / active waves", "occup");
  vendorLine(o, *this, "L2 cache hit", "l2");
  vendorLine(o, *this, "VRAM bandwidth / traffic", "vram");
  vendorLine(o, *this, "memory / dependency stalls", "stall");
  vendorLine(o, *this, "LDS activity", "lds");
  lineF(o, "CPU submission overhead", hostSubmitMs > 0.0, hostSubmitMs, "ms");

  o << "=== Graphics (PS) checklist mapping ===\n";
  lineU(o, "VS invocations", available, vsInvocations);
  lineU(o, "IA vertices", available, iaVertices);
  lineU(o, "IA primitives", available, iaPrimitives);
  lineU(o, "clipping invocations", available, clipInvocations);
  lineU(o, "clipping primitives", available, clipPrimitives);
  lineU(o, "PS invocations", available, fsInvocations);
  lineU(o, "occlusion samples passed", occlusionAvailable, occlusionSamplesPassed);
  lineF(o, "GPU render duration", gpuTimestampAvailable, gpuElapsedMs, "ms");
  vendorLine(o, *this, "fragment wave instructions", "fragment");
  lineF(o, "overdraw / fragment workload", available, overdrawEstimate());
  vendorLine(o, *this, "texture cache activity", "texture");
  vendorLine(o, *this, "VRAM bandwidth", "vram");
  vendorLine(o, *this, "depth/stencil activity", "depth");
  vendorLine(o, *this, "occupancy / shader stalls", "stall");

  (void)vendorHas;
  return o.str();
}

PipelineStats PipelineStats::medianOf(const std::vector<PipelineStats> &samples) {
  PipelineStats out;
  if (samples.empty())
    return out;

  out.available = false;
  out.occlusionAvailable = false;
  out.gpuTimestampAvailable = false;
  for (const auto &s : samples) {
    out.available = out.available || s.available;
    out.occlusionAvailable = out.occlusionAvailable || s.occlusionAvailable;
    out.gpuTimestampAvailable = out.gpuTimestampAvailable || s.gpuTimestampAvailable;
  }

  auto colU = [&](auto getter) {
    std::vector<std::uint64_t> v;
    v.reserve(samples.size());
    for (const auto &s : samples)
      v.push_back(getter(s));
    return medianU64(std::move(v));
  };
  auto colF = [&](auto getter) {
    std::vector<double> v;
    v.reserve(samples.size());
    for (const auto &s : samples)
      v.push_back(getter(s));
    return medianF64(std::move(v));
  };

  out.iaVertices = colU([](const PipelineStats &s) { return s.iaVertices; });
  out.iaPrimitives = colU([](const PipelineStats &s) { return s.iaPrimitives; });
  out.vsInvocations = colU([](const PipelineStats &s) { return s.vsInvocations; });
  out.tcsPatches = colU([](const PipelineStats &s) { return s.tcsPatches; });
  out.tesInvocations = colU([](const PipelineStats &s) { return s.tesInvocations; });
  out.gsInvocations = colU([](const PipelineStats &s) { return s.gsInvocations; });
  out.gsPrimitives = colU([](const PipelineStats &s) { return s.gsPrimitives; });
  out.fsInvocations = colU([](const PipelineStats &s) { return s.fsInvocations; });
  out.csInvocations = colU([](const PipelineStats &s) { return s.csInvocations; });
  out.clipInvocations = colU([](const PipelineStats &s) { return s.clipInvocations; });
  out.clipPrimitives = colU([](const PipelineStats &s) { return s.clipPrimitives; });
  out.occlusionSamplesPassed =
      colU([](const PipelineStats &s) { return s.occlusionSamplesPassed; });
  out.drawCount = colU([](const PipelineStats &s) { return s.drawCount; });
  out.dispatchCount = colU([](const PipelineStats &s) { return s.dispatchCount; });
  out.gpuElapsedMs = colF([](const PipelineStats &s) { return s.gpuElapsedMs; });
  out.hostRecordMs = colF([](const PipelineStats &s) { return s.hostRecordMs; });
  out.hostSubmitMs = colF([](const PipelineStats &s) { return s.hostSubmitMs; });
  out.hostQueueWaitMs = colF([](const PipelineStats &s) { return s.hostQueueWaitMs; });
  out.hostH2dMs = colF([](const PipelineStats &s) { return s.hostH2dMs; });
  out.hostD2hMs = colF([](const PipelineStats &s) { return s.hostD2hMs; });

  // GPU stages: median per field; keep kind from first available sample.
  {
    out.gpuStages = GpuStages{};
    for (const auto &s : samples) {
      if (s.gpuStages.available) {
        out.gpuStages.kind = s.gpuStages.kind;
        out.gpuStages.available = true;
        break;
      }
    }
    if (out.gpuStages.available) {
      out.gpuStages.totalMs = colF([](const PipelineStats &s) { return s.gpuStages.totalMs; });
      out.gpuStages.topToVsMs = colF([](const PipelineStats &s) { return s.gpuStages.topToVsMs; });
      out.gpuStages.vsToFsMs = colF([](const PipelineStats &s) { return s.gpuStages.vsToFsMs; });
      out.gpuStages.fsToColorMs =
          colF([](const PipelineStats &s) { return s.gpuStages.fsToColorMs; });
      out.gpuStages.colorToBottomMs =
          colF([](const PipelineStats &s) { return s.gpuStages.colorToBottomMs; });
      out.gpuStages.topToCsMs = colF([](const PipelineStats &s) { return s.gpuStages.topToCsMs; });
      out.gpuStages.csToBottomMs =
          colF([](const PipelineStats &s) { return s.gpuStages.csToBottomMs; });
    }
  }

  // Vendor: take the sample with the most counters (medians per-name when aligned).
  size_t best = 0;
  for (size_t i = 1; i < samples.size(); ++i) {
    if (samples[i].vendor.size() > samples[best].vendor.size())
      best = i;
  }
  out.vendorApi = samples[best].vendorApi;
  out.vendorNote = samples[best].vendorNote;
  out.vendor = samples[best].vendor;
  if (!out.vendor.empty()) {
    // Median each vendor counter by name across samples that have it.
    std::vector<VendorCounter> merged;
    for (const auto &ref : out.vendor) {
      std::vector<double> vals;
      for (const auto &s : samples) {
        for (const auto &c : s.vendor) {
          if (c.name == ref.name && c.category == ref.category) {
            vals.push_back(c.value);
            break;
          }
        }
      }
      VendorCounter m = ref;
      m.value = medianF64(std::move(vals));
      merged.push_back(std::move(m));
    }
    out.vendor = std::move(merged);
  }
  return out;
}

} // namespace csrun
