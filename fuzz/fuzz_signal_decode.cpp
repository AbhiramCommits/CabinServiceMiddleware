#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "cabin/signals/codec.hpp"

#ifdef CABIN_FUZZ_FALLBACK
#include <filesystem>
#include <fstream>
#include <iostream>
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size < 5)
    return 0;
  cabin::signals::CanFrame frame;
  frame.id = static_cast<std::uint32_t>(data[0]) |
             (static_cast<std::uint32_t>(data[1]) << 8u) |
             (static_cast<std::uint32_t>(data[2]) << 16u) |
             (static_cast<std::uint32_t>(data[3]) << 24u);
  frame.dlc = static_cast<std::uint8_t>(std::min<std::size_t>(data[4], 8));
  for (std::size_t index = 0; index < frame.dlc && 5 + index < size; ++index) {
    frame.data[index] = data[5 + index];
  }
  frame.timestamp_us = 1000000;
  cabin::signals::SignalCodec codec;
  auto decoded = codec.decode(frame);
  (void)decoded;

  std::uint64_t bits = 0;
  for (std::size_t index = 0; index < 8 && 5 + index < size; ++index) {
    bits |= static_cast<std::uint64_t>(data[5 + index]) << (8 * index);
  }
  for (std::size_t index = 0; index < cabin::signals::kSignalCount; ++index) {
    auto value = codec.decode_bits(static_cast<cabin::signals::SignalId>(index), bits);
    (void)value;
  }
  return 0;
}

#ifdef CABIN_FUZZ_FALLBACK
namespace {

std::uint64_t g_state = 0xD1B54A32D192ED03ULL;

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
    std::size_t length = static_cast<std::size_t>(next_random() % 64);
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
