//===----------------------------------------------------------------------===//
// Expected.h - Result-or-error container (no exceptions for expected failures)
//===----------------------------------------------------------------------===//

#ifndef CSRUN_CORE_EXPECTED_H
#define CSRUN_CORE_EXPECTED_H

#include "csrun/core/Error.h"

#include <utility>
#include <variant>

namespace csrun {

template <typename T>
class Expected {
public:
  Expected(T value) : storage_(std::move(value)) {}
  Expected(Error error) : storage_(std::move(error)) {}

  bool hasValue() const { return std::holds_alternative<T>(storage_); }
  bool hasError() const { return std::holds_alternative<Error>(storage_); }
  explicit operator bool() const { return hasValue(); }

  T &value() & { return std::get<T>(storage_); }
  const T &value() const & { return std::get<T>(storage_); }
  T &&value() && { return std::get<T>(std::move(storage_)); }

  Error &error() & { return std::get<Error>(storage_); }
  const Error &error() const & { return std::get<Error>(storage_); }

private:
  std::variant<T, Error> storage_;
};

template <>
class Expected<void> {
public:
  Expected() : ok_(true) {}
  Expected(Error error) : ok_(false), error_(std::move(error)) {}

  bool hasValue() const { return ok_; }
  bool hasError() const { return !ok_; }
  explicit operator bool() const { return ok_; }

  Error &error() & { return error_; }
  const Error &error() const & { return error_; }

private:
  bool ok_ = false;
  Error error_;
};

} // namespace csrun

#endif
