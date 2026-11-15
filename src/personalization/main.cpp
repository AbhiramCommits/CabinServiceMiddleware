#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "cabin/client.hpp"
#include "cabin/log.hpp"
#include "cabin/personalization/personalization_service.hpp"
#include "cabin/runtime.hpp"
#include "cabin_serdes.hpp"
#include "cabin_services.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) {
  g_stop.store(true);
}

void print_usage() {
  std::puts("usage: personalizationd [--runtime-dir DIR] [--state-dir DIR]\n");
}

} // namespace

int main(int argc, char** argv) {
  std::string runtime_dir = cabin::default_runtime_dir();
  std::string state_dir = cabin::default_state_dir();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--runtime-dir" && i + 1 < argc) {
      runtime_dir = argv[++i];
    } else if (arg == "--state-dir" && i + 1 < argc) {
      state_dir = argv[++i];
    } else {
      std::fprintf(stderr, "personalizationd: unknown argument %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  cabin::ProfileStore store(state_dir);
  auto loaded = store.load();
  if (!loaded) {
    cabin::log_warn("personalization", "profile_load_failed",
                    {cabin::Field("message", loaded.error().message)});
  }

  cabin::ServiceClientOptions options;
  options.runtime_dir = runtime_dir;
  auto client_result = cabin::ServiceClient::connect(options);
  if (!client_result) {
    cabin::log_error("personalization", "client_connect_failed",
                     {cabin::Field("message", client_result.error().message)});
    return 1;
  }
  auto client = client_result.value();

  auto publisher = [client](std::uint16_t event_id, const cabin::Bytes& payload) {
    auto status =
        client->publish(cabin::gen::kPersonalizationServiceId, event_id, payload);
    if (!status) {
      cabin::log_warn("personalization", "publish_failed",
                      {cabin::Field("message", status.error().message)});
    }
  };
  auto service = std::make_shared<cabin::PersonalizationService>(
      store, *client, publisher, [] { return cabin::now_us(); });
  cabin::ServiceHost host(client, service);
  auto started = host.start();
  if (!started) {
    cabin::log_error("personalization", "offer_failed",
                     {cabin::Field("message", started.error().message)});
    return 1;
  }

  auto subscribed = client->subscribe(
      cabin::gen::kSignalServiceId, cabin::gen::kSignalServiceEventSignalChanged,
      [service](std::uint16_t, std::uint16_t, const cabin::Bytes& payload) {
        auto snapshot = cabin::gen::decode<cabin::gen::SignalSnapshot>(payload);
        if (!snapshot)
          return;
        auto status = service->on_ignition_state(snapshot.value().ignition_state);
        (void)status;
      });
  if (!subscribed) {
    cabin::log_warn("personalization", "subscribe_failed",
                    {cabin::Field("message", subscribed.error().message)});
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  cabin::log_info("personalization", "service_offered",
                  {cabin::Field("state_dir", state_dir)});
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  host.stop();
  client->close();
  return 0;
}
