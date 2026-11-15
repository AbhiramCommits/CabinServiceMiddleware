#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cabin/broker.hpp"
#include "cabin/client.hpp"
#include "cabin/runtime.hpp"

using namespace cabin;

namespace {

constexpr std::uint16_t kEchoA = 0x0310;
constexpr std::uint16_t kEchoB = 0x0311;
constexpr std::uint16_t kSlow = 0x0312;
constexpr std::uint16_t kEvent = 7;

std::string make_runtime_dir() {
  char name[] = "/tmp/cabin-test-XXXXXX";
  char* directory = ::mkdtemp(name);
  EXPECT_NE(directory, nullptr);
  return directory != nullptr ? std::string(directory) : std::string("/tmp/cabin-test");
}

class EchoService : public gen::IService {
 public:
  EchoService(std::uint16_t id, int delay_ms = 0) : id_(id), delay_ms_(delay_ms) {}

  std::uint16_t service_id() const override { return id_; }
  const char* service_name() const override { return "EchoService"; }

  Result<Bytes> dispatch(std::uint16_t method_id, const Bytes& request) override {
    if (method_id != 1) {
      return make_error(ErrorCode::kNotFound, "unknown method");
    }
    if (delay_ms_ > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
    }
    return Bytes(request);
  }

 private:
  std::uint16_t id_;
  int delay_ms_;
};

std::shared_ptr<ServiceClient> connect_client(const std::string& dir) {
  ServiceClientOptions options;
  options.runtime_dir = dir;
  auto client = ServiceClient::connect(options);
  EXPECT_TRUE(client.ok()) << (client.ok() ? "" : client.error().message);
  if (!client)
    return nullptr;
  return client.value();
}

std::size_t open_fd_count() {
  std::size_t count = 0;
#if defined(__APPLE__)
  const char* path = "/dev/fd";
#else
  const char* path = "/proc/self/fd";
#endif
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
    (void)entry;
    ++count;
  }
  return count;
}

struct Stack {
  std::string dir;
  std::unique_ptr<Broker> broker;

  explicit Stack(std::size_t queue_limit = 4096) : dir(make_runtime_dir()) {
    BrokerConfig config;
    config.runtime_dir = dir;
    config.send_queue_limit = queue_limit;
    broker = std::make_unique<Broker>(config);
    auto status = broker->start();
    EXPECT_TRUE(status.ok()) << (status.ok() ? "" : status.error().message);
    broker->run_async();
  }

  ~Stack() {
    if (broker) {
      broker->stop();
      broker.reset();
    }
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  }
};

} // namespace

TEST(Broker, DiscoveryBeforeAndAfterProviderStarts) {
  Stack stack;
  auto client = connect_client(stack.dir);
  ASSERT_NE(client, nullptr);

  auto missing = client->find(0x0399, 120);
  EXPECT_FALSE(missing.ok());
  EXPECT_EQ(missing.code(), ErrorCode::kTimeout);

  std::atomic<bool> found{false};
  Result<void> find_result;
  std::thread finder([&] {
    find_result = client->find(0x0399, 3000);
    found.store(find_result.ok());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto provider = connect_client(stack.dir);
  ASSERT_NE(provider, nullptr);
  auto service = std::make_shared<EchoService>(0x0399);
  ServiceHost host(provider, service);
  ASSERT_TRUE(host.start().ok());
  finder.join();
  EXPECT_TRUE(found.load()) << (find_result.ok() ? "" : find_result.error().message);

  auto again = client->find(0x0399, 500);
  EXPECT_TRUE(again.ok());

  auto call = client->call(0x0399, 1, Bytes(std::string_view("hello")), 1000);
  ASSERT_TRUE(call.ok()) << call.error().message;
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(call.value().data()),
                        call.value().size()),
            "hello");
}

