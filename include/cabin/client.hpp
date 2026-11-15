#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cabin/bytes.hpp"
#include "cabin/frame.hpp"
#include "cabin/result.hpp"
#include "cabin/runtime.hpp"
#include "cabin_services.hpp"

namespace cabin {

using NotifyCallback = std::function<void(std::uint16_t service_id,
                                          std::uint16_t event_id, const Bytes& payload)>;

struct ServiceClientOptions {
  std::string runtime_dir{default_runtime_dir()};
  int connect_retry_ms{500};
};

// A connection to cabind. Implements the generated IRpcChannel so generated
// proxies (SeatServiceClient, ...) work unchanged.
//
// Reader, request-dispatch and notification-dispatch each run on their own
// thread. That is deliberate: a method handler is allowed to issue another
// client.call() (personalization applies profiles through Seat/HVAC/Lighting),
// and a notification callback may do the same, without deadlocking the socket.
class ServiceClient : public gen::IRpcChannel {
 public:
  ~ServiceClient() override;

  ServiceClient(const ServiceClient&) = delete;
  ServiceClient& operator=(const ServiceClient&) = delete;

  static Result<std::shared_ptr<ServiceClient>> connect(
      const ServiceClientOptions& options = {});

  Result<void> reconnect();
  void close();

  Result<void> find(std::uint16_t service_id, int timeout_ms = 1000);
  bool available(std::uint16_t service_id) const;
  Result<void> subscribe(std::uint16_t service_id, std::uint16_t event_id,
                         NotifyCallback callback);
  Result<void> unsubscribe(std::uint16_t service_id, std::uint16_t event_id);

  Result<Bytes> call(std::uint16_t service_id, std::uint16_t method_id,
                     const Bytes& request, int timeout_ms) override;

  Result<void> offer(const std::shared_ptr<gen::IService>& service);
  Result<void> offer_raw(std::uint16_t service_id);
  Result<void> withdraw(std::uint16_t service_id);
  Result<void> publish(std::uint16_t service_id, std::uint16_t event_id,
                       const Bytes& payload);

  std::string socket_path() const;
  std::string runtime_dir() const { return options_.runtime_dir; }
  bool running() const { return threads_running_.load(); }

 private:
  struct PendingCall {
    std::promise<Result<Bytes>> promise;
  };
  struct IncomingRequest {
    Frame frame;
  };
  struct IncomingEvent {
    std::uint16_t service_id;
    std::uint16_t event_id;
    Bytes payload;
  };

  explicit ServiceClient(ServiceClientOptions options);

  Result<void> open_socket();
  Result<void> start_threads();
  void stop_threads();
  void reader_loop();
  void request_loop();
  void event_loop();
  void handle_frame(Frame frame);
  void fail_pending(const Error& error);
  Result<void> send_frame(const Frame& frame);
  void send_error(std::uint16_t service_id, std::uint16_t method_id, std::uint32_t seq,
                  std::uint64_t trace_id, const Error& error);
  Result<void> offer_frame(std::uint16_t service_id);

  ServiceClientOptions options_;
  int fd_{-1};
  mutable std::mutex fd_mutex_;
  std::mutex write_mutex_;
  std::atomic<bool> threads_running_{false};
  std::atomic<std::uint32_t> next_seq_{1};

  std::thread reader_;
  std::thread request_worker_;
  std::thread event_worker_;

  mutable std::mutex pending_mutex_;
  std::map<std::uint32_t, std::shared_ptr<PendingCall>> pending_;

  mutable std::mutex state_mutex_;
  std::condition_variable state_cv_;
  std::set<std::uint16_t> available_;

  mutable std::mutex handler_mutex_;
  std::map<std::uint16_t, std::shared_ptr<gen::IService>> services_;
  std::set<std::uint16_t> raw_services_;

  mutable std::mutex subscription_mutex_;
  std::map<std::pair<std::uint16_t, std::uint16_t>, NotifyCallback> subscriptions_;

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<IncomingRequest> requests_;
  std::deque<IncomingEvent> events_;
  std::size_t event_queue_limit_{16384};
  std::uint64_t event_overflow_count_{0};
};

// Registers an IService with the broker, dispatches method requests and
// publishes events.
class ServiceHost {
 public:
  ServiceHost(std::shared_ptr<ServiceClient> client,
              std::shared_ptr<gen::IService> service);
  ~ServiceHost();

  ServiceHost(const ServiceHost&) = delete;
  ServiceHost& operator=(const ServiceHost&) = delete;

  Result<void> start();
  void stop();
  Result<void> publish(std::uint16_t event_id, const Bytes& payload);
  std::uint16_t service_id() const { return service_id_; }
  const char* service_name() const { return service_ ? service_->service_name() : ""; }

 private:
  std::shared_ptr<ServiceClient> client_;
  std::shared_ptr<gen::IService> service_;
  std::uint16_t service_id_{0};
  bool started_{false};
};

} // namespace cabin
