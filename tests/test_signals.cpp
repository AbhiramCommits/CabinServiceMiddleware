#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cabin/broker.hpp"
#include "cabin/client.hpp"
#include "cabin/log.hpp"
#include "cabin/runtime.hpp"
#include "cabin/signals/cache.hpp"
#include "cabin/signals/can.hpp"
#include "cabin/signals/codec.hpp"
#include "cabin/signals/services.hpp"
#include "cabin/signals/signal_service.hpp"

using namespace cabin;
using namespace cabin::signals;

namespace {

std::string source_path(const std::string& name) {
  return std::string(CABIN_SOURCE_DIR) + "/captures/" + name;
}

std::string make_runtime_dir() {
  char name[] = "/tmp/cabin-signal-test-XXXXXX";
  char* directory = ::mkdtemp(name);
  EXPECT_NE(directory, nullptr);
  return directory != nullptr ? std::string(directory) : std::string("/tmp/cabin-signal");
}

std::shared_ptr<ServiceClient> connect_client(const std::string& dir) {
  ServiceClientOptions options;
  options.runtime_dir = dir;
  auto client = ServiceClient::connect(options);
  EXPECT_TRUE(client.ok()) << (client.ok() ? "" : client.error().message);
  return client.ok() ? client.value() : nullptr;
}

struct Stack {
  std::string dir;
  std::unique_ptr<Broker> broker;
  Stack() : dir(make_runtime_dir()) {
    BrokerConfig config;
    config.runtime_dir = dir;
    broker = std::make_unique<Broker>(config);
    EXPECT_TRUE(broker->start().ok());
    broker->run_async();
  }
  ~Stack() {
    if (broker) {
      broker->stop();
      broker.reset();
    }
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  }
};

void replay_into(SignalProcessor& processor, const std::string& file) {
  ReplayCanSource source(file);
  ASSERT_TRUE(source.open().ok());
  while (true) {
    auto frame = source.next(0);
    ASSERT_TRUE(frame.ok()) << (frame.ok() ? "" : frame.error().message);
    if (!frame.value().has_value())
      break;
    processor.process(frame.value().value(), frame.value().value().timestamp_us);
  }
}

double cached(SignalProcessor& processor, SignalId id, double fallback) {
  auto entry = processor.cache().get(id);
  return entry.has_value() ? entry->value : fallback;
}

} // namespace

TEST(Signals, ParsesCandumpLines) {
  CanFrame frame;
  ASSERT_TRUE(parse_candump_line("(1700000000.123456) vcan0 120#a40112", &frame).ok());
  EXPECT_EQ(frame.id, 0x120u);
  EXPECT_EQ(frame.dlc, 3);
  EXPECT_EQ(frame.data[0], 0xA4);
  EXPECT_EQ(frame.data[1], 0x01);
  EXPECT_EQ(frame.data[2], 0x12);
  EXPECT_EQ(frame.timestamp_us, 1700000000123456ULL);

  ASSERT_TRUE(parse_candump_line("vcan0 160#02", &frame).ok());
  EXPECT_EQ(frame.id, 0x160u);
  EXPECT_EQ(frame.dlc, 1);
  EXPECT_EQ(frame.data[0], 0x02);

  EXPECT_FALSE(parse_candump_line("(123) vcan0 XYZ#00", &frame).ok());
  EXPECT_FALSE(parse_candump_line("vcan0 120#0", &frame).ok());
  EXPECT_FALSE(parse_candump_line("vcan0 120#001122334455667788", &frame).ok());
  EXPECT_FALSE(parse_candump_line("garbage", &frame).ok());
}

