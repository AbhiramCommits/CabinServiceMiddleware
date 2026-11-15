#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "cabin/frame.hpp"
#include "cabin_serdes.hpp"
#include "cabin_services.hpp"
#include "cabin_types.hpp"

using namespace cabin;

namespace {

Frame make_frame() {
  Frame frame;
  frame.msg_type = MsgType::kRequest;
  frame.service_id = gen::kHvacServiceId;
  frame.method_id = gen::kHvacServiceMethodSetSetpoint;
  frame.seq = 0x11223344u;
  frame.trace_id = 0x8000000000001234ULL;
  auto payload = gen::encode(gen::HvacSetRequest{22.5F, 3});
  EXPECT_TRUE(payload.ok());
  frame.payload = payload.value();
  return frame;
}

} // namespace

TEST(Frame, RoundTripsHeaderAndPayload) {
  const Frame frame = make_frame();
  auto encoded = encode_frame(frame);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  ASSERT_EQ(encoded.value().size(),
            kFrameHeaderSize + frame.payload.size() + kFrameCrcSize);

  std::size_t consumed = 0;
  auto decoded = decode_frame(encoded.value().data(), encoded.value().size(), &consumed);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(consumed, encoded.value().size());
  EXPECT_EQ(decoded.value().msg_type, MsgType::kRequest);
  EXPECT_EQ(decoded.value().service_id, gen::kHvacServiceId);
  EXPECT_EQ(decoded.value().method_id, gen::kHvacServiceMethodSetSetpoint);
  EXPECT_EQ(decoded.value().seq, 0x11223344u);
  EXPECT_EQ(decoded.value().trace_id, 0x8000000000001234ULL);
  EXPECT_EQ(decoded.value().idl_major, gen::kIdlMajor);
  EXPECT_EQ(decoded.value().idl_minor, gen::kIdlMinor);

  auto request = gen::decode<gen::HvacSetRequest>(decoded.value().payload);
  ASSERT_TRUE(request.ok());
  EXPECT_FLOAT_EQ(request.value().setpoint_c, 22.5F);
  EXPECT_EQ(request.value().fan_level, 3);
}

TEST(Frame, RejectsBadMagicCrcAndTruncation) {
  auto encoded = encode_frame(make_frame());
  ASSERT_TRUE(encoded.ok());
  const auto& bytes = encoded.value();

  {
    std::vector<std::uint8_t> corrupt(bytes.storage());
    corrupt[0] = 'X';
    std::size_t consumed = 0;
    auto decoded = decode_frame(corrupt.data(), corrupt.size(), &consumed);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.code(), ErrorCode::kBadMagic);
  }
  {
    std::vector<std::uint8_t> corrupt(bytes.storage());
    corrupt[corrupt.size() - 5] ^= 0x01; // payload byte, CRC now stale
    std::size_t consumed = 0;
    auto decoded = decode_frame(corrupt.data(), corrupt.size(), &consumed);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.code(), ErrorCode::kBadCrc);
  }
  {
    std::vector<std::uint8_t> corrupt(bytes.storage());
    corrupt[corrupt.size() - 1] ^= 0xFF; // CRC field itself
    auto decoded = decode_frame(corrupt.data(), corrupt.size(), nullptr);
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.code(), ErrorCode::kBadCrc);
  }
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    std::size_t consumed = 12345;
    auto decoded = decode_frame(bytes.data(), length, &consumed);
    EXPECT_FALSE(decoded.ok()) << "unexpected success at length " << length;
    EXPECT_EQ(consumed, 0u);
    if (length < 4) {
      EXPECT_EQ(decoded.code(), ErrorCode::kTruncated);
    } else if (length >= kFrameHeaderSize) {
      EXPECT_EQ(decoded.code(), ErrorCode::kTruncated);
    }
  }
}

TEST(Frame, VersionNegotiation) {
  Frame frame = make_frame();
  frame.idl_major = static_cast<std::uint8_t>(gen::kIdlMajor + 1);
  auto encoded = encode_frame(frame);
  ASSERT_TRUE(encoded.ok());
  auto decoded = decode_frame(encoded.value().data(), encoded.value().size(), nullptr);
  EXPECT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.code(), ErrorCode::kVersionMismatch);
  EXPECT_NE(decoded.error().message.find("major"), std::string::npos);

  Frame newer_minor = make_frame();
  newer_minor.idl_minor = 99;
  auto newer_bytes = encode_frame(newer_minor);
  ASSERT_TRUE(newer_bytes.ok());
  auto accepted =
      decode_frame(newer_bytes.value().data(), newer_bytes.value().size(), nullptr);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().idl_minor, 99);

  Frame older_minor = make_frame();
  older_minor.idl_minor = 0;
  auto older_bytes = encode_frame(older_minor);
  ASSERT_TRUE(older_bytes.ok());
  auto accepted_older =
      decode_frame(older_bytes.value().data(), older_bytes.value().size(), nullptr);
  EXPECT_TRUE(accepted_older.ok());
}

TEST(Frame, StreamDecoderHandlesArbitraryChunking) {
  auto first = encode_frame(make_frame());
  auto second = encode_frame(make_frame());
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  Bytes stream = first.value();
  stream.append_bytes(second.value());

  FrameDecoder decoder;
  std::vector<Frame> frames;
  for (std::size_t i = 0; i < stream.size(); i += 7) {
    const std::size_t chunk = std::min<std::size_t>(7, stream.size() - i);
    decoder.feed(stream.data() + i, chunk);
    auto frame = decoder.next();
    ASSERT_TRUE(frame.ok()) << frame.error().message;
    while (frame.value().has_value()) {
      frames.push_back(std::move(frame.value().value()));
      frame = decoder.next();
      ASSERT_TRUE(frame.ok());
    }
  }
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(decoder.buffered(), 0u);
}

TEST(Frame, TruncatedRandomInputNeverCrashes) {
  std::mt19937 rng(0xCAB1u);
  for (int iteration = 0; iteration < 10000; ++iteration) {
    const std::size_t length = rng() % 96;
    std::vector<std::uint8_t> bytes(length);
    for (auto& byte : bytes)
      byte = static_cast<std::uint8_t>(rng());
    std::size_t consumed = 0;
    auto decoded = decode_frame(bytes.data(), bytes.size(), &consumed);
    if (!decoded.ok()) {
      EXPECT_EQ(consumed, 0u);
    }
    FrameDecoder decoder;
    if (length > 0)
      decoder.feed(bytes.data(), bytes.size());
    auto streamed = decoder.next();
    (void)streamed;
  }
  SUCCEED();
}

TEST(Frame, ZeroLengthPayloadIsValid) {
  Frame frame;
  frame.msg_type = MsgType::kPing;
  auto encoded = encode_frame(frame);
  ASSERT_TRUE(encoded.ok());
  auto decoded = decode_frame(encoded.value().data(), encoded.value().size(), nullptr);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().payload.size(), 0u);
  EXPECT_STREQ(msg_type_name(decoded.value().msg_type), "PING");
}
