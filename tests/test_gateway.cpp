#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cabin/broker.hpp"
#include "cabin/client.hpp"
#include "cabin/gateway/gateway.hpp"
#include "cabin/gateway/http_server.hpp"
#include "cabin/json.hpp"
#include "cabin/log.hpp"
#include "cabin/personalization/personalization_service.hpp"
#include "cabin/personalization/profile_store.hpp"
#include "cabin/runtime.hpp"
#include "cabin/signals/cache.hpp"
#include "cabin/signals/can.hpp"
#include "cabin/signals/services.hpp"
#include "cabin/signals/signal_service.hpp"
#include "cabin_serdes.hpp"
#include "http_client.hpp"

using namespace cabin;
using namespace cabin::signals;
using namespace cabin::gateway;

namespace {

std::string make_dir(const char* prefix) {
  std::string name = std::string("/tmp/") + prefix + "-XXXXXX";
  std::vector<char> buffer(name.begin(), name.end());
  buffer.push_back('\0');
  char* directory = ::mkdtemp(buffer.data());
  EXPECT_NE(directory, nullptr);
  return directory != nullptr ? std::string(directory)
                              : std::string("/tmp/cabin-gateway");
}

std::string capture_path(const std::string& name) {
  return std::string(CABIN_SOURCE_DIR) + "/captures/" + name;
}

void replay_into(SignalProcessor& processor, const std::string& file) {
  ReplayCanSource source(file);
  EXPECT_TRUE(source.open().ok());
  while (true) {
    auto frame = source.next(0);
    EXPECT_TRUE(frame.ok());
    if (!frame.value().has_value())
      break;
    processor.process(frame.value().value(), frame.value().value().timestamp_us);
  }
}

std::shared_ptr<ServiceClient> connect_client(const std::string& dir) {
  ServiceClientOptions options;
  options.runtime_dir = dir;
  auto client = ServiceClient::connect(options);
  EXPECT_TRUE(client.ok()) << (client.ok() ? "" : client.error().message);
  return client.ok() ? client.value() : nullptr;
}

struct FullStack {
  std::string runtime_dir;
  std::string state_dir;
  std::unique_ptr<Broker> broker;
  SignalProcessor processor;
  std::shared_ptr<ServiceClient> signal_client;
  std::shared_ptr<ServiceClient> service_client;
  std::shared_ptr<ServiceClient> personalization_client;
  std::shared_ptr<ServiceClient> gateway_client;
  std::shared_ptr<SignalServiceImpl> signal_impl;
  std::unique_ptr<RemoteSignalLayer> layer;
  std::shared_ptr<SeatServiceImpl> seat;
  std::shared_ptr<HvacServiceImpl> hvac;
  std::shared_ptr<LightingServiceImpl> lighting;
  std::vector<std::unique_ptr<ServiceHost>> hosts;
  std::unique_ptr<ProfileStore> store;
  std::shared_ptr<PersonalizationService> personalization;
  std::unique_ptr<gateway::RpcGatewayBackend> backend;
  std::unique_ptr<gateway::GatewayApp> app;
  std::unique_ptr<gateway::HttpServer> server;