TEST(Signals, ReplaySourceReadsWholeCapture) {
  ReplayCanSource source(source_path("cabin_normal.log"));
  ASSERT_TRUE(source.open().ok());
  std::size_t frames = 0;
  std::uint64_t last = 0;
  while (true) {
    auto frame = source.next(0);
    ASSERT_TRUE(frame.ok()) << (frame.ok() ? "" : frame.error().message);
    if (!frame.value().has_value())
      break;
    EXPECT_GE(frame.value()->timestamp_us, last);
    last = frame.value()->timestamp_us;
    ++frames;
  }
  EXPECT_EQ(frames, 35u);
}

TEST(Signals, RoundTripsEveryMappedSignal) {
  SignalCodec codec;
  const std::vector<std::pair<SignalId, double>> values = {
      {SignalId::kSeatPositionMm, 400.0}, {SignalId::kSeatReclineDeg, 30.0},
      {SignalId::kHvacSetpointC, 22.5},   {SignalId::kHvacFanLevel, 4.0},
      {SignalId::kCabinLightLevel, 50.0}, {SignalId::kCabinLightColor, 1.0},
      {SignalId::kDoorState, 1.0},        {SignalId::kIgnitionState, 2.0},
  };
  for (const auto& entry : values) {
    auto frame = codec.encode(entry.first, entry.second);
    ASSERT_TRUE(frame.ok()) << signal_meta(entry.first).name << ": "
                            << (frame.ok() ? "" : frame.error().message);
    auto decoded = codec.decode(frame.value());
    ASSERT_TRUE(decoded.ok());
    ASSERT_FALSE(decoded.value().empty());
    const DecodedSignal* match = nullptr;
    for (const auto& candidate : decoded.value()) {
      if (candidate.id == entry.first)
        match = &candidate;
    }
    ASSERT_NE(match, nullptr) << signal_meta(entry.first).name;
    EXPECT_NEAR(match->value, entry.second, 1e-3) << signal_meta(entry.first).name;
    EXPECT_EQ(match->quality, SignalQuality::kOk);
  }
}

TEST(Signals, EncodeIntoMergesSignalsOnOneFrame) {
  SignalCodec codec;
  CanFrame frame;
  ASSERT_TRUE(codec.encode_into(&frame, SignalId::kSeatPositionMm, 800.0).ok());
  ASSERT_TRUE(codec.encode_into(&frame, SignalId::kSeatReclineDeg, 60.0).ok());
  EXPECT_EQ(frame.id, 0x120u);
  auto decoded = codec.decode(frame);
  ASSERT_TRUE(decoded.ok());
  ASSERT_EQ(decoded.value().size(), 2u);
  EXPECT_DOUBLE_EQ(decoded.value()[0].value, 800.0);
  EXPECT_DOUBLE_EQ(decoded.value()[1].value, 60.0);
}

TEST(Signals, BoundaryMathAndStrictEncoding) {
  SignalCodec codec;
  auto low = codec.encode(SignalId::kHvacSetpointC, 16.0);
  ASSERT_TRUE(low.ok());
  auto low_decoded = codec.decode(low.value());
  ASSERT_TRUE(low_decoded.ok());
  EXPECT_FLOAT_EQ(static_cast<float>(low_decoded.value()[0].value), 16.0F);

  auto high = codec.encode(SignalId::kSeatPositionMm, 800.0);
  ASSERT_TRUE(high.ok());
  auto high_decoded = codec.decode(high.value());
  ASSERT_TRUE(high_decoded.ok());
  EXPECT_DOUBLE_EQ(high_decoded.value()[0].value, 800.0);

  EXPECT_EQ(codec.encode(SignalId::kSeatPositionMm, 801.0).code(),
            ErrorCode::kOutOfRange);
  EXPECT_EQ(codec.encode(SignalId::kHvacSetpointC, 15.9).code(), ErrorCode::kOutOfRange);
  EXPECT_EQ(codec.encode(SignalId::kHvacFanLevel, 8.0).code(), ErrorCode::kOutOfRange);
  EXPECT_EQ(
      codec.encode(SignalId::kHvacSetpointC, std::numeric_limits<double>::quiet_NaN())
          .code(),
      ErrorCode::kOutOfRange);
}

