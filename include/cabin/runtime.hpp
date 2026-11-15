#pragma once

#include <string>

#include "cabin/result.hpp"

namespace cabin {

// ${CABIN_RUNTIME_DIR:-/tmp/cabin}
std::string default_runtime_dir();
std::string broker_socket_path(const std::string& runtime_dir);
std::string default_state_dir();

Status ensure_directory(const std::string& path);
Status ensure_runtime_dir(const std::string& runtime_dir);

} // namespace cabin