  FullStack() {
    runtime_dir = make_dir("cabin-gateway-test");
    state_dir = make_dir("cabin-gateway-state");
    BrokerConfig config;
    config.runtime_dir = runtime_dir;
    broker = std::make_unique<Broker>(config);
    EXPECT_TRUE(broker->start().ok());
    broker->run_async();
    replay_into(processor, capture_path("cabin_normal.log"));

    signal_client = connect_client(runtime_dir);
    service_client = connect_client(runtime_dir);
    personalization_client = connect_client(runtime_dir);
    gateway_client = connect_client(runtime_dir);
    if (!signal_client || !service_client || !personalization_client || !gateway_client) {
      return;
    }

    signal_impl = std::make_shared<SignalServiceImpl>(processor);
    hosts.push_back(std::make_unique<ServiceHost>(signal_client, signal_impl));
    EXPECT_TRUE(hosts.back()->start().ok());

    layer = std::make_unique<RemoteSignalLayer>(service_client);
    const auto clock = [this] { return processor.now_us(); };
    seat = std::make_shared<SeatServiceImpl>(
        *layer,
        [this](std::uint16_t event, const Bytes& payload) {
          const auto status =
              service_client->publish(gen::kSeatServiceId, event, payload);
          (void)status;
        },
        clock);
    hvac = std::make_shared<HvacServiceImpl>(
        *layer,
        [this](std::uint16_t event, const Bytes& payload) {
          const auto status =
              service_client->publish(gen::kHvacServiceId, event, payload);
          (void)status;
        },
        clock);
    lighting = std::make_shared<LightingServiceImpl>(
        *layer,
        [this](std::uint16_t event, const Bytes& payload) {
          const auto status =
              service_client->publish(gen::kLightingServiceId, event, payload);
          (void)status;
        },
        clock);
    hosts.push_back(std::make_unique<ServiceHost>(service_client, seat));
    hosts.push_back(std::make_unique<ServiceHost>(service_client, hvac));
    hosts.push_back(std::make_unique<ServiceHost>(service_client, lighting));
    for (auto& host : hosts) {
      if (!host->start().ok())
        ADD_FAILURE() << "host failed to start";
    }

    store = std::make_unique<ProfileStore>(state_dir);
    EXPECT_TRUE(store->load().ok());
    personalization = std::make_shared<PersonalizationService>(
        *store, *personalization_client,
        [this](std::uint16_t event, const Bytes& payload) {
          const auto status = personalization_client->publish(
              gen::kPersonalizationServiceId, event, payload);
          (void)status;
        },
        [] { return cabin::now_us(); });
    hosts.push_back(
        std::make_unique<ServiceHost>(personalization_client, personalization));
    EXPECT_TRUE(hosts.back()->start().ok());

    backend = std::make_unique<gateway::RpcGatewayBackend>(gateway_client);
    app = std::make_unique<gateway::GatewayApp>(*backend);
    server = std::make_unique<gateway::HttpServer>(
        "127.0.0.1", 0,
        [this](const gateway::HttpRequest& request) { return app->handle(request); });
    EXPECT_TRUE(server->start().ok());
  }

  ~FullStack() {
    if (server)
      server->stop();
    server.reset();
    app.reset();
    backend.reset();
    personalization.reset();
    store.reset();
    hosts.clear();
    lighting.reset();
    hvac.reset();
    seat.reset();
    layer.reset();
    signal_impl.reset();
    gateway_client.reset();
    personalization_client.reset();
    service_client.reset();
    signal_client.reset();
    if (broker) {
      broker->stop();
      broker.reset();
    }
    std::error_code error;
    std::filesystem::remove_all(runtime_dir, error);
    std::filesystem::remove_all(state_dir, error);
  }

  std::uint16_t port() const { return server->bound_port(); }
};

JsonValue body_json(const test::HttpResponseData& response) {
  auto parsed = parse_json(response.body);
  EXPECT_TRUE(parsed.ok()) << parsed.error().message;
  return parsed.ok() ? parsed.value() : JsonValue::object();
}

} // namespace

