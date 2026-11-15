#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "cabin/bytes.hpp"
#include "cabin/result.hpp"
#include "cabin_serdes.hpp"
#include "cabin_types.hpp"

using namespace cabin;
using namespace cabin::gen;

namespace {

template <typename T>
void expect_valid(const T& value) {
  auto status = validate(value);
  EXPECT_TRUE(status.ok()) << status.error().message;
  auto encoded = encode(value);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  auto decoded = decode<T>(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_TRUE(decoded.value() == value);
}

template <typename T>
void expect_rejected(const T& value) {
  auto status = validate(value);
  EXPECT_FALSE(status.ok()) << "expected validation rejection";
  if (!status.ok()) {
    EXPECT_EQ(status.code(), ErrorCode::kOutOfRange);
  }
  auto encoded = encode(value);
  EXPECT_FALSE(encoded.ok());
}

Profile valid_profile() {
  Profile profile;
  profile.driver_id = "driver-a";
  profile.seat_position_mm = 420;
  profile.seat_recline_deg = 18;
  profile.hvac_setpoint_c = 21.5F;
  profile.hvac_fan_level = 3;
  profile.light_level = 65;
  profile.light_color = LightColor::kWARM;
  profile.schema_version = 1;
  profile.preferred_language = "en";
  return profile;
}

} // namespace

TEST(Serdes, RoundTripsEveryGeneratedStruct) {
  expect_valid(Empty{});
  expect_valid(ApplyProfileRequest{"driver-a"});
  expect_valid(ApplyProfileResponse{true, 1234});
  expect_valid(GetProfileRequest{"driver-a"});
  GetProfileResponse get_profile;
  get_profile.found = true;
  get_profile.profile = valid_profile();
  expect_valid(get_profile);
  GetProfileResponse missing;
  expect_valid(missing);
  expect_valid(ProfileAppliedEvent{"driver-a", 90});
  expect_valid(PutProfileRequest{valid_profile()});
  expect_valid(PutProfileResponse{true, 1});

  expect_valid(SeatSetPositionRequest{400, 20});
  expect_valid(SeatSetPositionResponse{true, 400, 20});
  expect_valid(SeatState{400, 20, 123456});
  expect_valid(HvacSetRequest{22.0F, 4});
  expect_valid(HvacSetResponse{true, 22.0F, 4});
  expect_valid(HvacState{22.0F, 4, 999});
  expect_valid(LightSetRequest{70, LightColor::kNEUTRAL});
  expect_valid(LightSetResponse{true, 70, LightColor::kNEUTRAL});
  expect_valid(LightState{70, LightColor::kNEUTRAL, 77});

  SignalSnapshot snapshot;
  snapshot.seat_position_mm = 640;
  snapshot.seat_recline_deg = 30;
  snapshot.hvac_setpoint_c = 19.0F;
  snapshot.hvac_fan_level = 2;
  snapshot.cabin_light_level = 40;
  snapshot.cabin_light_color = LightColor::kCOOL;
  snapshot.door_state = DoorState::kAJAR;
  snapshot.ignition_state = IgnitionState::kON;
  snapshot.timestamp_us = 1234567;
  expect_valid(snapshot);

  expect_valid(SignalSetRequest{"hvac_setpoint_c", 23.5});
  expect_valid(SignalSetResponse{true, 23.5, 1234});
}

TEST(Serdes, RejectsEveryNumericRangeViolation) {
  expect_rejected(SeatSetPositionRequest{801, 20});
  expect_rejected(SeatSetPositionRequest{400, 61});
  expect_rejected(SeatState{801, 0, 0});
  expect_rejected(SeatState{0, 61, 0});
  expect_rejected(HvacSetRequest{15.5F, 4});
  expect_rejected(HvacSetRequest{30.5F, 4});
  expect_rejected(HvacSetRequest{22.0F, 8});
  expect_rejected(HvacState{31.0F, 4, 0});
  expect_rejected(LightSetRequest{101, LightColor::kWARM});
  expect_rejected(LightState{101, LightColor::kWARM, 0});

  Profile profile = valid_profile();
  profile.seat_position_mm = 801;
  expect_rejected(profile);
  profile = valid_profile();
  profile.seat_recline_deg = 61;
  expect_rejected(profile);
  profile = valid_profile();
  profile.hvac_setpoint_c = 15.0F;
  expect_rejected(profile);
  profile = valid_profile();
  profile.hvac_fan_level = 8;
  expect_rejected(profile);
  profile = valid_profile();
  profile.light_level = 101;
  expect_rejected(profile);
  profile = valid_profile();
  profile.schema_version = 0;
  expect_rejected(profile);
  profile = valid_profile();
  profile.schema_version = 2;
  expect_rejected(profile);
}

TEST(Serdes, RejectsBoundaryAndFloatingPointPathology) {
  expect_valid(SeatSetPositionRequest{0, 0});
  expect_valid(SeatSetPositionRequest{800, 60});
  expect_valid(HvacSetRequest{16.0F, 0});
  expect_valid(HvacSetRequest{30.0F, 7});

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  expect_rejected(HvacSetRequest{nan, 4});
  expect_rejected(HvacSetRequest{inf, 4});
  expect_rejected(HvacSetRequest{-inf, 4});
  Profile profile = valid_profile();
  profile.hvac_setpoint_c = nan;
  expect_rejected(profile);
}

TEST(Serdes, RejectsInvalidEnumsAndOversizedStrings) {
  LightSetRequest bad_color;
  bad_color.color = static_cast<LightColor>(3);
  expect_rejected(bad_color);
  SignalSnapshot snapshot;
  snapshot.door_state = static_cast<DoorState>(9);
  expect_rejected(snapshot);

  ApplyProfileRequest long_driver{std::string(65, 'x')};
  expect_rejected(long_driver);
  Profile long_nickname = valid_profile();
  long_nickname.preferred_language = std::string(17, 'y');
  expect_rejected(long_nickname);

  // Nested validation propagates: an invalid inner profile rejects the outer.
  PutProfileRequest nested;
  nested.profile = valid_profile();
  nested.profile.hvac_fan_level = 9;
  expect_rejected(nested);
}

TEST(Serdes, DecodeRejectsOutOfRangeWirePayloads) {
  SeatSetPositionRequest request{400, 20};
  auto encoded = encode(request);
  ASSERT_TRUE(encoded.ok());
  Bytes bytes = encoded.value();
  bytes.rewind();
  // Overwrite position_mm (first two bytes are the string-free u16) with 900.
  bytes.mutable_data()[0] = 0x84;
  bytes.mutable_data()[1] = 0x03;
  auto decoded = decode<SeatSetPositionRequest>(bytes);
  EXPECT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.code(), ErrorCode::kOutOfRange);
}

