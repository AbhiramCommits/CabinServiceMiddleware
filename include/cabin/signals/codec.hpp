#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cabin/result.hpp"
#include "cabin/signals/can.hpp"
#include "cabin_can_map.hpp"

namespace cabin::signals {

enum class SignalQuality : std::uint8_t { kOk = 0, kClamped = 1, kInvalid = 2 };

struct DecodedSignal {
  SignalId id{SignalId::kCount};
  double value{0.0};
  double raw_value{0.0};
  SignalQuality quality{SignalQuality::kOk};
};

// Mapping-driven codec: CAN bits -> engineering units with scale/offset and a
// range clamp, and typed values back to CAN bits. Decoding is deliberately
// lossy-safe (clamp + warn); encoding is strict (reject out of range).
class SignalCodec {
 public:
  Result<DecodedSignal> decode_bits(SignalId id, std::uint64_t raw_bits) const;
  Result<std::vector<DecodedSignal>> decode(const CanFrame& frame) const;
  std::size_t decode_into(const CanFrame& frame, DecodedSignal* out,
                          std::size_t capacity) const;
  Result<CanFrame> encode(SignalId id, double value) const;
  Status encode_into(CanFrame* frame, SignalId id, double value) const;

  static std::uint64_t extract_bits(const CanFrame& frame, std::uint16_t bit_offset,
                                    std::uint8_t bit_length);
  static void insert_bits(CanFrame* frame, std::uint16_t bit_offset,
                          std::uint8_t bit_length, std::uint64_t raw_bits);
};

} // namespace cabin::signals
