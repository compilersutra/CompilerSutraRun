#include "csrun/core/Types.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace csrun {
namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

} // namespace

std::string_view toString(DataType dtype) {
  switch (dtype) {
  case DataType::Unknown:
    return "unknown";
  case DataType::FP16:
    return "fp16";
  case DataType::BF16:
    return "bf16";
  case DataType::FP32:
    return "fp32";
  case DataType::FP64:
    return "fp64";
  case DataType::Int8:
    return "int8";
  case DataType::Int16:
    return "int16";
  case DataType::Int32:
    return "int32";
  case DataType::Int64:
    return "int64";
  case DataType::UInt8:
    return "uint8";
  case DataType::UInt16:
    return "uint16";
  case DataType::UInt32:
    return "uint32";
  case DataType::UInt64:
    return "uint64";
  }
  return "unknown";
}

DataType parseDataType(std::string_view text) {
  const auto t = lower(text);
  if (t == "fp16" || t == "float16" || t == "half")
    return DataType::FP16;
  if (t == "bf16" || t == "bfloat16")
    return DataType::BF16;
  if (t == "fp32" || t == "float32" || t == "float")
    return DataType::FP32;
  if (t == "fp64" || t == "float64" || t == "double")
    return DataType::FP64;
  if (t == "int8")
    return DataType::Int8;
  if (t == "int16")
    return DataType::Int16;
  if (t == "int32")
    return DataType::Int32;
  if (t == "int64")
    return DataType::Int64;
  if (t == "uint8")
    return DataType::UInt8;
  if (t == "uint16")
    return DataType::UInt16;
  if (t == "uint32")
    return DataType::UInt32;
  if (t == "uint64")
    return DataType::UInt64;
  return DataType::Unknown;
}

} // namespace csrun