TEST(Signals, OutOfRangeFrameIsClampedNotCrashed) {
  SignalCodec codec;
  CanFrame frame;
  frame.id = 0x130;
  frame.dlc = 5;
  // 40.0f = 0x42200000, little-endian; fan 3.
  frame.data[0] = 0x00;
  frame.data[1] = 0x00;
  frame.data[2] = 0x20;
  frame.data[3] = 0x42;
  frame.data[4] = 0x03;
  auto decoded = codec.decode(frame);
  ASSERT_TRUE(decoded.ok());
  ASSERT_EQ(decoded.value().size(), 2u);
  EXPECT_DOUBLE_EQ(decoded.value()[0].value, 30.0);
  EXPECT_EQ(decoded.value()[0].quality, SignalQuality::kClamped);
  EXPECT_EQ(decoded.value()[1].quality, SignalQuality::kOk);
}

TEST(Signals, CacheDetectsChangeAndStaleness) {
  SignalCache cache(1000000);
  EXPECT_TRUE(cache.update(SignalId::kSeatPositionMm, 100.0, 1000000));
  EXPECT_FALSE(cache.is_stale(SignalId::kSeatPositionMm, 1000000));
  EXPECT_TRUE(cache.is_stale(SignalId::kSeatPositionMm, 2000001));

  // Same value inside the max-age window does not republish.
  EXPECT_FALSE(cache.update(SignalId::kSeatPositionMm, 100.0, 1500000));
  // Same value after max_age is a keep-alive refresh.
  EXPECT_TRUE(cache.update(SignalId::kSeatPositionMm, 100.0, 2500001));
  // A real delta republishes immediately.
  EXPECT_TRUE(cache.update(SignalId::kSeatPositionMm, 101.0, 2501000));

  const auto snapshot = cache.snapshot(2501000);
  EXPECT_EQ(snapshot.seat_position_mm, 101);
  EXPECT_EQ(snapshot.hvac_setpoint_c, 16.0F);
}

TEST(Signals, FaultCaptureSurfacesClampAndStaleSeat) {
  std::string logs;
  Logger::instance().reset_for_testing();
  Logger::instance().set_sink_for_testing(&logs);
  Logger::instance().set_level(Level::kWarn);

  SignalProcessor processor;
  replay_into(processor, source_path("cabin_fault.log"));
  const std::uint64_t now = processor.now_us();

  EXPECT_NE(logs.find("signal_out_of_range"), std::string::npos);
  EXPECT_NE(logs.find("hvac_setpoint_c"), std::string::npos);

  EXPECT_FALSE(processor.cache().is_stale(SignalId::kHvacSetpointC, now));
  EXPECT_TRUE(processor.cache().is_stale(SignalId::kSeatPositionMm, now));
  EXPECT_DOUBLE_EQ(cached(processor, SignalId::kHvacSetpointC, 0.0), 22.0);
  EXPECT_DOUBLE_EQ(cached(processor, SignalId::kHvacFanLevel, 0.0), 4.0);
  EXPECT_DOUBLE_EQ(cached(processor, SignalId::kCabinLightLevel, 0.0), 80.0);
  EXPECT_DOUBLE_EQ(cached(processor, SignalId::kCabinLightColor, 0.0), 1.0);
  EXPECT_DOUBLE_EQ(cached(processor, SignalId::kDoorState, 0.0), 2.0);

  Logger::instance().reset_for_testing();
}