TEST(Broker, TwoHostsThreeClientsRouteByServiceId) {
  Stack stack;
  auto host_a_client = connect_client(stack.dir);
  auto host_b_client = connect_client(stack.dir);
  ASSERT_NE(host_a_client, nullptr);
  ASSERT_NE(host_b_client, nullptr);
  ServiceHost host_a(host_a_client, std::make_shared<EchoService>(kEchoA));
  ServiceHost host_b(host_b_client, std::make_shared<EchoService>(kEchoB));
  ASSERT_TRUE(host_a.start().ok());
  ASSERT_TRUE(host_b.start().ok());

  std::vector<std::shared_ptr<ServiceClient>> clients;
  for (int index = 0; index < 3; ++index) {
    auto client = connect_client(stack.dir);
    ASSERT_NE(client, nullptr);
    ASSERT_TRUE(client->find(kEchoA, 1000).ok());
    ASSERT_TRUE(client->find(kEchoB, 1000).ok());
    clients.push_back(client);
  }

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 3; ++index) {
    threads.emplace_back([&, index] {
      for (int call_index = 0; call_index < 100; ++call_index) {
        const std::string text =
            "client-" + std::to_string(index) + "-" + std::to_string(call_index);
        auto result_a = clients[static_cast<std::size_t>(index)]->call(
            kEchoA, 1, Bytes(std::string_view(text)), 1000);
        if (!result_a.ok()) {
          failures.fetch_add(1);
          continue;
        }
        const std::string reply_a(reinterpret_cast<const char*>(result_a.value().data()),
                                  result_a.value().size());
        if (reply_a != text)
          failures.fetch_add(1);

        auto result_b = clients[static_cast<std::size_t>(index)]->call(
            kEchoB, 1, Bytes(std::string_view(text)), 1000);
        if (!result_b.ok()) {
          failures.fetch_add(1);
          continue;
        }
        const std::string reply_b(reinterpret_cast<const char*>(result_b.value().data()),
                                  result_b.value().size());
        if (reply_b != text)
          failures.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads)
    thread.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST(Broker, RequestTimeoutReturnsTimeout) {
  Stack stack;
  auto provider = connect_client(stack.dir);
  ASSERT_NE(provider, nullptr);
  ServiceHost host(provider, std::make_shared<EchoService>(kSlow, 250));
  ASSERT_TRUE(host.start().ok());

  auto client = connect_client(stack.dir);
  ASSERT_NE(client, nullptr);
  ASSERT_TRUE(client->find(kSlow, 1000).ok());
  const auto started = std::chrono::steady_clock::now();
  auto result = client->call(kSlow, 1, Bytes(std::string_view("slow")), 50);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.code(), ErrorCode::kTimeout);
  EXPECT_GE(elapsed, 40);
  EXPECT_LT(elapsed, 240);
}

TEST(Broker, SubscribeNotifyAndUnsubscribe) {
  Stack stack;
  auto provider = connect_client(stack.dir);
  ASSERT_NE(provider, nullptr);
  ServiceHost host(provider, std::make_shared<EchoService>(kEchoA));
  ASSERT_TRUE(host.start().ok());

  auto client = connect_client(stack.dir);
  ASSERT_NE(client, nullptr);

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  std::string last;
  auto status = client->subscribe(
      kEchoA, kEvent,
      [&](std::uint16_t service_id, std::uint16_t event_id, const Bytes& payload) {
        EXPECT_EQ(service_id, kEchoA);
        EXPECT_EQ(event_id, kEvent);
        std::lock_guard<std::mutex> lock(mutex);
        last.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
        ++received;
        cv.notify_all();
      });
  ASSERT_TRUE(status.ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  for (int index = 0; index < 5; ++index) {
    ASSERT_TRUE(
        host.publish(kEvent, Bytes(std::string_view("event-" + std::to_string(index))))
            .ok());
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(
        cv.wait_for(lock, std::chrono::seconds(2), [&] { return received >= 5; }));
    EXPECT_EQ(last, "event-4");
  }

  ASSERT_TRUE(client->unsubscribe(kEchoA, kEvent).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  ASSERT_TRUE(host.publish(kEvent, Bytes(std::string_view("ignored"))).ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  std::lock_guard<std::mutex> lock(mutex);
  EXPECT_EQ(received, 5);
}

TEST(Broker, BrokerRestartAndClientReconnect) {
  Stack stack;
  auto client = connect_client(stack.dir);
  auto provider = connect_client(stack.dir);
  ASSERT_NE(client, nullptr);
  ASSERT_NE(provider, nullptr);
  ServiceHost host(provider, std::make_shared<EchoService>(kEchoA));
  ASSERT_TRUE(host.start().ok());
  ASSERT_TRUE(client->find(kEchoA, 1000).ok());
  auto first = client->call(kEchoA, 1, Bytes(std::string_view("before")), 1000);
  ASSERT_TRUE(first.ok());

  stack.broker->stop();
  stack.broker.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  BrokerConfig config;
  config.runtime_dir = stack.dir;
  config.send_queue_limit = 4096;
  stack.broker = std::make_unique<Broker>(config);
  ASSERT_TRUE(stack.broker->start().ok());
  stack.broker->run_async();

  ASSERT_TRUE(client->reconnect().ok()) << "client reconnect failed";
  ASSERT_TRUE(provider->reconnect().ok()) << "provider reconnect failed";
  ASSERT_TRUE(client->find(kEchoA, 1000).ok());
  auto after = client->call(kEchoA, 1, Bytes(std::string_view("after")), 1000);
  ASSERT_TRUE(after.ok()) << after.error().message;
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(after.value().data()),
                        after.value().size()),
            "after");
}

TEST(Broker, FiftyThousandRequestsWithZeroLoss) {
  Stack stack;
  auto provider = connect_client(stack.dir);
  ASSERT_NE(provider, nullptr);
  ServiceHost host(provider, std::make_shared<EchoService>(kEchoA));
  ASSERT_TRUE(host.start().ok());

  auto client = connect_client(stack.dir);
  ASSERT_NE(client, nullptr);
  ASSERT_TRUE(client->find(kEchoA, 1000).ok());

  int failures = 0;
  constexpr int kMessages = 50000;
  const Bytes payload(std::string_view("soak-payload-0123456789"));
  for (int index = 0; index < kMessages; ++index) {
    auto result = client->call(kEchoA, 1, payload, 2000);
    if (!result.ok() || result.value().size() != payload.size()) {
      ++failures;
    }
  }
  EXPECT_EQ(failures, 0);
}

TEST(Broker, NoLeakedFileDescriptors) {
  const std::size_t before = open_fd_count();
  {
    Stack stack;
    auto provider = connect_client(stack.dir);
    auto client = connect_client(stack.dir);
    ASSERT_NE(provider, nullptr);
    ASSERT_NE(client, nullptr);
    ServiceHost host(provider, std::make_shared<EchoService>(kEchoA));
    ASSERT_TRUE(host.start().ok());
    ASSERT_TRUE(client->find(kEchoA, 1000).ok());
    auto result = client->call(kEchoA, 1, Bytes(std::string_view("fd")), 1000);
    ASSERT_TRUE(result.ok());
    client->close();
    provider->close();
    host.stop();
    stack.broker->stop();
    stack.broker.reset();
  }
  const std::size_t after = open_fd_count();
  EXPECT_EQ(after, before) << "file descriptor leak detected";
}
