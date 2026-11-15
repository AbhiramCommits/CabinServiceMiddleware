#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cabin/client.hpp"
#include "cabin/log.hpp"
#include "cabin/signals/cache.hpp"
#include "cabin/signals/can.hpp"
#include "cabin/signals/signal_service.hpp"
#include "cabin/trace.hpp"
#include "cabin_can_map.hpp"
#include "cabin_serdes.hpp"
#include "cabin_services.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) {
  g_stop.store(true);
}

void print_usage() {
  std::puts(
      "usage: signald --source replay:PATH[:realtime]|socketcan:IFACE [--once]\n"
      "               [--runtime-dir DIR] [--max-age-ms N] [--exit-after-replay]\n"
      "\n"
      "  --once               decode the source offline, print a typed summary, exit\n"
      "  --exit-after-replay  do not hold the process open after replay EOF\n");
}

bool publish_snapshot(cabin::ServiceHost& host,
                      const cabin::gen::SignalSnapshot& snapshot) {
  auto encoded = cabin::gen::encode(snapshot);
  if (!encoded)
    return false;
  auto status =
      host.publish(cabin::gen::kSignalServiceEventSignalChanged, encoded.value());
  if (!status) {
    cabin::log_warn("signald", "publish_failed",
                    {cabin::Field("message", status.error().message)});
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  std::string source_spec = "replay:captures/cabin_normal.log";
  std::string runtime_dir = cabin::default_runtime_dir();
  bool once = false;
  bool exit_after_replay = false;
  std::uint64_t max_age_ms = 1000;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--source" && i + 1 < argc) {
      source_spec = argv[++i];
    } else if (arg == "--runtime-dir" && i + 1 < argc) {
      runtime_dir = argv[++i];
    } else if (arg == "--max-age-ms" && i + 1 < argc) {
      max_age_ms = static_cast<std::uint64_t>(std::atoll(argv[++i]));
    } else if (arg == "--once") {
      once = true;
    } else if (arg == "--exit-after-replay") {
      exit_after_replay = true;
    } else {
      std::fprintf(stderr, "signald: unknown argument %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  auto source_result = cabin::signals::make_can_source(source_spec);
  if (!source_result) {
    cabin::log_error("signald", "source_open_failed",
                     {cabin::Field("message", source_result.error().message)});
    return 1;
  }
  auto source = std::move(source_result.value());
  cabin::signals::SignalProcessor processor(max_age_ms * 1000);
  cabin::log_info("signald", "source_opened",
                  {cabin::Field("source", source->description())});

  if (once) {
    std::size_t frames = 0;
    while (true) {
      auto frame = source->next(0);
      if (!frame) {
        cabin::log_error("signald", "replay_error",
                         {cabin::Field("message", frame.error().message)});
        return 1;
      }
      if (!frame.value().has_value())
        break;
      const auto& value = frame.value().value();
      const std::uint64_t timestamp =
          value.timestamp_us != 0 ? value.timestamp_us : cabin::now_us();
      processor.process(value, timestamp);
      ++frames;
    }
    std::printf("replayed %zu CAN frames from %s\n", frames,
                source->description().c_str());
    std::printf("%s", processor.summary().c_str());
    return 0;
  }

  cabin::ServiceClientOptions options;
  options.runtime_dir = runtime_dir;
  auto client_result = cabin::ServiceClient::connect(options);
  if (!client_result) {
    cabin::log_error("signald", "client_connect_failed",
                     {cabin::Field("message", client_result.error().message)});
    return 1;
  }
  auto client = client_result.value();
  auto service = std::make_shared<cabin::signals::SignalServiceImpl>(processor);
  cabin::ServiceHost host(client, service);
  auto started = host.start();
  if (!started) {
    cabin::log_error("signald", "offer_failed",
                     {cabin::Field("message", started.error().message)});
    return 1;
  }
  service->set_on_change([&host](const cabin::gen::SignalSnapshot& snapshot) {
    publish_snapshot(host, snapshot);
  });

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  bool replay_done = false;
  bool stall_logged = false;
  std::uint64_t last_frame_wall_us = cabin::now_us();
  while (!g_stop.load()) {
    if (!replay_done) {
      auto frame = source->next(100);
      if (!frame) {
        cabin::log_error("signald", "replay_error",
                         {cabin::Field("message", frame.error().message)});
        break;
      }
      if (frame.value().has_value()) {
        const auto& value = frame.value().value();
        const std::uint64_t timestamp =
            value.timestamp_us != 0 ? value.timestamp_us : cabin::now_us();
        last_frame_wall_us = cabin::now_us();
        stall_logged = false;
        const auto& changed = processor.process(value, timestamp);
        if (!changed.empty()) {
          publish_snapshot(host, processor.snapshot());
        }
        continue;
      }
      if (!source->exhausted()) {
        if (!stall_logged && source->description().rfind("socketcan:", 0) == 0 &&
            cabin::now_us() - last_frame_wall_us > 5000000) {
          cabin::log_warn("signald", "signal_watchdog_stall",
                          {cabin::Field("source", source->description())});
          stall_logged = true;
        }
        continue;
      }
      replay_done = true;
      cabin::log_info("signald", "replay_finished",
                      {cabin::Field("summary", processor.summary())});
      if (exit_after_replay)
        break;
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  host.stop();
  client->close();
  return 0;
}
