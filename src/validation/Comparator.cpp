#include "csrun/validation/Comparator.h"

#include <cmath>
#include <sstream>

namespace csrun {

CompareResult Comparator::compareFloat32(std::span<const float> reference,
                                         std::span<const float> actual,
                                         const ComparePolicy &policy) {
  CompareResult result;
  if (reference.size() != actual.size()) {
    result.passed = false;
    result.message = "size mismatch";
    return result;
  }

  for (std::size_t i = 0; i < reference.size(); ++i) {
    const float r = reference[i];
    const float a = actual[i];
    if (std::isnan(r) || std::isnan(a)) {
      if (std::isnan(r) != std::isnan(a)) {
        ++result.mismatchCount;
        if (result.mismatchCount == 1)
          result.mismatchIndex = i;
      }
      continue;
    }
    if (std::isinf(r) || std::isinf(a)) {
      if (r != a) {
        ++result.mismatchCount;
        if (result.mismatchCount == 1)
          result.mismatchIndex = i;
      }
      continue;
    }
    const double diff = std::abs(static_cast<double>(r) - static_cast<double>(a));
    const double denom = std::max(std::abs(static_cast<double>(r)), 1e-12);
    const double rel = diff / denom;
    if (diff > policy.absoluteTolerance && rel > policy.relativeTolerance) {
      ++result.mismatchCount;
      if (result.mismatchCount == 1)
        result.mismatchIndex = i;
    }
  }

  result.passed = result.mismatchCount == 0;
  if (result.passed) {
    result.message = "PASS";
  } else {
    std::ostringstream oss;
    oss << "FAIL mismatches=" << result.mismatchCount << " first_index=" << result.mismatchIndex;
    result.message = oss.str();
  }
  return result;
}

} // namespace csrun
