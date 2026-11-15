#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cabin/json.hpp"
#include "cabin/personalization/personalization_service.hpp"
#include "cabin/personalization/profile_store.hpp"
#include "cabin_serdes.hpp"
#include "cabin_services.hpp"

using namespace cabin;

namespace {

std::string make_state_dir() {
  char name[] = "/tmp/cabin-profile-test-XXXXXX";
  char* directory = ::mkdtemp(name);
  EXPECT_NE(directory, nullptr);
  return directory != nullptr ? std::string(directory)
                              : std::string("/tmp/cabin-profile");
}

gen::Profile valid_profile(std::string driver = "driver-a") {
  gen::Profile profile;
  profile.driver_id = std::move(driver);
  profile.seat_position_mm = 420;
  profile.seat_recline_deg = 18;
  profile.hvac_setpoint_c = 21.5F;
  profile.hvac_fan_level = 3;
  profile.light_level = 65;
  profile.light_color = gen::LightColor::kWARM;
  profile.schema_version = 1;
  profile.preferred_language = "en";
  return profile;
}

class FakeChannel : public gen::IRpcChannel {
 public:
  std::vector<std::uint16_t> calls;

  Result<Bytes> call(std::uint16_t service_id, std::uint16_t method_id,
                     const Bytes& request, int timeout_ms) override {
    (void)timeout_ms;
    calls.push_back(service_id);
    if (service_id == gen::kSeatServiceId &&
        method_id == gen::kSeatServiceMethodSetPosition) {
      auto decoded = gen::decode<gen::SeatSetPositionRequest>(request);
      if (!decoded)
        return decoded.error();
      gen::SeatSetPositionResponse response;
      response.accepted = true;
      response.position_mm = decoded.value().position_mm;
      response.recline_deg = decoded.value().recline_deg;
      return gen::encode(response);
    }
    if (service_id == gen::kHvacServiceId &&
        method_id == gen::kHvacServiceMethodSetSetpoint) {
      auto decoded = gen::decode<gen::HvacSetRequest>(request);
      if (!decoded)
        return decoded.error();
      gen::HvacSetResponse response;
      response.accepted = true;
      response.setpoint_c = decoded.value().setpoint_c;
      response.fan_level = decoded.value().fan_level;
      return gen::encode(response);
    }
    if (service_id == gen::kLightingServiceId &&
        method_id == gen::kLightingServiceMethodSetLight) {
      auto decoded = gen::decode<gen::LightSetRequest>(request);
      if (!decoded)
        return decoded.error();
      gen::LightSetResponse response;
      response.accepted = true;
      response.level = decoded.value().level;
      response.color = decoded.value().color;
      return gen::encode(response);
    }
    return make_error(ErrorCode::kNotFound, "fake channel: unknown method");
  }
};

} // namespace

TEST(Json, ParsesAndDumpsDocuments) {
  const std::string text =
      "{\"name\":\"driver-a\",\"ok\":true,\"count\":3,\"temp\":21.5,"
      "\"values\":[1,2,3],\"nested\":{\"x\":null},\"escaped\":\"a\\\"b\\n\"}";
  auto parsed = parse_json(text);
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_EQ(parsed.value().get_string("name"), "driver-a");
  EXPECT_TRUE(parsed.value().get_bool("ok"));
  EXPECT_EQ(parsed.value().get_int("count"), 3);
  EXPECT_DOUBLE_EQ(parsed.value().get_number("temp"), 21.5);
  ASSERT_NE(parsed.value().find("values"), nullptr);
  ASSERT_TRUE(parsed.value().find("values")->is_array());
  EXPECT_EQ(parsed.value().find("values")->size(), 3u);
  EXPECT_EQ(parsed.value().get_string("escaped"), "a\"b\n");

  const std::string dumped = parsed.value().dump(2);
  auto reparsed = parse_json(dumped);
  ASSERT_TRUE(reparsed.ok()) << reparsed.error().message;
  EXPECT_EQ(reparsed.value().get_string("name"), "driver-a");
  EXPECT_EQ(reparsed.value().find("values")->size(), 3u);

  EXPECT_FALSE(parse_json("{").ok());
  EXPECT_FALSE(parse_json("{\"a\":}").ok());
  EXPECT_FALSE(parse_json("{} trailing").ok());
  EXPECT_FALSE(parse_json("[1,]").ok());
}

TEST(Personalization, ProfileStorePersistsAcrossRestart) {
  const std::string dir = make_state_dir();
  {
    ProfileStore store(dir);
    ASSERT_TRUE(store.load().ok());
    ASSERT_EQ(store.size(), 0u);
    ASSERT_TRUE(store.put(valid_profile()).ok());
    ASSERT_TRUE(store.set_active_driver("driver-a").ok());
    ASSERT_TRUE(store.save().ok());
    EXPECT_TRUE(std::filesystem::exists(store.path()));
  }
  {
    ProfileStore reopened(dir);
    ASSERT_TRUE(reopened.load().ok());
    auto profile = reopened.get("driver-a");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->seat_position_mm, 420);
    EXPECT_EQ(profile->light_color, gen::LightColor::kWARM);
    EXPECT_EQ(profile->preferred_language, "en");
    EXPECT_EQ(reopened.active_driver(), "driver-a");
  }

  // Corrupt file is reported, never silently overwritten with garbage.
  std::ofstream(dir + "/profiles.json") << "{not json";
  ProfileStore broken(dir);
  EXPECT_FALSE(broken.load().ok());
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

