#include "cabin/signals/codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "cabin/log.hpp"

namespace cabin::signals {
namespace {

std::uint64_t max_raw(std::uint8_t bit_length) {
  if (bit_length >= 64)
    return ~0ULL;
  return (1ULL << bit_length) - 1ULL;
}

} // namespace

std::uint64_t SignalCodec::extract_bits(const CanFrame& frame, std::uint16_t bit_offset,
                                        std::uint8_t bit_length) {
  std::uint64_t value = 0;
  for (std::uint8_t index = 0; index < bit_length && index < 64; ++index) {
    const std::uint16_t bit = static_cast<std::uint16_t>(bit_offset + index);
    const std::size_t byte = bit / 8u;
    if (byte >= 8)
      break;
    const std::uint64_t bit_value = (frame.data[byte] >> (bit % 8u)) & 1u;
    value |= bit_value << index;
  }
  return value;
}

void SignalCodec::insert_bits(CanFrame* frame, std::uint16_t bit_offset,
                              std::uint8_t bit_length, std::uint64_t raw_bits) {
  if (frame == nullptr)
    return;
  for (std::uint8_t index = 0; index < bit_length && index < 64; ++index) {
    const std::uint16_t bit = static_cast<std::uint16_t>(bit_offset + index);
    const std::size_t byte = bit / 8u;
    if (byte >= 8)
      break;
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << (bit % 8u));
    if (((raw_bits >> index) & 1u) != 0) {
      frame->data[byte] |= mask;
    } else {
      frame->data[byte] = static_cast<std::uint8_t>(frame->data[byte] & ~mask);
    }
  }
  const std::size_t needed_bytes =
      (static_cast<std::size_t>(bit_offset) + bit_length + 7u) / 8u;
  if (needed_bytes > frame->dlc) {
    frame->dlc = static_cast<std::uint8_t>(std::min<std::size_t>(needed_bytes, 8));
  }
}

Result<DecodedSignal> SignalCodec::decode_bits(SignalId id,
                                               std::uint64_t raw_bits) const {
  const SignalMeta& meta = signal_meta(id);
  if (meta.id != id) {
    return make_error(ErrorCode::kInvalidArgument, "unknown signal id");
  }
  double raw_value = static_cast<double>(raw_bits);
  if (meta.can.encoding == SignalEncoding::kFloat32) {
    if (meta.can.bit_length != 32) {
      return make_error(ErrorCode::kInvalidArgument,
                        "float32 signal must be 32 bits wide");
    }
    const auto bits = static_cast<std::uint32_t>(raw_bits);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    raw_value = value;
  }
  double value = raw_value * meta.can.scale + meta.can.offset;
  SignalQuality quality = SignalQuality::kOk;
  bool clamped = false;
  if (value < meta.min) {
    value = meta.min;
    clamped = true;
  } else if (value > meta.max) {
    value = meta.max;
    clamped = true;
  }
  if (clamped) {
    quality = SignalQuality::kClamped;
    log_warn(
        "signals", "signal_out_of_range",
        {Field("signal", meta.name), Field("raw_value", raw_value),
         Field("clamped_value", value), Field("min", meta.min), Field("max", meta.max)});
  }
  DecodedSignal decoded;
  decoded.id = id;
  decoded.value = value;
  decoded.raw_value = raw_value;
  decoded.quality = quality;
  return decoded;
}

std::size_t SignalCodec::decode_into(const CanFrame& frame, DecodedSignal* out,
                                     std::size_t capacity) const {
  std::size_t count = 0;
  for (std::size_t index = 0; index < kSignalCount && count < capacity; ++index) {
    const auto id = static_cast<SignalId>(index);
    const SignalMeta& meta = signal_meta(id);
    if (meta.can.frame_id != frame.id)
      continue;
    const std::size_t needed_bits =
        static_cast<std::size_t>(meta.can.bit_offset) + meta.can.bit_length;
    if (needed_bits > static_cast<std::size_t>(frame.dlc) * 8u) {
      log_warn("signals", "frame_too_short",
               {Field("signal", meta.name),
                Field("frame_id", static_cast<std::int64_t>(frame.id)),
                Field("dlc", static_cast<std::int64_t>(frame.dlc))});
      continue;
    }
    const std::uint64_t raw =
        extract_bits(frame, meta.can.bit_offset, meta.can.bit_length);
    auto value = decode_bits(id, raw);
    if (!value)
      continue;
    out[count++] = value.value();
  }
  return count;
}

Result<std::vector<DecodedSignal>> SignalCodec::decode(const CanFrame& frame) const {
  std::array<DecodedSignal, kSignalCount> buffer{};
  const std::size_t count = decode_into(frame, buffer.data(), buffer.size());
  std::vector<DecodedSignal> decoded;
  decoded.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    decoded.push_back(buffer[index]);
  }
  return decoded;
}

Result<CanFrame> SignalCodec::encode(SignalId id, double value) const {
  const SignalMeta& meta = signal_meta(id);
  if (meta.id != id) {
    return make_error(ErrorCode::kInvalidArgument, "unknown signal id");
  }
  CanFrame frame;
  frame.id = meta.can.frame_id;
  auto status = encode_into(&frame, id, value);
  if (!status)
    return status.error();
  return frame;
}

Status SignalCodec::encode_into(CanFrame* frame, SignalId id, double value) const {
  if (frame == nullptr) {
    return make_error(ErrorCode::kInvalidArgument, "null frame");
  }
  const SignalMeta& meta = signal_meta(id);
  if (meta.id != id) {
    return make_error(ErrorCode::kInvalidArgument, "unknown signal id");
  }
  if (!std::isfinite(value) || value < meta.min || value > meta.max) {
    return make_error(ErrorCode::kOutOfRange, "value for " + std::string(meta.name) +
                                                  " is outside [" +
                                                  std::to_string(meta.min) + ", " +
                                                  std::to_string(meta.max) + "]");
  }
  const double scaled = (value - meta.can.offset) / meta.can.scale;
  std::uint64_t raw = 0;
  if (meta.can.encoding == SignalEncoding::kFloat32) {
    const float as_float = static_cast<float>(scaled);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &as_float, sizeof(bits));
    raw = bits;
  } else {
    const double rounded = std::round(scaled);
    if (rounded < 0.0 || rounded > static_cast<double>(max_raw(meta.can.bit_length))) {
      return make_error(ErrorCode::kOutOfRange,
                        "quantized value does not fit the CAN bit width");
    }
    raw = static_cast<std::uint64_t>(rounded);
  }
  frame->id = meta.can.frame_id;
  const std::size_t needed_bytes =
      (static_cast<std::size_t>(meta.can.bit_offset) + meta.can.bit_length + 7u) / 8u;
  if (needed_bytes > 8) {
    return make_error(ErrorCode::kOutOfRange, "signal exceeds an 8-byte CAN frame");
  }
  if (frame->dlc < needed_bytes)
    frame->dlc = static_cast<std::uint8_t>(needed_bytes);
  insert_bits(frame, meta.can.bit_offset, meta.can.bit_length, raw);
  return ok_status();
}

} // namespace cabin::signals
