#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "cabin/result.hpp"

namespace cabin::signals {

struct CanFrame {
  std::uint32_t id{0};
  std::uint8_t dlc{0};
  std::array<std::uint8_t, 8> data{};
  std::uint64_t timestamp_us{0};
};

bool operator==(const CanFrame& lhs, const CanFrame& rhs);

// Abstract CAN ingestion. ReplayCanSource works everywhere; SocketCanSource is
// only compiled on Linux so the rest of the project stays portable.
class ICanSource {
 public:
  virtual ~ICanSource() = default;
  virtual Result<std::optional<CanFrame>> next(int timeout_ms) = 0;
  virtual std::string description() const = 0;
  virtual void close() = 0;
};

// Parses `(1700000000.123456) vcan0 120#a40112` (candump -L text format).
Status parse_candump_line(std::string_view line, CanFrame* out);

class ReplayCanSource : public ICanSource {
 public:
  ReplayCanSource(std::string path, bool realtime = false, double rate = 1.0);
  Result<void> open();
  Result<std::optional<CanFrame>> next(int timeout_ms) override;
  std::string description() const override;
  void close() override;
  std::size_t line_number() const { return line_number_; }

 private:
  std::string path_;
  bool realtime_;
  double rate_;
  std::ifstream stream_;
  std::size_t line_number_{0};
  bool started_{false};
  std::uint64_t first_timestamp_us_{0};
  std::uint64_t wall_start_us_{0};
};

#ifdef __linux__
class SocketCanSource : public ICanSource {
 public:
  explicit SocketCanSource(std::string interface_name);
  Result<void> open();
  Result<std::optional<CanFrame>> next(int timeout_ms) override;
  std::string description() const override;
  void close() override;
  ~SocketCanSource() override;

 private:
  std::string interface_name_;
  int fd_{-1};
};
#endif

// spec: "replay:PATH" (as fast as possible), "replay:PATH:realtime",
// "socketcan:vcan0" (Linux only).
Result<std::unique_ptr<ICanSource>> make_can_source(const std::string& spec,
                                                    double rate = 1.0);

} // namespace cabin::signals
