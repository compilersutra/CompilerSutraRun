//===----------------------------------------------------------------------===//
// VendorCountersMode.h - portable / vendor counter selection knobs
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BENCHMARK_VENDORCOUNTERSMODE_H
#define CSRUN_BENCHMARK_VENDORCOUNTERSMODE_H

namespace csrun {

/// How many VK_KHR_performance_query counters to enable.
enum class VendorCountersMode {
  /// Skip VK_KHR_performance_query (no profiling lock — safer when display
  /// shares the same dGPU).
  Off,
  /// Every counter the driver exposes (may need 2 submit passes on GFX12).
  All,
  /// Largest set that fits in one pass (WSI / present path).
  PreferSinglePass,
};

} // namespace csrun

#endif
