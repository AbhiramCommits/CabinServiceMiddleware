#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "cabin/client.hpp"
#include "cabin/gateway/gateway.hpp"
#include "cabin/gateway/http_server.hpp"
#include "cabin/log.hpp"
#include "cabin/runtime.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) {
  g_stop.store(true);
}

void print_usage() {
  std::puts(
      "usage: cabin-gateway [--runtime-dir DIR] [--host 127.0.0.1] [--port 8080]\n");
}

} // namespace

int main(int argc, char** argv) {
  std::string runtime_dir = cabin::default_runtime_dir();
  std::string host = "127.0.0.1";
  std::uint16_t port = 8080;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage();
      return 0;
    }
    if (arg == "--runtime-dir" && i + 1 < argc) {
      runtime_dir = argv[++i];
    } else if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else {
      std::fprintf(stderr, "cabin-gateway: unknown argument %s\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  cabin::ServiceClientOptions options;
  options.runtime_dir = runtime_dir;
  auto client_result = cabin::ServiceClient::connect(options);
  if (!client_result) {
    cabin::log_error("gateway", "client_connect_failed",
                     {cabin::Field("message", client_result.error().message)});
    return 1;
  }
  auto client = client_result.value();
  cabin::gateway::RpcGatewayBackend backend(client);
  cabin::gateway::GatewayApp app(backend);
  cabin::gateway::HttpServer server(
      host, port,
      [&app](const cabin::gateway::HttpRequest& request) { return app.handle(request); });
  auto started = server.start();
  if (!started) {
    cabin::log_error("gateway", "listen_failed",
                     {cabin::Field("message", started.error().message)});
    return 1;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  cabin::log_info("gateway", "gateway_ready",
                  {cabin::Field("host", host),
                   cabin::Field("port", static_cast<std::int64_t>(server.bound_port()))});
  std::printf("cabin-gateway listening on http://%s:%u\n", host.c_str(),
              static_cast<unsigned>(server.bound_port()));
  std::fflush(stdout);
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  server.stop();
  client->close();
  return 0;
}
