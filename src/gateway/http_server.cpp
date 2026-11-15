#include "cabin/gateway/http_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sstream>

#include "cabin/json.hpp"
#include "cabin/log.hpp"
#include "cabin/trace.hpp"

namespace cabin::gateway {
namespace {

#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif

std::string lowercase(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view trim_view(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

bool write_all(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t result =
        ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (result > 0) {
      sent += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

HttpResponse make_error(int status, const std::string& message) {
  HttpResponse response;
  response.status = status;
  response.body = "{\"error\":{\"code\":\"" + std::string(status_reason(status)) +
                  "\",\"message\":\"" + json_escape(message) + "\"}}\n";
  return response;
}

} // namespace

const char* status_reason(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "BAD_REQUEST";
    case 404:
      return "NOT_FOUND";
    case 405:
      return "METHOD_NOT_ALLOWED";
    case 413:
      return "PAYLOAD_TOO_LARGE";
    case 500:
      return "INTERNAL_ERROR";
    case 503:
      return "UNAVAILABLE";
    case 504:
      return "TIMEOUT";
    default:
      return "ERROR";
  }
}

std::string HttpRequest::header(std::string_view name) const {
  auto it = headers.find(lowercase(name));
  return it == headers.end() ? std::string() : it->second;
}

HttpServer::HttpServer(std::string host, std::uint16_t port, HttpHandler handler)
    : host_(std::move(host)), port_(port), handler_(std::move(handler)) {}

HttpServer::~HttpServer() {
  stop();
}

Status HttpServer::start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return make_error(ErrorCode::kIo, std::string("socket: ") + std::strerror(errno));
  }
  int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return make_error(ErrorCode::kInvalidArgument, "invalid bind address " + host_);
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string message = std::string("bind: ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return make_error(ErrorCode::kIo, message);
  }
  if (::listen(listen_fd_, 32) != 0) {
    const std::string message = std::string("listen: ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return make_error(ErrorCode::kIo, message);
  }
  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
    bound_port_ = ntohs(bound.sin_port);
  } else {
    bound_port_ = port_;
  }
  running_.store(true);
  accept_thread_ = std::thread([this] { accept_loop(); });
  log_info("gateway", "http_listen",
           {Field("host", host_), Field("port", static_cast<std::int64_t>(bound_port_))});
  return ok_status();
}

void HttpServer::accept_loop() {
  while (running_.load()) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &length);
    if (fd < 0) {
      if (errno == EINTR)
        continue;
      if (!running_.load())
        break;
      log_warn("gateway", "accept_failed", {Field("errno", errno)});
      continue;
    }
    active_connections_.fetch_add(1);
    std::thread([this, fd] {
      handle_connection(fd);
      active_connections_.fetch_sub(1);
    }).detach();
  }
}

void HttpServer::handle_connection(int fd) {
  struct timeval timeout{};
  timeout.tv_sec = 3;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  std::string buffer;
  buffer.reserve(1024);
  std::size_t header_end = std::string::npos;
  char chunk[2048];
  bool too_large = false;
  while (header_end == std::string::npos) {
    const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
    if (received <= 0)
      break;
    buffer.append(chunk, static_cast<std::size_t>(received));
    if (buffer.size() > kMaxHeaderBytes) {
      too_large = true;
      break;
    }
    header_end = buffer.find("\r\n\r\n");
  }

  HttpResponse response;
  if (too_large) {
    response = make_error(413, "request headers exceed 64 KiB");
  } else if (header_end == std::string::npos) {
    response = make_error(400, "incomplete HTTP request");
  } else {
    const std::string header_block = buffer.substr(0, header_end);
    std::istringstream stream(header_block);
    std::string request_line;
    std::getline(stream, request_line);
    if (!request_line.empty() && request_line.back() == '\r')
      request_line.pop_back();
    std::istringstream line(request_line);
    HttpRequest request;
    line >> request.method >> request.path >> request.version;
    if (request.method.empty() || request.path.empty() ||
        request.version.rfind("HTTP/", 0) != 0 || request.path.front() != '/') {
      response = make_error(400, "malformed request line");
    } else {
      std::string header;
      while (std::getline(stream, header)) {
        if (!header.empty() && header.back() == '\r')
          header.pop_back();
        const std::size_t colon = header.find(':');
        if (colon == std::string::npos)
          continue;
        request.headers[lowercase(trim_view(header.substr(0, colon)))] =
            std::string(trim_view(header.substr(colon + 1)));
      }
      std::size_t content_length = 0;
      const std::string length_text = request.header("content-length");
      if (!length_text.empty()) {
        char* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(length_text.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || errno == ERANGE) {
          response = make_error(400, "invalid Content-Length");
          content_length = 0;
        } else {
          content_length = static_cast<std::size_t>(parsed);
        }
      }
      if (response.status != 200) {
        // keep the 400
      } else if (content_length > kMaxBodyBytes) {
        response = make_error(413, "request body exceeds 64 KiB");
      } else {
        std::size_t body_start = header_end + 4;
        while (buffer.size() - body_start < content_length) {
          const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
          if (received <= 0)
            break;
          buffer.append(chunk, static_cast<std::size_t>(received));
        }
        if (buffer.size() - body_start < content_length) {
          response = make_error(400, "truncated request body");
        } else {
          request.body = buffer.substr(body_start, content_length);
          try {
            response = handler_ ? handler_(request) : make_error(500, "no handler");
          } catch (const std::exception& error) {
            response = make_error(500, error.what());
          } catch (...) {
            response = make_error(500, "unhandled exception");
          }
        }
      }
    }
  }

  if (response.headers.find("X-Cabin-Trace-Id") == response.headers.end()) {
    response.headers["X-Cabin-Trace-Id"] = trace_id_hex(new_trace_id());
  }
  std::ostringstream out;
  out << "HTTP/1.1 " << response.status << " " << status_reason(response.status)
      << "\r\n";
  out << "Content-Type: " << response.content_type << "\r\n";
  out << "Content-Length: " << response.body.size() << "\r\n";
  out << "Connection: close\r\n";
  for (const auto& header : response.headers) {
    out << header.first << ": " << header.second << "\r\n";
  }
  out << "\r\n";
  out << response.body;
  write_all(fd, out.str());
  ::close(fd);
}

void HttpServer::stop() {
  if (!running_.exchange(false)) {
    if (accept_thread_.joinable())
      accept_thread_.join();
    return;
  }
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (accept_thread_.joinable())
    accept_thread_.join();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (active_connections_.load() > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

} // namespace cabin::gateway
