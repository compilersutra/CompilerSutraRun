//===----------------------------------------------------------------------===//
//
// ExecuteOptions.h
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BACKEND_EXECUTEOPTIONS_H
#define CSRUN_BACKEND_EXECUTEOPTIONS_H

#include "csrun/benchmark/VendorCountersMode.h"

#include <cstdint>
#include <optional>

namespace csrun {

/// Control-plane knobs for Backend::execute (CLI / runtime).
struct ExecuteOptions {
  bool validate = true;
  bool benchmark = true;
  /// Override workload.execution.show when set.
  bool showWindow = true;
  bool showWindowSet = false;
  /// Force Vulkan/HIP/… physical device index when set.
  std::optional<std::uint32_t> deviceIndex;
  /// Vendor HW counter selection (RADV VK_KHR_performance_query).
  VendorCountersMode vendorCounters = VendorCountersMode::PreferSinglePass;
};

} // namespace csrun

#endif
