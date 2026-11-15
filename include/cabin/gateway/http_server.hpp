#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>

#include "cabin/result.hpp"

namespace cabin::gateway {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string version;
  std::map<std::string, std::string> headers;
  std::string body;

  std::string header(std::string_view name) const;
};

struct HttpResponse {
  int status{200};
  std::string body;
  std::string content_type{"application/json"};
  std::map<std::string, std::string> headers;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// Minimal HTTP/1.1 server: one request per connection, 64 KiB header/body caps,
// thread-per-connection. It is intentionally small; the gateway only needs five
// JSON endpoints.
class HttpServer {
 public:
  HttpServer(std::string host, std::uint16_t port, HttpHandler handler);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  Status start();
  void stop();
  std::uint16_t bound_port() const { return bound_port_; }
  bool running() const { return running_.load(); }

  static constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
  static constexpr std::size_t kMaxBodyBytes = 64 * 1024;

 private:
  void accept_loop();
  void handle_connection(int fd);

  std::string host_;
  std::uint16_t port_;
  std::uint16_t bound_port_{0};
  HttpHandler handler_;
  std::atomic<int> listen_fd_{-1};
  std::atomic<bool> running_{false};
  std::atomic<int> active_connections_{0};
  std::thread accept_thread_;
};

const char* status_reason(int status);

} // namespace cabin::gateway
