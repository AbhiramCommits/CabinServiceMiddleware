#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cabin/broker.hpp"
#include "cabin/client.hpp"
#include "cabin/clock.hpp"
#include "cabin/log.hpp"
#include "cabin/runtime.hpp"

namespace {

constexpr std::uint16_t kBenchService = 0x0200;
constexpr std::uint16_t kBenchEvent = 0x0001;

struct Recorder {
  std::mutex mutex;
  std::vector<std::uint64_t> latencies;
  std::atomic<std::uint64_t> received{0};
};

struct Args {
  std::uint64_t messages{10000};
  std::size_t payload_bytes{64};
  int subscribers{1};
};

double percentile(const std::vector<std::uint64_t>& sorted, double fraction) {
  if (sorted.empty())
    return 0.0;
  const double index = fraction * static_cast<double>(sorted.size() - 1);
  const auto lower = static_cast<std::size_t>(index);
  const auto upper = std::min(lower + 1, sorted.size() - 1);
  const double weight = index - static_cast<double>(lower);
  return static_cast<double>(sorted[lower]) * (1.0 - weight) +
         static_cast<double>(sorted[upper]) * weight;
}

std::string hostname() {
  char buffer[256] = {0};
  if (::gethostname(buffer, sizeof(buffer) - 1) != 0)
    return "unknown";
  return buffer;
}

void write_results(const Args& args, const std::vector<std::uint64_t>& samples,
                   double elapsed_us, std::uint64_t dropped) {
  std::vector<std::uint64_t> sorted = samples;
  std::sort(sorted.begin(), sorted.end());
  const double p50 = percentile(sorted, 0.50);
  const double p95 = percentile(sorted, 0.95);
  const double p99 = percentile(sorted, 0.99);
  const double maximum = sorted.empty() ? 0.0 : static_cast<double>(sorted.back());
  const double rate =
      elapsed_us > 0.0 ? static_cast<double>(args.messages) / (elapsed_us / 1e6) : 0.0;

  auto status = cabin::ensure_directory("results");
  if (!status) {
    std::fprintf(stderr, "bench_ipc: cannot create results/: %s\n",
                 status.error().message.c_str());
    return;
  }
  std::ofstream out("results/ipc_latency.json", std::ios::trunc);
  out.setf(std::ios::fixed);
  out.precision(3);
  out << "{\n";
  out << "  \"benchmark\": \"ipc_publish_latency\",\n";
  out << "  \"host\": \"" << hostname() << "\",\n";
  out << "  \"compiler\": \"" << CABIN_COMPILER << "\",\n";
  out << "  \"build_type\": \"" << CABIN_BUILD_TYPE << "\",\n";
  out << "  \"messages\": " << args.messages << ",\n";
  out << "  \"payload_bytes\": " << args.payload_bytes << ",\n";
  out << "  \"subscribers\": " << args.subscribers << ",\n";
  out << "  \"samples\": " << samples.size() << ",\n";
  out << "  \"p50_us\": " << p50 << ",\n";
  out << "  \"p95_us\": " << p95 << ",\n";
  out << "  \"p99_us\": " << p99 << ",\n";
  out << "  \"max_us\": " << maximum << ",\n";
  out << "  \"messages_per_sec\": " << rate << ",\n";
  out << "  \"elapsed_us\": " << elapsed_us << ",\n";
  out << "  \"dropped\": " << dropped << "\n";
  out << "}\n";

  std::printf("ipc latency: messages=%llu payload=%zu subscribers=%d samples=%zu\n",
              static_cast<unsigned long long>(args.messages), args.payload_bytes,
              args.subscribers, samples.size());
  std::printf(
      "p50=%.1f us p95=%.1f us p99=%.1f us max=%.1f us rate=%.0f msg/s "
      "dropped=%llu\n",
      p50, p95, p99, maximum, rate, static_cast<unsigned long long>(dropped));
  std::printf("wrote results/ipc_latency.json\n");
}

} // namespace