TEST(Gateway, AllFiveEndpointsOverRealHttp) {
  FullStack stack;
  EXPECT_NE(stack.port(), 0);

  auto health = test::http_request(stack.port(), "GET", "/healthz");
  EXPECT_EQ(health.status, 200);
  EXPECT_EQ(body_json(health).get_string("status"), "ok");
  EXPECT_FALSE(health.headers["x-cabin-trace-id"].empty());

  auto signals = test::http_request(stack.port(), "GET", "/v1/signals");
  EXPECT_EQ(signals.status, 200);
  JsonValue snapshot = body_json(signals);
  EXPECT_EQ(snapshot.get_int("seat_position_mm"), 420);
  EXPECT_EQ(snapshot.get_string("ignition_state"), "ON");
  EXPECT_EQ(snapshot.get_string("cabin_light_color"), "WARM");

  const std::string profile_body = R"({
    "seat_position_mm": 500,
    "seat_recline_deg": 25,
    "hvac_setpoint_c": 19.0,
    "hvac_fan_level": 2,
    "light_level": 30,
    "light_color": "NEUTRAL"
  })";
  auto put =
      test::http_request(stack.port(), "PUT", "/v1/profiles/driver-a", profile_body);
  EXPECT_EQ(put.status, 200) << put.body;
  EXPECT_TRUE(body_json(put).get_bool("accepted"));

  auto get = test::http_request(stack.port(), "GET", "/v1/profiles/driver-a");
  EXPECT_EQ(get.status, 200) << get.body;
  JsonValue profile = body_json(get);
  EXPECT_EQ(profile.get_string("driver_id"), "driver-a");
  EXPECT_EQ(profile.get_int("seat_position_mm"), 500);
  EXPECT_EQ(profile.get_string("light_color"), "NEUTRAL");
  EXPECT_EQ(profile.get_string("preferred_language"), "en");

  auto apply = test::http_request(stack.port(), "POST", "/v1/profiles/driver-a/apply");
  EXPECT_EQ(apply.status, 200) << apply.body;
  JsonValue applied = body_json(apply);
  EXPECT_TRUE(applied.get_bool("applied"));
  EXPECT_GE(applied.get_int("latency_us"), 0);

  auto after = test::http_request(stack.port(), "GET", "/v1/signals");
  EXPECT_EQ(after.status, 200);
  JsonValue updated = body_json(after);
  EXPECT_EQ(updated.get_int("seat_position_mm"), 500);
  EXPECT_EQ(updated.get_int("seat_recline_deg"), 25);
  EXPECT_DOUBLE_EQ(updated.get_number("hvac_setpoint_c"), 19.0);
  EXPECT_EQ(updated.get_int("cabin_light_level"), 30);

  auto missing = test::http_request(stack.port(), "GET", "/v1/profiles/nobody");
  EXPECT_EQ(missing.status, 404);
}

TEST(Gateway, HandlesMalformedBodiesAndHttpErrors) {
  FullStack stack;

  auto malformed = test::http_request(stack.port(), "PUT", "/v1/profiles/driver-a", "{");
  EXPECT_EQ(malformed.status, 400);

  auto out_of_range = test::http_request(
      stack.port(), "PUT", "/v1/profiles/driver-a",
      R"({"seat_position_mm":900,"seat_recline_deg":10,"hvac_setpoint_c":20.0,
          "hvac_fan_level":2,"light_level":40,"light_color":"WARM"})");
  EXPECT_EQ(out_of_range.status, 400);
  EXPECT_EQ(body_json(out_of_range).find("error")->get_string("code"), "OUT_OF_RANGE");

  auto wrong_method = test::http_request(stack.port(), "DELETE", "/healthz");
  EXPECT_EQ(wrong_method.status, 405);
  EXPECT_EQ(wrong_method.headers["allow"], "GET");

  auto post_signals = test::http_request(stack.port(), "POST", "/v1/signals");
  EXPECT_EQ(post_signals.status, 405);

  auto unknown = test::http_request(stack.port(), "GET", "/v1/unknown");
  EXPECT_EQ(unknown.status, 404);

  auto raw = test::http_raw(stack.port(), "THIS IS NOT HTTP\r\n\r\n");
  EXPECT_EQ(raw.status, 400);
  EXPECT_FALSE(raw.body.empty());

  const std::string huge(70000, 'x');
  auto oversized = test::http_request(stack.port(), "PUT", "/v1/profiles/driver-a", huge);
  EXPECT_EQ(oversized.status, 413);
}

TEST(Gateway, TraceHeaderCorrelatesHttpWithServiceLogs) {
  FullStack stack;
  std::string logs;
  Logger::instance().reset_for_testing();
  Logger::instance().set_sink_for_testing(&logs);
  Logger::instance().set_level(Level::kInfo);

  const std::string trace = "80000000000000aa";
  auto response = test::http_request(
      stack.port(), "PUT", "/v1/profiles/driver-a",
      R"({"seat_position_mm":410,"seat_recline_deg":15,"hvac_setpoint_c":21.0,
          "hvac_fan_level":3,"light_level":50,"light_color":"COOL"})",
      {{"X-Cabin-Trace-Id", trace}});
  EXPECT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(response.headers["x-cabin-trace-id"], trace);
  EXPECT_NE(logs.find("\"trace_id\":\"" + trace + "\""), std::string::npos);
  EXPECT_NE(logs.find("\"service\":\"gateway\""), std::string::npos);
  EXPECT_NE(logs.find("\"event\":\"http_request\""), std::string::npos);

  const std::string apply_trace = "80000000000000bb";
  auto applied = test::http_request(stack.port(), "POST", "/v1/profiles/driver-a/apply",
                                    "", {{"X-Cabin-Trace-Id", apply_trace}});
  EXPECT_EQ(applied.status, 200) << applied.body;
  EXPECT_EQ(applied.headers["x-cabin-trace-id"], apply_trace);
  EXPECT_NE(logs.find("\"trace_id\":\"" + apply_trace + "\""), std::string::npos);
  EXPECT_NE(logs.find("\"service\":\"personalization\""), std::string::npos);
  EXPECT_NE(logs.find("\"event\":\"profile_applied\""), std::string::npos);

  Logger::instance().reset_for_testing();
}

