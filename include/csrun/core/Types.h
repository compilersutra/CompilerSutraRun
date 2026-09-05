//===----------------------------------------------------------------------===//
// Types.h - Shared strong types and enums
//===----------------------------------------------------------------------===//

#ifndef CSRUN_CORE_TYPES_H
#define CSRUN_CORE_TYPES_H

#include <cstdint>
#include <string>
#include <string_view>

namespace csrun {

enum class DataType {
  Unknown,
  FP16,
  BF16,
  FP32,
  FP64,
  Int8,
  Int16,
  Int32,
  Int64,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
};

std::string_view toString(DataType dtype);
DataType parseDataType(std::string_view text);

struct Dimensions {
  std::uint64_t m = 0;
  std::uint64_t n = 0;
  std::uint64_t k = 0;
  std::uint64_t count = 0;
};

} // namespace csrun

#endif
