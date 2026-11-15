#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cabin/client.hpp"
#include "cabin/log.hpp"
#include "cabin/runtime.hpp"
#include "cabin/signals/services.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) {
  g_stop.store(true);
}

void print_usage() {
  std::puts(
      "usage: cabin-services [--runtime-dir DIR] [--service seat|hvac|lighting|all]\n");
}

} // namespace

int main(int argc, char** argv) {
  std::string runtime_dir = cabin::default_runtime_dir();
  std::string selection = "all";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--runtime-dir" && i + 1 < argc) {
      runtime_dir = argv[++i];
    } else if (arg == "--service" && i + 1 < argc) {
      selection = argv[++i];
    } else {
      std::fprintf(stderr, "cabin-services: unknown argument %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }
  if (selection != "all" && selection != "seat" && selection != "hvac" &&
      selection != "lighting") {
    std::fprintf(stderr, "cabin-services: --service must be seat|hvac|lighting|all\n");
    return 2;
  }

  cabin::ServiceClientOptions options;
  options.runtime_dir = runtime_dir;
  auto client_result = cabin::ServiceClient::connect(options);
  if (!client_result) {
    cabin::log_error("services", "client_connect_failed",
                     {cabin::Field("message", client_result.error().message)});
    return 1;
  }
  auto client = client_result.value();
  cabin::signals::RemoteSignalLayer layer(client);

  const auto publish = [client](std::uint16_t service_id) {
    return [client, service_id](std::uint16_t event_id, const cabin::Bytes& payload) {
      auto status = client->publish(service_id, event_id, payload);
      if (!status) {
        cabin::log_warn("services", "publish_failed",
                        {cabin::Field("message", status.error().message)});
      }
    };
  };

  auto seat = std::make_shared<cabin::signals::SeatServiceImpl>(
      layer, publish(cabin::gen::kSeatServiceId), [] { return cabin::now_us(); });
  auto hvac = std::make_shared<cabin::signals::HvacServiceImpl>(
      layer, publish(cabin::gen::kHvacServiceId), [] { return cabin::now_us(); });
  auto lighting = std::make_shared<cabin::signals::LightingServiceImpl>(
      layer, publish(cabin::gen::kLightingServiceId), [] { return cabin::now_us(); });

  std::vector<std::unique_ptr<cabin::ServiceHost>> hosts;
  if (selection == "all" || selection == "seat") {
    hosts.push_back(std::make_unique<cabin::ServiceHost>(client, seat));
  }
  if (selection == "all" || selection == "hvac") {
    hosts.push_back(std::make_unique<cabin::ServiceHost>(client, hvac));
  }
  if (selection == "all" || selection == "lighting") {
    hosts.push_back(std::make_unique<cabin::ServiceHost>(client, lighting));
  }
  for (auto& host : hosts) {
    auto status = host->start();
    if (!status) {
      cabin::log_error("services", "offer_failed",
                       {cabin::Field("service", host->service_name()),
                        cabin::Field("message", status.error().message)});
      return 1;
    }
    cabin::log_info("services", "service_offered",
                    {cabin::Field("service", host->service_name())});
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  for (auto& host : hosts)
    host->stop();
  client->close();
  return 0;
}
