#include <cstddef>
#include <cstdint>
#include <vector>

#include "cabin/frame.hpp"

#ifdef CABIN_FUZZ_FALLBACK
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0)
    return 0;
  cabin::FrameDecoder decoder;
  decoder.feed(data, size);
  for (int index = 0; index < 16; ++index) {
    auto frame = decoder.next();
    if (!frame || !frame.value().has_value())
      break;
    const auto& decoded = frame.value().value();
    (void)decoded.payload.size();
  }
  std::size_t consumed = 0;
  auto single = cabin::decode_frame(data, size, &consumed);
  (void)single;
  return 0;
}

#ifdef CABIN_FUZZ_FALLBACK
namespace {

std::uint64_t g_state = 0x9E3779B97F4A7C15ULL;

std::uint64_t next_random() {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return g_state;
}

void run_bytes(const std::vector<std::uint8_t>& bytes) {
  LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
}

} // namespace

int main(int argc, char** argv) {
  std::size_t files = 0;
  if (argc > 1) {
    for (int index = 1; index < argc; ++index) {
      std::error_code error;
      if (std::filesystem::is_directory(argv[index], error)) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(argv[index], error)) {
          if (!entry.is_regular_file())
            continue;
          std::ifstream input(entry.path(), std::ios::binary);
          std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                          std::istreambuf_iterator<char>());
          run_bytes(bytes);
          ++files;
        }
      } else {
        std::ifstream input(argv[index], std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
        run_bytes(bytes);
        ++files;
      }
    }
  }
  for (int iteration = 0; iteration < 200000; ++iteration) {
    std::size_t length = static_cast<std::size_t>(next_random() % 160);
    std::vector<std::uint8_t> bytes(length);
    for (std::size_t index = 0; index < length; ++index) {
      bytes[index] = static_cast<std::uint8_t>(next_random());
    }
    run_bytes(bytes);
  }
  std::cout << "fallback fuzz: " << files << " corpus files + 200000 random inputs\n";
  return 0;
}
#endif