TEST(Personalization, LoadsV1PayloadWithAdditiveV12FieldAndUnknownFields) {
  const std::string dir = make_state_dir();
  std::ofstream out(dir + "/profiles.json");
  out << R"({
    "schema_version": 1,
    "active_driver": "driver-old",
    "profiles": {
      "driver-old": {
        "driver_id": "driver-old",
        "seat_position_mm": 300,
        "seat_recline_deg": 10,
        "hvac_setpoint_c": 20.0,
        "hvac_fan_level": 2,
        "light_level": 40,
        "light_color": "COOL",
        "schema_version": 1,
        "future_field_from_v13": {"ignored": true}
      }
    }
  })";
  out.close();

  ProfileStore store(dir);
  ASSERT_TRUE(store.load().ok());
  auto profile = store.get("driver-old");
  ASSERT_TRUE(profile.has_value());
  EXPECT_EQ(profile->preferred_language, "en")
      << "v1.2 additive field falls back to its default";
  EXPECT_EQ(profile->light_color, gen::LightColor::kCOOL);
  EXPECT_EQ(profile->seat_position_mm, 300);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

TEST(Personalization, PutProfileRejectsOutOfRange) {
  const std::string dir = make_state_dir();
  ProfileStore store(dir);
  ASSERT_TRUE(store.load().ok());
  FakeChannel channel;
  auto service = std::make_unique<PersonalizationService>(
      store, channel, ServiceEventPublisher{}, [] { return std::uint64_t{1}; });

  gen::Profile invalid = valid_profile();
  invalid.seat_position_mm = 900;
  auto result = service->put_profile(gen::PutProfileRequest{invalid});
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.code(), ErrorCode::kOutOfRange);
  EXPECT_EQ(store.size(), 0u);

  gen::Profile invalid_color = valid_profile();
  invalid_color.light_color = static_cast<gen::LightColor>(9);
  auto color_result = service->put_profile(gen::PutProfileRequest{invalid_color});
  EXPECT_FALSE(color_result.ok());
  EXPECT_EQ(color_result.code(), ErrorCode::kOutOfRange);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

TEST(Personalization, ApplyProfileCallsSeatThenHvacThenLighting) {
  const std::string dir = make_state_dir();
  ProfileStore store(dir);
  ASSERT_TRUE(store.load().ok());
  ASSERT_TRUE(store.put(valid_profile()).ok());
  ASSERT_TRUE(store.save().ok());

  FakeChannel channel;
  std::uint64_t clock = 1000;
  int published = 0;
  gen::ProfileAppliedEvent last_event;
  ServiceEventPublisher publisher = [&](std::uint16_t event_id, const Bytes& payload) {
    EXPECT_EQ(event_id, gen::kPersonalizationServiceEventProfileApplied);
    auto decoded = gen::decode<gen::ProfileAppliedEvent>(payload);
    if (!decoded)
      return;
    last_event = decoded.value();
    ++published;
  };
  PersonalizationService service(store, channel, publisher,
                                 [&clock] { return clock += 1000; });

  auto result = service.apply_profile(gen::ApplyProfileRequest{"driver-a"});
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(result.value().applied);
  EXPECT_GT(result.value().latency_us, 0u);
  ASSERT_EQ(channel.calls.size(), 3u);
  EXPECT_EQ(channel.calls[0], gen::kSeatServiceId);
  EXPECT_EQ(channel.calls[1], gen::kHvacServiceId);
  EXPECT_EQ(channel.calls[2], gen::kLightingServiceId);
  EXPECT_EQ(published, 1);
  EXPECT_EQ(last_event.driver_id, "driver-a");
  EXPECT_EQ(last_event.latency_us, result.value().latency_us);
  EXPECT_EQ(store.active_driver(), "driver-a");
  EXPECT_EQ(service.last_apply_latency_us(), result.value().latency_us);

  auto missing = service.apply_profile(gen::ApplyProfileRequest{"nobody"});
  EXPECT_FALSE(missing.ok());
  EXPECT_EQ(missing.code(), ErrorCode::kNotFound);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

TEST(Personalization, IgnitionOnAppliesOncePerTransition) {
  const std::string dir = make_state_dir();
  ProfileStore store(dir);
  ASSERT_TRUE(store.load().ok());
  ASSERT_TRUE(store.put(valid_profile()).ok());
  ASSERT_TRUE(store.set_active_driver("driver-a").ok());
  ASSERT_TRUE(store.save().ok());

  FakeChannel channel;
  int published = 0;
  PersonalizationService service(
      store, channel, [&](std::uint16_t, const Bytes&) { ++published; },
      [] { return std::uint64_t{5000}; });

  ASSERT_TRUE(service.on_ignition_state(gen::IgnitionState::kACC).ok());
  EXPECT_EQ(channel.calls.size(), 0u);
  ASSERT_TRUE(service.on_ignition_state(gen::IgnitionState::kON).ok());
  EXPECT_EQ(channel.calls.size(), 3u);
  ASSERT_TRUE(service.on_ignition_state(gen::IgnitionState::kON).ok());
  EXPECT_EQ(channel.calls.size(), 3u) << "repeated ON snapshots must not re-apply";
  ASSERT_TRUE(service.on_ignition_state(gen::IgnitionState::kOFF).ok());
  ASSERT_TRUE(service.on_ignition_state(gen::IgnitionState::kON).ok());
  EXPECT_EQ(channel.calls.size(), 6u);
  EXPECT_EQ(published, 2);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}
