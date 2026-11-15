#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>

namespace cabin::test {

struct HttpResponseData {
  int status{0};
  std::map<std::string, std::string> headers;
  std::string body;
};

inline std::string lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

inline HttpResponseData http_raw(std::uint16_t port, const std::string& raw) {
  HttpResponseData result;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return result;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return result;
  }
  struct timeval timeout{};
  timeout.tv_sec = 5;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  std::size_t sent = 0;
  while (sent < raw.size()) {
    const ssize_t count = ::send(fd, raw.data() + sent, raw.size() - sent, 0);
    if (count <= 0)
      break;
    sent += static_cast<std::size_t>(count);
  }
  std::string response;
  char chunk[4096];
  while (true) {
    const ssize_t count = ::recv(fd, chunk, sizeof(chunk), 0);
    if (count <= 0)
      break;
    response.append(chunk, static_cast<std::size_t>(count));
  }
  ::close(fd);
  const std::size_t first_space = response.find(' ');
  if (first_space == std::string::npos)
    return result;
  result.status = std::atoi(response.c_str() + first_space + 1);
  const std::size_t header_end = response.find("\r\n\r\n");
  if (header_end != std::string::npos) {
    result.body = response.substr(header_end + 4);
  }
  return result;
}

inline HttpResponseData http_request(
    std::uint16_t port, const std::string& method, const std::string& path,
    const std::string& body = "",
    const std::map<std::string, std::string>& extra_headers = {}) {
  HttpResponseData result;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return result;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return result;
  }
  struct timeval timeout{};
  timeout.tv_sec = 5;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  std::string request;
  request += method + " " + path + " HTTP/1.1\r\n";
  request += "Host: 127.0.0.1\r\n";
  request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  for (const auto& entry : extra_headers) {
    request += entry.first + ": " + entry.second + "\r\n";
  }
  request += "Connection: close\r\n\r\n";
  request += body;
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t count = ::send(fd, request.data() + sent, request.size() - sent, 0);
    if (count <= 0)
      break;
    sent += static_cast<std::size_t>(count);
  }

  std::string raw;
  char chunk[4096];
  while (true) {
    const ssize_t count = ::recv(fd, chunk, sizeof(chunk), 0);
    if (count <= 0)
      break;
    raw.append(chunk, static_cast<std::size_t>(count));
  }
  ::close(fd);

  const std::size_t first_space = raw.find(' ');
  if (first_space == std::string::npos)
    return result;
  result.status = std::atoi(raw.c_str() + first_space + 1);
  const std::size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string::npos)
    return result;
  const std::string header_block = raw.substr(0, header_end);
  result.body = raw.substr(header_end + 4);
  std::size_t line_start = header_block.find("\r\n");
  while (line_start != std::string::npos) {
    line_start += 2;
    const std::size_t line_end = header_block.find("\r\n", line_start);
    const std::string line = header_block.substr(line_start, line_end == std::string::npos
                                                                 ? std::string::npos
                                                                 : line_end - line_start);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = lower(line.substr(0, colon));
      std::string value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ')
        value.erase(value.begin());
      result.headers[key] = value;
    }
    if (line_end == std::string::npos)
      break;
    line_start = line_end;
  }
  return result;
}

} // namespace cabin::test
