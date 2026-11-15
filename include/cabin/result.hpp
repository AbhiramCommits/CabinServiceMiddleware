#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace cabin {

// Stable numeric error codes. They are part of the wire ABI: gateways and
// log tooling map them directly onto JSON `code` fields.
enum class ErrorCode : std::int32_t {
  kOk = 0,
  kInvalidArgument = 1,
  kOutOfRange = 2,
  kTruncated = 3,
  kBadMagic = 4,
  kBadCrc = 5,
  kVersionMismatch = 6,
  kTimeout = 7,
  kNotFound = 8,
  kUnavailable = 9,
  kQueueOverflow = 10,
  kIo = 11,
  kParse = 12,
  kUnsupported = 13,
  kAlreadyExists = 14,
  kInternal = 15,
};

struct Error {
  ErrorCode code{ErrorCode::kInternal};
  std::string message;

  Error() = default;
  Error(ErrorCode c, std::string m) : code(c), message(std::move(m)) {}
};

inline Error make_error(ErrorCode code, std::string message) {
  return Error{code, std::move(message)};
}

inline const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk:
      return "OK";
    case ErrorCode::kInvalidArgument:
      return "INVALID_ARGUMENT";
    case ErrorCode::kOutOfRange:
      return "OUT_OF_RANGE";
    case ErrorCode::kTruncated:
      return "TRUNCATED";
    case ErrorCode::kBadMagic:
      return "BAD_MAGIC";
    case ErrorCode::kBadCrc:
      return "BAD_CRC";
    case ErrorCode::kVersionMismatch:
      return "VERSION_MISMATCH";
    case ErrorCode::kTimeout:
      return "TIMEOUT";
    case ErrorCode::kNotFound:
      return "NOT_FOUND";
    case ErrorCode::kUnavailable:
      return "UNAVAILABLE";
    case ErrorCode::kQueueOverflow:
      return "QUEUE_OVERFLOW";
    case ErrorCode::kIo:
      return "IO";
    case ErrorCode::kParse:
      return "PARSE";
    case ErrorCode::kUnsupported:
      return "UNSUPPORTED";
    case ErrorCode::kAlreadyExists:
      return "ALREADY_EXISTS";
    case ErrorCode::kInternal:
      return "INTERNAL";
  }
  return "INTERNAL";
}

// Value-or-error. Hot paths never throw; every fallible operation returns a
// Result and callers must inspect it before touching `value()`.
template <typename T>
class [[nodiscard]] Result {
 public:
  Result(T value) : storage_(std::move(value)) {} // NOLINT(google-explicit-constructor)
  Result(Error error)
      : storage_(std::move(error)) {} // NOLINT(google-explicit-constructor)
  Result(ErrorCode code, std::string message)
      : storage_(Error{code, std::move(message)}) {}

  bool ok() const { return std::holds_alternative<T>(storage_); }
  explicit operator bool() const { return ok(); }

  T& value() & { return std::get<T>(storage_); }
  const T& value() const& { return std::get<T>(storage_); }
  T&& value() && { return std::get<T>(std::move(storage_)); }

  const Error& error() const { return std::get<Error>(storage_); }
  ErrorCode code() const {
    return ok() ? ErrorCode::kOk : std::get<Error>(storage_).code;
  }

 private:
  std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() : error_(ErrorCode::kOk, "") {}
  Result(Error error) : error_(std::move(error)) {} // NOLINT(google-explicit-constructor)
  Result(ErrorCode code, std::string message) : error_(Error{code, std::move(message)}) {}

  bool ok() const { return error_.code == ErrorCode::kOk; }
  explicit operator bool() const { return ok(); }
  const Error& error() const { return error_; }
  ErrorCode code() const { return error_.code; }

 private:
  Error error_;
};

using Status = Result<void>;

// NOLINTNEXTLINE(google-explicit-constructor)
inline Status ok_status() {
  return Status{};
}

} // namespace cabin
