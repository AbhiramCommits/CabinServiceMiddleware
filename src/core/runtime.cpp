#include "cabin/runtime.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace cabin {

std::string default_runtime_dir() {
  const char* value = std::getenv("CABIN_RUNTIME_DIR");
  if (value != nullptr && *value != '\0')
    return value;
  return "/tmp/cabin";
}

std::string broker_socket_path(const std::string& runtime_dir) {
  return runtime_dir + "/broker.sock";
}

std::string default_state_dir() {
  const char* value = std::getenv("CABIN_STATE_DIR");
  if (value != nullptr && *value != '\0')
    return value;
  return default_runtime_dir() + "/state";
}

Status ensure_directory(const std::string& path) {
  if (path.empty())
    return make_error(ErrorCode::kInvalidArgument, "empty path");
  struct stat info{};
  if (::stat(path.c_str(), &info) == 0) {
    if (S_ISDIR(info.st_mode))
      return ok_status();
    return make_error(ErrorCode::kIo, path + " exists but is not a directory");
  }
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return make_error(ErrorCode::kIo,
                      "mkdir " + path + " failed: " + std::strerror(errno));
  }
  return ok_status();
}

Status ensure_runtime_dir(const std::string& runtime_dir) {
  return ensure_directory(runtime_dir);
}

} // namespace cabin