TEST(Serdes, DecodeRejectsTruncatedFields) {
  auto encoded = encode(SeatSetPositionRequest{400, 20});
  ASSERT_TRUE(encoded.ok());
  Bytes short_bytes;
  short_bytes.append_u8(encoded.value().data()[0]);
  auto decoded = decode<SeatSetPositionRequest>(short_bytes);
  EXPECT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.code(), ErrorCode::kTruncated);
}

TEST(Serdes, AdditiveV12FieldDefaultsWhenAbsentAndTrailingIsIgnored) {
  // v1.2 encoder writes preferred_language. A v1.0 decoder would read the
  // known fields and ignore trailing bytes, which the length-free top level
  // and length-delimited nested structs both allow.
  auto encoded = encode(valid_profile());
  ASSERT_TRUE(encoded.ok());
  const auto& bytes = encoded.value();

  // Append a hypothetical v1.3 field. v1.2 must still decode successfully.
  Bytes future = bytes;
  future.append_u32(0xAABBCCDDu);
  auto decoded = decode<Profile>(future);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value().preferred_language, "en");

  // Simulate a v1.0 producer that omitted preferred_language. Its payload is
  // the v1.2 encoding minus the trailing length-prefixed string.
  Bytes old_wire;
  old_wire.append_bytes(bytes.data(), bytes.size() - (2 + 2)); // drop "en"
  auto old_decoded = decode<Profile>(old_wire);
  ASSERT_TRUE(old_decoded.ok()) << old_decoded.error().message;
  EXPECT_EQ(old_decoded.value().preferred_language, "en");
  EXPECT_EQ(old_decoded.value().driver_id, "driver-a");
}