int main(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--messages" && i + 1 < argc) {
      args.messages = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--payload-bytes" && i + 1 < argc) {
      args.payload_bytes = static_cast<std::size_t>(std::atoi(argv[++i]));
    } else if (arg == "--subscribers" && i + 1 < argc) {
      args.subscribers = std::atoi(argv[++i]);
    } else {
      std::fprintf(stderr,
                   "usage: bench_ipc [--messages N] [--payload-bytes B] "
                   "[--subscribers S]\n");
      return 2;
    }
  }
  if (args.payload_bytes < 8)
    args.payload_bytes = 8;
  if (args.payload_bytes > 60 * 1024)
    args.payload_bytes = 60 * 1024;
  if (args.subscribers < 1)
    args.subscribers = 1;
  cabin::Logger::instance().set_level(cabin::Level::kError);

  char runtime_template[] = "/tmp/cabin-bench-XXXXXX";
  char* directory = ::mkdtemp(runtime_template);
  if (directory == nullptr) {
    std::perror("mkdtemp");
    return 1;
  }
  const std::string runtime_dir = directory;

  cabin::BrokerConfig config;
  config.runtime_dir = runtime_dir;
  config.send_queue_limit = 65536;
  config.max_clients = 64;
  auto broker = std::make_unique<cabin::Broker>(config);
  auto status = broker->start();
  if (!status) {
    std::fprintf(stderr, "bench_ipc: broker start failed: %s\n",
                 status.error().message.c_str());
    return 1;
  }
  broker->run_async();

  cabin::ServiceClientOptions options;
  options.runtime_dir = runtime_dir;
  auto publisher_result = cabin::ServiceClient::connect(options);
  if (!publisher_result) {
    std::fprintf(stderr, "bench_ipc: publisher connect failed: %s\n",
                 publisher_result.error().message.c_str());
    return 1;
  }
  auto publisher = publisher_result.value();
  status = publisher->offer_raw(kBenchService);
  if (!status) {
    std::fprintf(stderr, "bench_ipc: offer failed: %s\n", status.error().message.c_str());
    return 1;
  }

  std::vector<std::shared_ptr<cabin::ServiceClient>> subscribers;
  std::vector<std::shared_ptr<Recorder>> recorders;
  for (int index = 0; index < args.subscribers; ++index) {
    auto subscriber = cabin::ServiceClient::connect(options);
    if (!subscriber) {
      std::fprintf(stderr, "bench_ipc: subscriber connect failed: %s\n",
                   subscriber.error().message.c_str());
      return 1;
    }
    auto recorder = std::make_shared<Recorder>();
    recorder->latencies.reserve(static_cast<std::size_t>(args.messages));
    auto subscribed = subscriber.value()->subscribe(
        kBenchService, kBenchEvent,
        [recorder](std::uint16_t, std::uint16_t, const cabin::Bytes& payload) {
          if (payload.size() < 8)
            return;
          cabin::Bytes copy = payload;
          auto sent = copy.read_u64();
          if (!sent)
            return;
          const std::uint64_t now = cabin::now_us();
          {
            std::lock_guard<std::mutex> lock(recorder->mutex);
            recorder->latencies.push_back(now > sent.value() ? now - sent.value() : 0);
          }
          recorder->received.fetch_add(1);
        });
    if (!subscribed) {
      std::fprintf(stderr, "bench_ipc: subscribe failed: %s\n",
                   subscribed.error().message.c_str());
      return 1;
    }
    subscribers.push_back(subscriber.value());
    recorders.push_back(recorder);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  cabin::Bytes payload;
  payload.reserve(args.payload_bytes);
  for (std::size_t index = 0; index < args.payload_bytes; ++index) {
    payload.append_u8(static_cast<std::uint8_t>(index & 0xFFu));
  }

  const std::uint64_t total_expected =
      args.messages * static_cast<std::uint64_t>(args.subscribers);
  const std::uint64_t start_us = cabin::now_us();
  std::uint64_t last_receive_us = start_us;
  for (std::uint64_t index = 0; index < args.messages; ++index) {
    payload.rewind();
    // First 8 bytes are the send timestamp; the rest is deterministic filler.
    cabin::Bytes message;
    message.reserve(args.payload_bytes);
    message.append_u64(cabin::now_us());
    for (std::size_t byte = 8; byte < args.payload_bytes; ++byte) {
      message.append_u8(static_cast<std::uint8_t>((index + byte) & 0xFFu));
    }
    auto published = publisher->publish(kBenchService, kBenchEvent, message);
    if (!published) {
      std::fprintf(stderr, "bench_ipc: publish failed: %s\n",
                   published.error().message.c_str());
      break;
    }
    if ((index & 0x3F) == 0) {
      // Keep the in-flight window small so this measures latency, not loss.
      const std::uint64_t expected_so_far =
          (index + 1) * static_cast<std::uint64_t>(args.subscribers);
      for (int spin = 0; spin < 200; ++spin) {
        std::uint64_t received = 0;
        for (const auto& recorder : recorders)
          received += recorder->received.load();
        if (expected_so_far <= received + 2048)
          break;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    std::uint64_t received = 0;
    for (const auto& recorder : recorders)
      received += recorder->received.load();
    last_receive_us = cabin::now_us();
    if (received >= total_expected)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  std::vector<std::uint64_t> samples;
  for (const auto& recorder : recorders) {
    std::lock_guard<std::mutex> lock(recorder->mutex);
    samples.insert(samples.end(), recorder->latencies.begin(), recorder->latencies.end());
  }
  const double elapsed_us = static_cast<double>(last_receive_us - start_us);
  write_results(args, samples, elapsed_us, broker->dropped_messages());

  for (auto& subscriber : subscribers)
    subscriber->close();
  publisher->close();
  broker->stop();
  broker.reset();
  ::unlink((runtime_dir + "/broker.sock").c_str());
  ::rmdir(runtime_dir.c_str());
  return samples.size() >= total_expected ? 0 : 1;
}
