#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "cabin/result.hpp"
#include "cabin_types.hpp"

namespace cabin {

// Per-driver profiles persisted as JSON at
// ${CABIN_STATE_DIR}/profiles.json with temp-file + fsync + rename.
class ProfileStore {
 public:
  explicit ProfileStore(std::string state_dir);

  Status load();
  Status save() const;

  Status put(const gen::Profile& profile);
  std::optional<gen::Profile> get(std::string_view driver_id) const;
  std::size_t size() const;

  Status set_active_driver(std::string driver_id);
  std::string active_driver() const;

  std::string path() const;
  const std::string& state_dir() const { return state_dir_; }

 private:
  std::string state_dir_;
  mutable std::mutex mutex_;
  std::map<std::string, gen::Profile> profiles_;
  std::string active_driver_;
};

} // namespace cabin
