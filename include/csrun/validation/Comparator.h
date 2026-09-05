//===----------------------------------------------------------------------===//
// Comparator.h - Deterministic numeric comparison
//===----------------------------------------------------------------------===//

#ifndef CSRUN_VALIDATION_COMPARATOR_H
#define CSRUN_VALIDATION_COMPARATOR_H

#include "csrun/core/Expected.h"

#include <cstddef>
#include <span>
#include <string>

namespace csrun {

struct ComparePolicy {
  double absoluteTolerance = 1e-5;
  double relativeTolerance = 1e-5;
};

struct CompareResult {
  bool passed = false;
  std::size_t mismatchIndex = 0;
  std::size_t mismatchCount = 0;
  std::string message;
};

class Comparator {
public:
  static CompareResult compareFloat32(std::span<const float> reference,
                                      std::span<const float> actual,
                                      const ComparePolicy &policy);
};

} // namespace csrun

#endif