TEST(Gateway, IgnitionOnAppliesActiveProfileThroughRealStack) {
  FullStack stack;
  auto first = test::http_request(
      stack.port(), "PUT", "/v1/profiles/driver-a",
      R"({"seat_position_mm":500,"seat_recline_deg":25,"hvac_setpoint_c":19.0,
          "hvac_fan_level":2,"light_level":30,"light_color":"NEUTRAL"})");
  EXPECT_EQ(first.status, 200) << first.body;
  auto applied = test::http_request(stack.port(), "POST", "/v1/profiles/driver-a/apply");
  EXPECT_EQ(applied.status, 200) << applied.body;

  // Store a second profile without applying it; an ignition cycle must apply it.
  auto second = test::http_request(
      stack.port(), "PUT", "/v1/profiles/driver-a",
      R"({"seat_position_mm":610,"seat_recline_deg":31,"hvac_setpoint_c":23.5,
          "hvac_fan_level":6,"light_level":75,"light_color":"COOL"})");
  EXPECT_EQ(second.status, 200) << second.body;

  EXPECT_TRUE(stack.personalization->on_ignition_state(gen::IgnitionState::kOFF).ok());
  EXPECT_TRUE(stack.personalization->on_ignition_state(gen::IgnitionState::kON).ok());

  auto signals = test::http_request(stack.port(), "GET", "/v1/signals");
  EXPECT_EQ(signals.status, 200);
  JsonValue snapshot = body_json(signals);
  EXPECT_EQ(snapshot.get_int("seat_position_mm"), 610);
  EXPECT_EQ(snapshot.get_int("seat_recline_deg"), 31);
  EXPECT_DOUBLE_EQ(snapshot.get_number("hvac_setpoint_c"), 23.5);
  EXPECT_EQ(snapshot.get_int("hvac_fan_level"), 6);
  EXPECT_EQ(snapshot.get_int("cabin_light_level"), 75);
  EXPECT_EQ(snapshot.get_string("cabin_light_color"), "COOL");
  EXPECT_GT(stack.personalization->last_apply_latency_us(), 0u);
}

TEST(Gateway, ProfilePersistenceSurvivesStoreRestart) {
  const std::string dir = make_dir("cabin-gateway-persist");
  {
    ProfileStore store(dir);
    EXPECT_TRUE(store.load().ok());
    gen::Profile profile;
    profile.driver_id = "driver-p";
    profile.seat_position_mm = 350;
    profile.seat_recline_deg = 12;
    profile.hvac_setpoint_c = 20.5F;
    profile.hvac_fan_level = 1;
    profile.light_level = 20;
    profile.light_color = gen::LightColor::kCOOL;
    profile.schema_version = 1;
    profile.preferred_language = "de";
    EXPECT_TRUE(store.put(profile).ok());
    EXPECT_TRUE(store.set_active_driver("driver-p").ok());
    EXPECT_TRUE(store.save().ok());
  }
  ProfileStore reopened(dir);
  EXPECT_TRUE(reopened.load().ok());
  auto profile = reopened.get("driver-p");
  EXPECT_TRUE(profile.has_value());
  EXPECT_EQ(profile->preferred_language, "de");
  EXPECT_EQ(profile->hvac_setpoint_c, 20.5F);
  EXPECT_EQ(reopened.active_driver(), "driver-p");
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}
