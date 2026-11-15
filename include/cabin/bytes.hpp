#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "cabin/result.hpp"

namespace cabin {

// Little-endian, bounds-checked byte buffer. It owns its storage and keeps a
// read cursor; `data()` always exposes the full backing buffer so callers can
// append after decoding. Every read validates the remaining length and returns
// ErrorCode::kTruncated instead of reading out of bounds.
class Bytes {
 public:
  Bytes() = default;
  explicit Bytes(std::vector<std::uint8_t> data) : data_(std::move(data)) {}
  explicit Bytes(std::string_view text) : data_(text.begin(), text.end()) {}

  const std::uint8_t* data() const { return data_.data(); }
  std::uint8_t* mutable_data() { return data_.data(); }
  std::size_t size() const { return data_.size(); }
  std::size_t read_offset() const { return read_; }
  std::size_t remaining() const { return data_.size() - read_; }
  bool empty() const { return data_.empty(); }

  void clear() {
    data_.clear();
    read_ = 0;
  }
  void rewind() { read_ = 0; }
  void skip(std::size_t count) { read_ += count; }
  void reserve(std::size_t count) { data_.reserve(count); }

  void append_u8(std::uint8_t value) { data_.push_back(value); }
  void append_u16(std::uint16_t value) {
    data_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    data_.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
  }
  void append_u32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      data_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }
  void append_u64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      data_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
    }
  }
  void append_i32(std::int32_t value) { append_u32(static_cast<std::uint32_t>(value)); }
  void append_f32(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(bits);
  }
  void append_f64(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u64(bits);
  }
  void append_bytes(const std::uint8_t* data, std::size_t count) {
    data_.insert(data_.end(), data, data + count);
  }
  void append_bytes(const Bytes& other) {
    append_bytes(other.data_.data(), other.data_.size());
  }
  void append_string(std::string_view value) {
    if (value.size() > 0xFFFFu) {
      value = value.substr(0, 0xFFFFu);
    }
    append_u16(static_cast<std::uint16_t>(value.size()));
    data_.insert(data_.end(), value.begin(), value.end());
  }

  Result<std::uint8_t> read_u8() {
    if (remaining() < 1)
      return truncated("u8");
    return data_[read_++];
  }
  Result<std::uint16_t> read_u16() {
    if (remaining() < 2)
      return truncated("u16");
    std::uint16_t value = static_cast<std::uint16_t>(data_[read_]) |
                          static_cast<std::uint16_t>(data_[read_ + 1] << 8u);
    read_ += 2;
    return value;
  }
  Result<std::uint32_t> read_u32() {
    if (remaining() < 4)
      return truncated("u32");
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(data_[read_ + static_cast<std::size_t>(i)])
               << (8 * i);
    }
    read_ += 4;
    return value;
  }
  Result<std::uint64_t> read_u64() {
    if (remaining() < 8)
      return truncated("u64");
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(data_[read_ + static_cast<std::size_t>(i)])
               << (8 * i);
    }
    read_ += 8;
    return value;
  }
  Result<std::int32_t> read_i32() {
    auto value = read_u32();
    if (!value)
      return value.error();
    return static_cast<std::int32_t>(value.value());
  }
  Result<float> read_f32() {
    auto bits = read_u32();
    if (!bits)
      return bits.error();
    float value = 0.0F;
    std::memcpy(&value, &bits.value(), sizeof(value));
    return value;
  }
  Result<double> read_f64() {
    auto bits = read_u64();
    if (!bits)
      return bits.error();
    double value = 0.0;
    std::memcpy(&value, &bits.value(), sizeof(value));
    return value;
  }
  Result<std::string> read_string() {
    auto length = read_u16();
    if (!length)
      return length.error();
    if (remaining() < length.value())
      return truncated("string body");
    std::string value(reinterpret_cast<const char*>(data_.data() + read_),
                      length.value());
    read_ += length.value();
    return value;
  }
  Result<Bytes> read_bytes(std::size_t count) {
    if (remaining() < count)
      return truncated("bytes");
    Bytes out;
    out.data_.assign(data_.begin() + static_cast<std::ptrdiff_t>(read_),
                     data_.begin() + static_cast<std::ptrdiff_t>(read_ + count));
    read_ += count;
    return out;
  }

  std::vector<std::uint8_t> take() {
    read_ = 0;
    return std::move(data_);
  }
  const std::vector<std::uint8_t>& storage() const { return data_; }

 private:
  static Error truncated(const char* what) {
    return make_error(ErrorCode::kTruncated, std::string("truncated read of ") + what);
  }

  std::vector<std::uint8_t> data_;
  std::size_t read_{0};
};

} // namespace cabin
