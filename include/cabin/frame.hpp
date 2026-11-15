#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cabin/bytes.hpp"
#include "cabin/result.hpp"
#include "cabin_version.hpp"

namespace cabin {

// Wire frame:
//   magic(4) | idl_major(1) | idl_minor(1) | msg_type(1) | flags(1) |
//   service_id(2) | method_id(2) | seq(4) | trace_id(8) | payload_len(4) |
//   payload | crc32(4)
//
// All integers little-endian; CRC-32 covers header and payload.
inline constexpr std::size_t kFrameHeaderSize = 28;
inline constexpr std::size_t kFrameCrcSize = 4;
inline constexpr std::uint32_t kMaxFramePayload = 16u * 1024u * 1024u;

inline constexpr char kFrameMagic[4] = {'C', 'B', 'N', '1'};

enum class MsgType : std::uint8_t {
  kRequest = 1,
  kResponse = 2,
  kError = 3,
  kEvent = 4,
  kOfferService = 5,
  kFindService = 6,
  kServiceAvailable = 7,
  kServiceUnavailable = 8,
  kSubscribe = 9,
  kUnsubscribe = 10,
  kNotify = 11,
  kPing = 12,
  kPong = 13,
};

const char* msg_type_name(MsgType type);

struct Frame {
  std::uint8_t idl_major{gen::kIdlMajor};
  std::uint8_t idl_minor{gen::kIdlMinor};
  MsgType msg_type{MsgType::kRequest};
  std::uint8_t flags{0};
  std::uint16_t service_id{0};
  std::uint16_t method_id{0};
  std::uint32_t seq{0};
  std::uint64_t trace_id{0};
  Bytes payload;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size);
std::uint32_t crc32(const Bytes& bytes);

// Error frames carry a numeric ErrorCode followed by a length-prefixed message
// so clients can preserve the exact failure reason across the broker.
Bytes encode_error_payload(const Error& error);
Error decode_error_payload(const Bytes& payload);

// Serializes a frame. The returned buffer contains exactly one frame.
Result<Bytes> encode_frame(const Frame& frame);

// Decodes one frame from the front of `data`. On success `consumed` is the
// number of bytes read. Rejects bad magic, bad CRC, truncation and a mismatched
// IDL major version (minor mismatches are accepted for forward compatibility).
Result<Frame> decode_frame(const std::uint8_t* data, std::size_t size,
                           std::size_t* consumed);

// Streaming decoder for sockets. Feed arbitrary chunks, pull complete frames.
class FrameDecoder {
 public:
  void feed(const std::uint8_t* data, std::size_t size);
  void feed(const Bytes& bytes) { feed(bytes.data(), bytes.size()); }
  Result<std::optional<Frame>> next();
  std::size_t buffered() const { return buffer_.size() - offset_; }
  void clear() {
    buffer_.clear();
    offset_ = 0;
  }

 private:
  std::vector<std::uint8_t> buffer_;
  std::size_t offset_{0};
};

} // namespace cabin
