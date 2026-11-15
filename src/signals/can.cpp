#include "cabin/signals/can.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "cabin/clock.hpp"
#include "cabin/log.hpp"

#ifdef __linux__
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cabin::signals {
namespace {

int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool parse_hex(const std::string_view text, std::uint32_t* value) {
  if (text.empty() || text.size() > 8)
    return false;
  std::uint32_t result = 0;
  for (char c : text) {
    const int digit = hex_value(c);
    if (digit < 0)
      return false;
    result = (result << 4u) | static_cast<std::uint32_t>(digit);
  }
  *value = result;
  return true;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

} // namespace

bool operator==(const CanFrame& lhs, const CanFrame& rhs) {
  return lhs.id == rhs.id && lhs.dlc == rhs.dlc && lhs.data == rhs.data &&
         lhs.timestamp_us == rhs.timestamp_us;
}

Status parse_candump_line(std::string_view line, CanFrame* out) {
  if (out == nullptr) {
    return make_error(ErrorCode::kInvalidArgument, "null frame output");
  }
  line = trim(line);
  if (line.empty() || line.front() == '#') {
    return make_error(ErrorCode::kParse, "empty or comment line");
  }

  std::uint64_t timestamp_us = 0;
  if (line.front() == '(') {
    const std::size_t close = line.find(')');
    if (close == std::string_view::npos) {
      return make_error(ErrorCode::kParse, "unterminated candump timestamp");
    }
    const std::string_view stamp = line.substr(1, close - 1);
    const std::size_t dot = stamp.find('.');
    const std::string_view seconds_text =
        dot == std::string_view::npos ? stamp : stamp.substr(0, dot);
    if (seconds_text.empty()) {
      return make_error(ErrorCode::kParse, "empty candump timestamp");
    }
    std::uint64_t seconds = 0;
    for (char c : seconds_text) {
      if (c < '0' || c > '9') {
        return make_error(ErrorCode::kParse, "non-numeric candump timestamp");
      }
      seconds = seconds * 10u + static_cast<std::uint64_t>(c - '0');
    }
    timestamp_us = seconds * 1000000u;
    if (dot != std::string_view::npos) {
      const std::string_view fraction = stamp.substr(dot + 1);
      std::uint64_t micros = 0;
      int digits = 0;
      for (char c : fraction) {
        if (c < '0' || c > '9' || digits >= 6)
          break;
        micros = micros * 10u + static_cast<std::uint64_t>(c - '0');
        ++digits;
      }
      while (digits < 6) {
        micros *= 10u;
        ++digits;
      }
      timestamp_us += micros;
    }
    line.remove_prefix(close + 1);
    line = trim(line);
  }

  const std::size_t space = line.find(' ');
  if (space == std::string_view::npos) {
    return make_error(ErrorCode::kParse, "missing interface or id#data");
  }
  line.remove_prefix(space + 1);
  line = trim(line);
  const std::size_t hash = line.find('#');
  if (hash == std::string_view::npos) {
    return make_error(ErrorCode::kParse, "missing '#' separator");
  }
  // Optional CAN FD '##' is not supported; the cabin bus is classic CAN.
  if (hash + 1 < line.size() && line[hash + 1] == '#') {
    return make_error(ErrorCode::kUnsupported, "CAN FD frames are not supported");
  }
  std::uint32_t id = 0;
  if (!parse_hex(line.substr(0, hash), &id)) {
    return make_error(ErrorCode::kParse, "invalid CAN id");
  }
  const std::string_view payload = line.substr(hash + 1);
  if (payload.size() % 2 != 0) {
    return make_error(ErrorCode::kParse, "odd number of hex data digits");
  }
  const std::size_t bytes = payload.size() / 2;
  if (bytes > 8) {
    return make_error(ErrorCode::kParse, "classic CAN payload exceeds 8 bytes");
  }
  CanFrame frame;
  frame.id = id;
  frame.dlc = static_cast<std::uint8_t>(bytes);
  for (std::size_t index = 0; index < bytes; ++index) {
    const int high = hex_value(payload[index * 2]);
    const int low = hex_value(payload[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return make_error(ErrorCode::kParse, "invalid hex data");
    }
    frame.data[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  frame.timestamp_us = timestamp_us;
  *out = frame;
  return ok_status();
}

ReplayCanSource::ReplayCanSource(std::string path, bool realtime, double rate)
    : path_(std::move(path)), realtime_(realtime), rate_(rate) {
  if (rate_ <= 0.0)
    rate_ = 1.0;
}

Result<void> ReplayCanSource::open() {
  if (stream_.is_open())
    stream_.close();
  stream_.open(path_);
  if (!stream_.is_open()) {
    return make_error(ErrorCode::kNotFound, "cannot open capture " + path_);
  }
  line_number_ = 0;
  started_ = false;
  first_timestamp_us_ = 0;
  wall_start_us_ = 0;
  return ok_status();
}

std::string ReplayCanSource::description() const {
  return "replay:" + path_ + (realtime_ ? ":realtime" : "");
}

void ReplayCanSource::close() {
  if (stream_.is_open())
    stream_.close();
}

Result<std::optional<CanFrame>> ReplayCanSource::next(int timeout_ms) {
  (void)timeout_ms;
  if (!stream_.is_open()) {
    return make_error(ErrorCode::kUnavailable, "replay source is not open");
  }
  std::string line;
  while (std::getline(stream_, line)) {
    ++line_number_;
    const std::string_view view = trim(line);
    if (view.empty() || view.front() == '#')
      continue;
    CanFrame frame;
    auto status = parse_candump_line(line, &frame);
    if (!status) {
      return make_error(status.error().code, path_ + ":" + std::to_string(line_number_) +
                                                 ": " + status.error().message);
    }
    if (!started_) {
      started_ = true;
      first_timestamp_us_ = frame.timestamp_us;
      wall_start_us_ = now_us();
    }
    if (realtime_ && frame.timestamp_us > first_timestamp_us_) {
      const double desired_us =
          static_cast<double>(frame.timestamp_us - first_timestamp_us_) / rate_;
      const auto desired =
          std::chrono::microseconds(static_cast<std::int64_t>(desired_us));
      const auto elapsed =
          std::chrono::microseconds(static_cast<std::int64_t>(now_us() - wall_start_us_));
      if (desired > elapsed) {
        std::this_thread::sleep_for(desired - elapsed);
      }
    }
    return std::optional<CanFrame>(frame);
  }
  return std::optional<CanFrame>{};
}

#ifdef __linux__
SocketCanSource::SocketCanSource(std::string interface_name)
    : interface_name_(std::move(interface_name)) {}

SocketCanSource::~SocketCanSource() {
  close();
}

Result<void> SocketCanSource::open() {
  fd_ = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (fd_ < 0) {
    return make_error(ErrorCode::kIo, std::string("CAN socket: ") + std::strerror(errno));
  }
  struct ifreq interface{};
  std::snprintf(interface.ifr_name, sizeof(interface.ifr_name), "%s",
                interface_name_.c_str());
  if (::ioctl(fd_, SIOCGIFINDEX, &interface) < 0) {
    const std::string message =
        "SIOCGIFINDEX " + interface_name_ + ": " + std::strerror(errno);
    close();
    return make_error(ErrorCode::kNotFound, message);
  }
  sockaddr_can address{};
  address.can_family = AF_CAN;
  address.can_ifindex = interface.ifr_ifindex;
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    const std::string message =
        "bind CAN " + interface_name_ + ": " + std::strerror(errno);
    close();
    return make_error(ErrorCode::kIo, message);
  }
  return ok_status();
}

std::string SocketCanSource::description() const {
  return "socketcan:" + interface_name_;
}

void SocketCanSource::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

Result<std::optional<CanFrame>> SocketCanSource::next(int timeout_ms) {
  if (fd_ < 0) {
    return make_error(ErrorCode::kUnavailable, "CAN socket is not open");
  }
  pollfd entry{};
  entry.fd = fd_;
  entry.events = POLLIN;
  const int ready = ::poll(&entry, 1, timeout_ms);
  if (ready < 0) {
    if (errno == EINTR)
      return std::optional<CanFrame>{};
    return make_error(ErrorCode::kIo, std::string("poll CAN: ") + std::strerror(errno));
  }
  if (ready == 0)
    return std::optional<CanFrame>{};
  struct can_frame raw{};
  const ssize_t received = ::read(fd_, &raw, sizeof(raw));
  if (received < 0) {
    if (errno == EINTR || errno == EAGAIN)
      return std::optional<CanFrame>{};
    return make_error(ErrorCode::kIo, std::string("read CAN: ") + std::strerror(errno));
  }
  CanFrame frame;
  frame.id = raw.can_id & CAN_EFF_MASK;
  frame.dlc = raw.can_dlc > 8 ? 8 : raw.can_dlc;
  for (std::size_t index = 0; index < frame.dlc; ++index) {
    frame.data[index] = raw.data[index];
  }
  frame.timestamp_us = now_us();
  return std::optional<CanFrame>(frame);
}
#endif

Result<std::unique_ptr<ICanSource>> make_can_source(const std::string& spec,
                                                    double rate) {
  if (spec.rfind("replay:", 0) == 0) {
    std::string path = spec.substr(7);
    bool realtime = false;
    const std::string suffix = ":realtime";
    if (path.size() > suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
      realtime = true;
      path.resize(path.size() - suffix.size());
    }
    if (path.empty()) {
      return make_error(ErrorCode::kInvalidArgument, "replay source needs a path");
    }
    auto source = std::make_unique<ReplayCanSource>(path, realtime, rate);
    auto status = source->open();
    if (!status)
      return status.error();
    return std::unique_ptr<ICanSource>(std::move(source));
  }
  if (spec.rfind("socketcan:", 0) == 0) {
#ifdef __linux__
    const std::string interface = spec.substr(10);
    if (interface.empty()) {
      return make_error(ErrorCode::kInvalidArgument,
                        "socketcan source needs an interface name");
    }
    auto source = std::make_unique<SocketCanSource>(interface);
    auto status = source->open();
    if (!status)
      return status.error();
    log_info("signald", "socketcan_open", {Field("interface", interface)});
    return std::unique_ptr<ICanSource>(std::move(source));
#else
    return make_error(ErrorCode::kUnsupported,
                      "SocketCAN is only available on Linux; use replay:PATH");
#endif
  }
  return make_error(ErrorCode::kInvalidArgument,
                    "unknown source spec (use replay:PATH or socketcan:IFACE)");
}

} // namespace cabin::signals
