#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cabin/broker.hpp"
#include "cabin/log.hpp"
#include "cabin/runtime.hpp"

namespace {

cabin::Broker* g_broker = nullptr;

void handle_signal(int signal_number) {
  if (g_broker != nullptr)
    g_broker->request_stop();
  (void)signal_number;
}

void print_usage() {
  std::puts(
      "usage: cabind [--runtime-dir DIR] [--max-clients N] [--queue-limit N]\n"
      "\n"
      "Serves the cabin broker on $CABIN_RUNTIME_DIR/broker.sock.\n");
}

} // namespace

int main(int argc, char** argv) {
  cabin::BrokerConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--runtime-dir" && i + 1 < argc) {
      config.runtime_dir = argv[++i];
    } else if (arg == "--max-clients" && i + 1 < argc) {
      config.max_clients = static_cast<std::size_t>(std::atol(argv[++i]));
    } else if (arg == "--queue-limit" && i + 1 < argc) {
      config.send_queue_limit = static_cast<std::size_t>(std::atol(argv[++i]));
    } else {
      std::fprintf(stderr, "cabind: unknown argument %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  cabin::Broker broker(config);
  g_broker = &broker;
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  auto status = broker.start();
  if (!status) {
    cabin::log_error("cabind", "start_failed",
                     {cabin::Field("message", status.error().message)});
    return 1;
  }
  broker.run();
  g_broker = nullptr;
  return 0;
}