TEST(Signals, EndToEndReplayThroughBrokerToHvacService) {
  Stack stack;
  SignalProcessor processor;
  replay_into(processor, source_path("cabin_normal.log"));

  const auto snapshot = processor.snapshot();
  EXPECT_EQ(snapshot.seat_position_mm, 420);
  EXPECT_EQ(snapshot.seat_recline_deg, 18);
  EXPECT_FLOAT_EQ(snapshot.hvac_setpoint_c, 21.5F);
  EXPECT_EQ(snapshot.hvac_fan_level, 3);
  EXPECT_EQ(snapshot.cabin_light_level, 65);
  EXPECT_EQ(snapshot.cabin_light_color, gen::LightColor::kWARM);
  EXPECT_EQ(snapshot.door_state, gen::DoorState::kCLOSED);
  EXPECT_EQ(snapshot.ignition_state, gen::IgnitionState::kON);

  auto signal_client = connect_client(stack.dir);
  auto hvac_client = connect_client(stack.dir);
  auto consumer = connect_client(stack.dir);
  ASSERT_NE(signal_client, nullptr);
  ASSERT_NE(hvac_client, nullptr);
  ASSERT_NE(consumer, nullptr);

  auto signal_service = std::make_shared<SignalServiceImpl>(processor);
  ServiceHost signal_host(signal_client, signal_service);
  ASSERT_TRUE(signal_host.start().ok());

  RemoteSignalLayer layer(hvac_client);
  auto hvac = std::make_shared<HvacServiceImpl>(
      layer,
      [hvac_client](std::uint16_t event_id, const Bytes& payload) {
        const auto status = hvac_client->publish(gen::kHvacServiceId, event_id, payload);
        (void)status;
      },
      [&processor] { return processor.now_us(); });
  ServiceHost hvac_host(hvac_client, hvac);
  ASSERT_TRUE(hvac_host.start().ok());

  ASSERT_TRUE(consumer->find(gen::kHvacServiceId, 1000).ok());

  std::mutex mutex;
  std::condition_variable cv;
  int events = 0;
  int event_fan = -1;
  ASSERT_TRUE(consumer
                  ->subscribe(gen::kHvacServiceId, gen::kHvacServiceEventHvacStateChanged,
                              [&](std::uint16_t, std::uint16_t, const Bytes& payload) {
                                auto state = gen::decode<gen::HvacState>(payload);
                                if (!state)
                                  return;
                                std::lock_guard<std::mutex> lock(mutex);
                                event_fan = state.value().fan_level;
                                ++events;
                                cv.notify_all();
                              })
                  .ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  gen::HvacServiceClient proxy(*consumer, 2000);
  auto response = proxy.set_setpoint(gen::HvacSetRequest{19.5F, 2});
  ASSERT_TRUE(response.ok()) << response.error().message;
  EXPECT_TRUE(response.value().accepted);

  EXPECT_DOUBLE_EQ(cached(processor, SignalId::kHvacSetpointC, 0.0), 19.5);
  EXPECT_DOUBLE_EQ(cached(processor, SignalId::kHvacFanLevel, 0.0), 2.0);
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&] { return events >= 1; }));
    EXPECT_EQ(event_fan, 2);
  }

  // Generated decoding rejects an out-of-range setpoint before it reaches HVAC.
  auto rejected = proxy.set_setpoint(gen::HvacSetRequest{31.0F, 2});
  EXPECT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.code(), ErrorCode::kOutOfRange);
  EXPECT_DOUBLE_EQ(cached(processor, SignalId::kHvacSetpointC, 0.0), 19.5);

  auto state = proxy.get_state();
  ASSERT_TRUE(state.ok());
  EXPECT_FLOAT_EQ(state.value().setpoint_c, 19.5F);
}

TEST(Signals, SocketCanSourceIsCompileGuarded) {
#ifdef __linux__
  auto source = make_can_source("socketcan:vcan0");
  if (!source) {
    GTEST_SKIP() << "vcan0 is not configured: " << source.error().message;
  }
  EXPECT_EQ(source.value()->description(), "socketcan:vcan0");
#else
  auto source = make_can_source("socketcan:vcan0");
  EXPECT_FALSE(source.ok());
  EXPECT_EQ(source.code(), ErrorCode::kUnsupported);
#endif
}
