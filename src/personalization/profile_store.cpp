#include "cabin/personalization/profile_store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "cabin/json.hpp"
#include "cabin/log.hpp"
#include "cabin/runtime.hpp"
#include "cabin_serdes.hpp"

namespace cabin {
namespace {

JsonValue profile_to_json(const gen::Profile& profile) {
  JsonValue object = JsonValue::object();
  object.set("driver_id", JsonValue::string(profile.driver_id));
  object.set("seat_position_mm", JsonValue::number(profile.seat_position_mm));
  object.set("seat_recline_deg", JsonValue::number(profile.seat_recline_deg));
  object.set("hvac_setpoint_c", JsonValue::number(profile.hvac_setpoint_c));
  object.set("hvac_fan_level", JsonValue::number(profile.hvac_fan_level));
  object.set("light_level", JsonValue::number(profile.light_level));
  object.set("light_color", JsonValue::string(gen::to_string(profile.light_color)));
  object.set("schema_version", JsonValue::number(profile.schema_version));
  object.set("preferred_language", JsonValue::string(profile.preferred_language));
  return object;
}

Result<gen::Profile> profile_from_json(const JsonValue& object) {
  if (!object.is_object()) {
    return make_error(ErrorCode::kParse, "profile must be a JSON object");
  }
  gen::Profile profile;
  profile.driver_id = object.get_string("driver_id");
  profile.seat_position_mm =
      static_cast<std::uint16_t>(object.get_int("seat_position_mm", 0));
  profile.seat_recline_deg =
      static_cast<std::uint8_t>(object.get_int("seat_recline_deg", 0));
  profile.hvac_setpoint_c =
      static_cast<float>(object.get_number("hvac_setpoint_c", 16.0));
  profile.hvac_fan_level = static_cast<std::uint8_t>(object.get_int("hvac_fan_level", 0));
  profile.light_level = static_cast<std::uint8_t>(object.get_int("light_level", 0));
  const std::string color = object.get_string("light_color", "WARM");
  const auto parsed_color = gen::light_color_from_string(color);
  if (!parsed_color.has_value()) {
    return make_error(ErrorCode::kParse, "invalid light_color " + color);
  }
  profile.light_color = parsed_color.value();
  profile.schema_version =
      static_cast<std::uint16_t>(object.get_int("schema_version", 1));
  profile.preferred_language = object.get_string("preferred_language", "en");
  auto valid = gen::validate(profile);
  if (!valid)
    return valid.error();
  return profile;
}

Status write_atomic(const std::string& path, const std::string& contents) {
  const std::string temporary = path + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return make_error(ErrorCode::kIo, "open " + temporary + ": " + std::strerror(errno));
  }
  std::size_t written = 0;
  while (written < contents.size()) {
    const ssize_t result =
        ::write(fd, contents.data() + written, contents.size() - written);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      const std::string message = std::strerror(errno);
      ::close(fd);
      ::unlink(temporary.c_str());
      return make_error(ErrorCode::kIo, "write " + temporary + ": " + message);
    }
    written += static_cast<std::size_t>(result);
  }
  if (::fsync(fd) != 0) {
    const std::string message = std::strerror(errno);
    ::close(fd);
    ::unlink(temporary.c_str());
    return make_error(ErrorCode::kIo, "fsync " + temporary + ": " + message);
  }
  ::close(fd);
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    const std::string message = std::strerror(errno);
    ::unlink(temporary.c_str());
    return make_error(ErrorCode::kIo, "rename " + path + ": " + message);
  }
  return ok_status();
}

} // namespace

ProfileStore::ProfileStore(std::string state_dir) : state_dir_(std::move(state_dir)) {}

std::string ProfileStore::path() const {
  return state_dir_ + "/profiles.json";
}

Status ProfileStore::load() {
  std::lock_guard<std::mutex> lock(mutex_);
  profiles_.clear();
  active_driver_.clear();
  std::ifstream input(path());
  if (!input.is_open()) {
    return ok_status();
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  auto parsed = parse_json(buffer.str());
  if (!parsed) {
    return make_error(ErrorCode::kParse, "profiles.json: " + parsed.error().message);
  }
  const JsonValue& root = parsed.value();
  if (!root.is_object()) {
    return make_error(ErrorCode::kParse, "profiles.json root must be an object");
  }
  active_driver_ = root.get_string("active_driver");
  const JsonValue* profiles = root.find("profiles");
  if (profiles == nullptr)
    return ok_status();
  if (!profiles->is_object()) {
    return make_error(ErrorCode::kParse, "profiles must be an object");
  }
  for (std::size_t index = 0; index < profiles->object_size(); ++index) {
    const JsonValue& value = profiles->value_at(index);
    auto profile = profile_from_json(value);
    if (!profile) {
      return make_error(ErrorCode::kParse, "profile " + profiles->key_at(index) + ": " +
                                               profile.error().message);
    }
    if (profile.value().driver_id.empty()) {
      profile.value().driver_id = profiles->key_at(index);
    }
    profiles_[profile.value().driver_id] = profile.value();
  }
  return ok_status();
}

Status ProfileStore::save() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto status = ensure_directory(state_dir_);
  if (!status)
    return status;

  JsonValue root = JsonValue::object();
  root.set("schema_version", JsonValue::number(1));
  root.set("active_driver", JsonValue::string(active_driver_));
  JsonValue profiles = JsonValue::object();
  for (const auto& entry : profiles_) {
    profiles.set(entry.first, profile_to_json(entry.second));
  }
  root.set("profiles", std::move(profiles));
  return write_atomic(path(), root.dump(2) + "\n");
}

Status ProfileStore::put(const gen::Profile& profile) {
  auto valid = gen::validate(profile);
  if (!valid)
    return valid.error();
  if (profile.driver_id.empty()) {
    return make_error(ErrorCode::kInvalidArgument, "driver_id must not be empty");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  profiles_[profile.driver_id] = profile;
  return ok_status();
}

std::optional<gen::Profile> ProfileStore::get(std::string_view driver_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = profiles_.find(std::string(driver_id));
  if (it == profiles_.end())
    return std::nullopt;
  return it->second;
}

std::size_t ProfileStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return profiles_.size();
}

Status ProfileStore::set_active_driver(std::string driver_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  active_driver_ = std::move(driver_id);
  return ok_status();
}

std::string ProfileStore::active_driver() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_driver_;
}

} // namespace cabin
