#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cabin/frame.hpp"
#include "cabin/result.hpp"
#include "cabin/runtime.hpp"

namespace cabin {

struct BrokerConfig {
  std::string runtime_dir{default_runtime_dir()};
  std::size_t max_clients{64};
  std::size_t send_queue_limit{1024};
  int poll_timeout_ms{100};
  bool unlink_socket_on_stop{true};
};

// cabind: Unix-domain-socket broker with SOME/IP-style discovery, pub/sub and
// request/response routing. One Broker instance owns one listening socket.
//
// Thread model: start() creates the socket; run()/run_async() drive a single
// poll(2) loop. All client state is only touched by that loop.
class Broker {
 public:
  explicit Broker(BrokerConfig config);
  ~Broker();

  Broker(const Broker&) = delete;
  Broker& operator=(const Broker&) = delete;

  Result<void> start();
  void run();
  void run_async();
  void stop();

  // Async-signal-safe: only sets an atomic and writes one byte to a pipe.
  void request_stop();

  const std::string& socket_path() const { return socket_path_; }
  const std::string& runtime_dir() const { return config_.runtime_dir; }
  bool running() const { return running_.load(); }

  // Statistics, readable from any thread after stop (or racily while running).
  std::size_t connected_clients() const { return client_count_.load(); }
  std::uint64_t dropped_messages() const { return dropped_messages_.load(); }

 private:
  struct Outgoing {
    MsgType type{MsgType::kNotify};
    Bytes data;
    std::size_t sent{0};
  };
  struct Client {
    int fd{-1};
    std::uint64_t id{0};
    FrameDecoder decoder;
    std::deque<Outgoing> outbox;
    std::set<std::uint16_t> services;
    std::set<std::pair<std::uint16_t, std::uint16_t>> subscriptions;
  };
  struct Route {
    std::uint64_t caller{0};
    std::uint32_t original_seq{0};
    std::uint64_t provider{0};
  };

  void loop();
  void cleanup();
  void accept_clients();
  void read_client(Client& client);
  void flush_client(Client& client);
  void close_client(std::uint64_t id, const char* reason);
  void handle_frame(Client& client, Frame frame);
  void handle_offer(Client& client, const Frame& frame);
  void handle_find(Client& client, const Frame& frame);
  void handle_request(Client& client, Frame frame);
  void handle_response(Client& client, const Frame& frame);
  void handle_notify(Client& client, const Frame& frame);
  void broadcast(std::uint16_t service_id, MsgType type, const Bytes& payload);
  void send_to(Client& client, MsgType type, std::uint16_t service_id,
               std::uint16_t method_id, std::uint32_t seq, std::uint64_t trace_id,
               const Bytes& payload);
  void enqueue(Client& client, Outgoing outgoing);
  Client* find_client(std::uint64_t id);
  void reap_clients();
  void fail_calls_for(std::uint64_t client_id, const Error& error);

  BrokerConfig config_;
  std::string socket_path_;
  int listen_fd_{-1};
  int wake_read_{-1};
  int wake_write_{-1};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> client_count_{0};
  std::atomic<std::uint64_t> dropped_messages_{0};
  std::uint64_t next_client_id_{1};
  std::uint32_t next_route_seq_{1};
  std::vector<std::unique_ptr<Client>> clients_;
  std::map<std::uint16_t, std::uint64_t> service_providers_;
  std::map<std::uint32_t, Route> routes_;
  std::thread thread_;
};

} // namespace cabin
