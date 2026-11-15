#include "cabin/frame.hpp"

#include <cstring>

namespace cabin {
namespace {

std::uint8_t byte_at(const std::uint8_t* data, std::size_t index) {
  return data[index];
}

std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8u);
}

std::uint32_t read_u32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8u) |
         (static_cast<std::uint32_t>(data[2]) << 16u) |
         (static_cast<std::uint32_t>(data[3]) << 24u);
}

std::uint64_t read_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8u) | data[i];
  }
  return value;
}

} // namespace

const char* msg_type_name(MsgType type) {
  switch (type) {
    case MsgType::kRequest:
      return "REQUEST";
    case MsgType::kResponse:
      return "RESPONSE";
    case MsgType::kError:
      return "ERROR";
    case MsgType::kEvent:
      return "EVENT";
    case MsgType::kOfferService:
      return "OFFER_SERVICE";
    case MsgType::kFindService:
      return "FIND_SERVICE";
    case MsgType::kServiceAvailable:
      return "SERVICE_AVAILABLE";
    case MsgType::kServiceUnavailable:
      return "SERVICE_UNAVAILABLE";
    case MsgType::kSubscribe:
      return "SUBSCRIBE";
    case MsgType::kUnsubscribe:
      return "UNSUBSCRIBE";
    case MsgType::kNotify:
      return "NOTIFY";
    case MsgType::kPing:
      return "PING";
    case MsgType::kPong:
      return "PONG";
  }
  return "UNKNOWN";
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1u) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

std::uint32_t crc32(const Bytes& bytes) {
  return crc32(bytes.data(), bytes.size());
}

Bytes encode_error_payload(const Error& error) {
  Bytes payload;
  payload.append_u32(static_cast<std::uint32_t>(error.code));
  payload.append_string(error.message);
  return payload;
}

Error decode_error_payload(const Bytes& payload) {
  Bytes copy = payload;
  auto code = copy.read_u32();
  if (!code) {
    return make_error(ErrorCode::kParse, "malformed error payload");
  }
  auto message = copy.read_string();
  if (!message) {
    return make_error(ErrorCode::kParse, "malformed error message");
  }
  const auto raw = code.value();
  if (raw > static_cast<std::uint32_t>(ErrorCode::kInternal)) {
    return make_error(static_cast<ErrorCode>(raw), message.value());
  }
  return Error{static_cast<ErrorCode>(raw), message.value()};
}

Result<Bytes> encode_frame(const Frame& frame) {
  if (frame.payload.size() > kMaxFramePayload) {
    return make_error(ErrorCode::kInvalidArgument, "frame payload exceeds limit");
  }
  Bytes out;
  out.reserve(kFrameHeaderSize + frame.payload.size() + kFrameCrcSize);
  out.append_bytes(reinterpret_cast<const std::uint8_t*>(kFrameMagic), 4);
  out.append_u8(frame.idl_major);
  out.append_u8(frame.idl_minor);
  out.append_u8(static_cast<std::uint8_t>(frame.msg_type));
  out.append_u8(frame.flags);
  out.append_u16(frame.service_id);
  out.append_u16(frame.method_id);
  out.append_u32(frame.seq);
  out.append_u64(frame.trace_id);
  out.append_u32(static_cast<std::uint32_t>(frame.payload.size()));
  out.append_bytes(frame.payload);
  const std::uint32_t checksum = crc32(out.data(), out.size());
  out.append_u32(checksum);
  return out;
}

Result<Frame> decode_frame(const std::uint8_t* data, std::size_t size,
                           std::size_t* consumed) {
  if (consumed != nullptr)
    *consumed = 0;
  if (data == nullptr) {
    return make_error(ErrorCode::kInvalidArgument, "null frame buffer");
  }
  if (size < 4) {
    return make_error(ErrorCode::kTruncated, "frame shorter than magic");
  }
  if (std::memcmp(data, kFrameMagic, 4) != 0) {
    return make_error(ErrorCode::kBadMagic, "bad frame magic");
  }
  if (size < kFrameHeaderSize) {
    return make_error(ErrorCode::kTruncated, "frame shorter than header");
  }
  const std::uint32_t payload_len = read_u32(data + 24);
  if (payload_len > kMaxFramePayload) {
    return make_error(ErrorCode::kInvalidArgument, "frame payload exceeds limit");
  }
  const std::size_t total =
      kFrameHeaderSize + static_cast<std::size_t>(payload_len) + kFrameCrcSize;
  if (size < total) {
    return make_error(ErrorCode::kTruncated, "frame payload truncated");
  }
  const std::uint32_t expected_crc = read_u32(data + kFrameHeaderSize + payload_len);
  const std::uint32_t actual_crc =
      crc32(data, kFrameHeaderSize + static_cast<std::size_t>(payload_len));
  if (expected_crc != actual_crc) {
    return make_error(ErrorCode::kBadCrc, "frame crc mismatch");
  }

  const std::uint8_t major = byte_at(data, 4);
  const std::uint8_t minor = byte_at(data, 5);
  if (major != gen::kIdlMajor) {
    return make_error(ErrorCode::kVersionMismatch,
                      std::string("idl major mismatch: frame ") + std::to_string(major) +
                          ", supported " + std::to_string(gen::kIdlMajor));
  }
  // A higher minor is accepted: additive fields are optional at decode time.
  (void)minor;

  Frame frame;
  frame.idl_major = major;
  frame.idl_minor = minor;
  frame.msg_type = static_cast<MsgType>(byte_at(data, 6));
  frame.flags = byte_at(data, 7);
  frame.service_id = read_u16(data + 8);
  frame.method_id = read_u16(data + 10);
  frame.seq = read_u32(data + 12);
  frame.trace_id = read_u64(data + 16);
  // skip payload_len at 24
  if (payload_len > 0) {
    frame.payload.append_bytes(data + kFrameHeaderSize, payload_len);
  }
  if (consumed != nullptr)
    *consumed = total;
  return frame;
}

void FrameDecoder::feed(const std::uint8_t* data, std::size_t size) {
  if (size == 0)
    return;
  buffer_.insert(buffer_.end(), data, data + size);
}

Result<std::optional<Frame>> FrameDecoder::next() {
  if (buffered() < 4)
    return std::optional<Frame>{};
  const std::uint8_t* base = buffer_.data() + offset_;
  if (std::memcmp(base, kFrameMagic, 4) != 0) {
    return make_error(ErrorCode::kBadMagic, "bad frame magic in stream");
  }
  if (buffered() < kFrameHeaderSize)
    return std::optional<Frame>{};
  const std::uint32_t payload_len = read_u32(base + 24);
  if (payload_len > kMaxFramePayload) {
    return make_error(ErrorCode::kInvalidArgument, "frame payload exceeds limit");
  }
  const std::size_t total =
      kFrameHeaderSize + static_cast<std::size_t>(payload_len) + kFrameCrcSize;
  if (buffered() < total)
    return std::optional<Frame>{};

  std::size_t consumed = 0;
  auto decoded = decode_frame(base, total, &consumed);
  if (!decoded)
    return decoded.error();
  offset_ += consumed;
  if (offset_ > 4096 && offset_ * 2 > buffer_.size()) {
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
  }
  return std::optional<Frame>(std::move(decoded.value()));
}

} // namespace cabin
