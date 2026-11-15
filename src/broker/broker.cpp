#include "cabin/broker.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "cabin/log.hpp"
#include "cabin/trace.hpp"

namespace cabin {
namespace {

int set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return -1;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif

} // namespace

Broker::Broker(BrokerConfig config) : config_(std::move(config)) {
  if (config_.runtime_dir.empty())
    config_.runtime_dir = default_runtime_dir();
  if (config_.send_queue_limit == 0)
    config_.send_queue_limit = 1;
  socket_path_ = broker_socket_path(config_.runtime_dir);
}

Broker::~Broker() {
  stop();
  if (thread_.joinable())
    thread_.join();
  cleanup();
}

Result<void> Broker::start() {
  auto status = ensure_runtime_dir(config_.runtime_dir);
  if (!status)
    return status;
  sockaddr_un address{};
  if (socket_path_.size() >= sizeof(address.sun_path)) {
    return make_error(ErrorCode::kInvalidArgument,
                      "runtime dir is too long for a Unix socket path");
  }

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return make_error(ErrorCode::kIo, std::string("socket: ") + std::strerror(errno));
  }
  ::unlink(socket_path_.c_str());
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string message =
        std::string("bind ") + socket_path_ + ": " + std::strerror(errno);
    close_fd(listen_fd_);
    return make_error(ErrorCode::kIo, message);
  }
  if (::listen(listen_fd_, 64) != 0) {
    const std::string message = std::string("listen: ") + std::strerror(errno);
    close_fd(listen_fd_);
    ::unlink(socket_path_.c_str());
    return make_error(ErrorCode::kIo, message);
  }
  set_nonblocking(listen_fd_);

  int wake[2] = {-1, -1};
  if (::pipe(wake) != 0) {
    close_fd(listen_fd_);
    ::unlink(socket_path_.c_str());
    return make_error(ErrorCode::kIo, std::string("pipe: ") + std::strerror(errno));
  }
  wake_read_ = wake[0];
  wake_write_ = wake[1];
  set_nonblocking(wake_read_);
  set_nonblocking(wake_write_);

  running_.store(true);
  stopping_.store(false);
  log_info("cabind", "broker_start",
           {Field("socket", socket_path_),
            Field("queue_limit", static_cast<std::int64_t>(config_.send_queue_limit))});
  return ok_status();
}

void Broker::run() {
  if (!running_.load()) {
    log_warn("cabind", "run_without_start");
    return;
  }
  loop();
  cleanup();
}

void Broker::run_async() {
  thread_ = std::thread([this] {
    loop();
    cleanup();
  });
}

void Broker::stop() {
  stopping_.store(true);
  running_.store(false);
  if (wake_write_ >= 0) {
    const std::uint8_t byte = 1;
    const ssize_t ignored = ::write(wake_write_, &byte, 1);
    (void)ignored;
  }
}

void Broker::request_stop() {
  stop();
}

void Broker::loop() {
  std::vector<pollfd> fds;
  while (running_.load()) {
    fds.clear();
    fds.push_back(pollfd{listen_fd_, POLLIN, 0});
    fds.push_back(pollfd{wake_read_, POLLIN, 0});
    std::vector<std::uint64_t> ids;
    ids.reserve(clients_.size());
    for (const auto& client : clients_) {
      if (client->fd < 0)
        continue;
      short events = POLLIN;
      if (!client->outbox.empty())
        events |= POLLOUT;
      fds.push_back(pollfd{client->fd, events, 0});
      ids.push_back(client->id);
    }

    const int ready = ::poll(fds.data(), fds.size(), config_.poll_timeout_ms);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      log_error("cabind", "poll_failed", {Field("errno", errno)});
      break;
    }
    if (ready == 0) {
      reap_clients();
      continue;
    }

    if ((fds[1].revents & POLLIN) != 0) {
      std::uint8_t drain[64];
      while (::read(wake_read_, drain, sizeof(drain)) > 0) {
      }
      if (stopping_.load())
        break;
    }
    if ((fds[0].revents & POLLIN) != 0)
      accept_clients();

    for (std::size_t index = 0; index < ids.size(); ++index) {
      const pollfd& entry = fds[index + 2];
      if (entry.revents == 0)
        continue;
      Client* client = find_client(ids[index]);
      if (client == nullptr || client->fd < 0)
        continue;
      if ((entry.revents & POLLOUT) != 0)
        flush_client(*client);
      if (client->fd >= 0 && (entry.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        read_client(*client);
      }
    }
    reap_clients();
    std::size_t count = 0;
    for (const auto& client : clients_) {
      if (client->fd >= 0)
        ++count;
    }
    client_count_.store(count);
  }
  running_.store(false);
}

void Broker::cleanup() {
  const bool had_listener = listen_fd_ >= 0;
  for (auto& client : clients_) {
    if (client->fd >= 0) {
      ::close(client->fd);
      client->fd = -1;
    }
  }
  clients_.clear();
  client_count_.store(0);
  close_fd(listen_fd_);
  close_fd(wake_read_);
  close_fd(wake_write_);
  if (config_.unlink_socket_on_stop && had_listener) {
    ::unlink(socket_path_.c_str());
  }
  if (had_listener) {
    log_info("cabind", "broker_stop", {Field("socket", socket_path_)});
  }
}

void Broker::accept_clients() {
  while (true) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      if (errno == EINTR)
        continue;
      log_error("cabind", "accept_failed", {Field("errno", errno)});
      return;
    }
    if (clients_.size() >= config_.max_clients) {
      log_warn("cabind", "max_clients_reached",
               {Field("max_clients", static_cast<std::int64_t>(config_.max_clients))});
      ::close(fd);
      continue;
    }
    set_nonblocking(fd);
    auto client = std::make_unique<Client>();
    client->fd = fd;
    client->id = next_client_id_++;
    log_info("cabind", "client_connect",
             {Field("client_id", static_cast<std::int64_t>(client->id))});
    clients_.push_back(std::move(client));
    client_count_.store(clients_.size());
  }
}

void Broker::read_client(Client& client) {
  std::uint8_t buffer[4096];
  while (client.fd >= 0) {
    const ssize_t received = ::recv(client.fd, buffer, sizeof(buffer), 0);
    if (received > 0) {
      client.decoder.feed(buffer, static_cast<std::size_t>(received));
      while (client.fd >= 0) {
        auto frame = client.decoder.next();
        if (!frame) {
          log_warn("cabind", "frame_decode_error",
                   {Field("client_id", static_cast<std::int64_t>(client.id)),
                    Field("error_code", static_cast<std::int64_t>(frame.code())),
                    Field("message", frame.error().message)});
          close_client(client.id, "decode error");
          return;
        }
        if (!frame.value().has_value())
          break;
        handle_frame(client, std::move(frame.value().value()));
      }
      continue;
    }
    if (received == 0) {
      close_client(client.id, "peer closed");
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    if (errno == EINTR)
      continue;
    close_client(client.id, std::strerror(errno));
    return;
  }
}

void Broker::flush_client(Client& client) {
  while (!client.outbox.empty() && client.fd >= 0) {
    Outgoing& outgoing = client.outbox.front();
    const std::size_t remaining = outgoing.data.size() - outgoing.sent;
    const ssize_t sent =
        ::send(client.fd, outgoing.data.data() + outgoing.sent, remaining, MSG_NOSIGNAL);
    if (sent > 0) {
      outgoing.sent += static_cast<std::size_t>(sent);
      if (outgoing.sent >= outgoing.data.size()) {
        client.outbox.pop_front();
      }
      continue;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;
    if (sent < 0 && errno == EINTR)
      continue;
    close_client(client.id, "send failed");
    return;
  }
}

void Broker::enqueue(Client& client, Outgoing outgoing) {
  if (client.fd < 0)
    return;
  if (client.outbox.size() >= config_.send_queue_limit) {
    auto victim =
        std::find_if(client.outbox.begin(), client.outbox.end(),
                     [](const Outgoing& item) { return item.type == MsgType::kNotify; });
    MsgType dropped = outgoing.type;
    if (victim != client.outbox.end()) {
      dropped = victim->type;
      client.outbox.erase(victim);
    } else {
      dropped = client.outbox.front().type;
      client.outbox.pop_front();
    }
    dropped_messages_.fetch_add(1);
    log_warn("cabind", "queue_overflow",
             {Field("client_id", static_cast<std::int64_t>(client.id)),
              Field("dropped_type", msg_type_name(dropped)),
              Field("queue_limit", static_cast<std::int64_t>(config_.send_queue_limit))});
  }
  client.outbox.push_back(std::move(outgoing));
  flush_client(client);
}

void Broker::close_client(std::uint64_t id, const char* reason) {
  Client* client = find_client(id);
  if (client == nullptr || client->fd < 0)
    return;
  ::close(client->fd);
  client->fd = -1;
  log_info("cabind", "client_disconnect",
           {Field("client_id", static_cast<std::int64_t>(id)), Field("reason", reason)});

  for (std::uint16_t service_id : client->services) {
    auto it = service_providers_.find(service_id);
    if (it != service_providers_.end() && it->second == id) {
      service_providers_.erase(it);
      broadcast(service_id, MsgType::kServiceUnavailable, Bytes());
      log_info("cabind", "service_unavailable",
               {Field("service_id", static_cast<std::int64_t>(service_id))});
    }
  }
  fail_calls_for(id, make_error(ErrorCode::kUnavailable, "peer disconnected"));
  client->outbox.clear();
  client->services.clear();
  client->subscriptions.clear();
  client_count_.store(0);
}

void Broker::reap_clients() {
  clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                [](const std::unique_ptr<Client>& client) {
                                  return client->fd < 0;
                                }),
                 clients_.end());
  std::size_t count = 0;
  for (const auto& client : clients_) {
    if (client->fd >= 0)
      ++count;
  }
  client_count_.store(count);
}

void Broker::handle_frame(Client& client, Frame frame) {
  switch (frame.msg_type) {
    case MsgType::kOfferService:
      handle_offer(client, frame);
      break;
    case MsgType::kFindService:
      handle_find(client, frame);
      break;
    case MsgType::kRequest:
      handle_request(client, std::move(frame));
      break;
    case MsgType::kResponse:
    case MsgType::kError:
      handle_response(client, frame);
      break;
    case MsgType::kNotify:
      handle_notify(client, frame);
      break;
    case MsgType::kSubscribe:
      client.subscriptions.emplace(frame.service_id, frame.method_id);
      send_to(client, MsgType::kResponse, frame.service_id, frame.method_id, frame.seq,
              frame.trace_id, Bytes());
      break;
    case MsgType::kUnsubscribe:
      client.subscriptions.erase({frame.service_id, frame.method_id});
      send_to(client, MsgType::kResponse, frame.service_id, frame.method_id, frame.seq,
              frame.trace_id, Bytes());
      break;
    case MsgType::kServiceUnavailable: {
      auto it = service_providers_.find(frame.service_id);
      if (it != service_providers_.end() && it->second == client.id) {
        service_providers_.erase(it);
        client.services.erase(frame.service_id);
        broadcast(frame.service_id, MsgType::kServiceUnavailable, Bytes());
        log_info("cabind", "service_withdrawn",
                 {Field("service_id", static_cast<std::int64_t>(frame.service_id))});
      }
      break;
    }
    case MsgType::kPing:
      send_to(client, MsgType::kPong, 0, 0, frame.seq, frame.trace_id, Bytes());
      break;
    default:
      send_to(client, MsgType::kError, frame.service_id, frame.method_id, frame.seq,
              frame.trace_id,
              encode_error_payload(
                  make_error(ErrorCode::kUnsupported, "unsupported message type")));
      break;
  }
}

void Broker::handle_offer(Client& client, const Frame& frame) {
  client.services.insert(frame.service_id);
  service_providers_[frame.service_id] = client.id;
  Bytes endpoint;
  endpoint.append_string(socket_path_);
  broadcast(frame.service_id, MsgType::kServiceAvailable, endpoint);
  log_info("cabind", "service_offered",
           {Field("service_id", static_cast<std::int64_t>(frame.service_id)),
            Field("client_id", static_cast<std::int64_t>(client.id)),
            Field("trace_id", trace_id_hex(frame.trace_id))});
}

void Broker::handle_find(Client& client, const Frame& frame) {
  auto it = service_providers_.find(frame.service_id);
  log_debug("cabind", "find_service",
            {Field("service_id", static_cast<std::int64_t>(frame.service_id)),
             Field("found", it != service_providers_.end())});
  if (it == service_providers_.end())
    return;
  Bytes endpoint;
  endpoint.append_string(socket_path_);
  send_to(client, MsgType::kServiceAvailable, frame.service_id, 0, frame.seq,
          frame.trace_id, endpoint);
}

void Broker::handle_request(Client& client, Frame frame) {
  auto provider_it = service_providers_.find(frame.service_id);
  if (provider_it == service_providers_.end()) {
    send_to(client, MsgType::kError, frame.service_id, frame.method_id, frame.seq,
            frame.trace_id,
            encode_error_payload(make_error(
                ErrorCode::kNotFound,
                "no provider for service " + std::to_string(frame.service_id))));
    return;
  }
  Client* provider = find_client(provider_it->second);
  if (provider == nullptr || provider->fd < 0) {
    send_to(
        client, MsgType::kError, frame.service_id, frame.method_id, frame.seq,
        frame.trace_id,
        encode_error_payload(make_error(ErrorCode::kUnavailable, "provider is gone")));
    return;
  }
  std::uint32_t routed_seq = next_route_seq_++;
  if (next_route_seq_ == 0)
    next_route_seq_ = 1;
  routes_[routed_seq] = Route{client.id, frame.seq, provider->id};
  send_to(*provider, MsgType::kRequest, frame.service_id, frame.method_id, routed_seq,
          frame.trace_id, frame.payload);
}

void Broker::handle_response(Client& client, const Frame& frame) {
  auto it = routes_.find(frame.seq);
  if (it == routes_.end())
    return;
  if (it->second.provider != client.id) {
    log_warn("cabind", "response_from_wrong_provider",
             {Field("seq", static_cast<std::int64_t>(frame.seq)),
              Field("client_id", static_cast<std::int64_t>(client.id))});
    return;
  }
  Client* caller = find_client(it->second.caller);
  const std::uint32_t original_seq = it->second.original_seq;
  routes_.erase(it);
  if (caller == nullptr || caller->fd < 0)
    return;
  send_to(*caller, frame.msg_type, frame.service_id, frame.method_id, original_seq,
          frame.trace_id, frame.payload);
}

void Broker::handle_notify(Client& client, const Frame& frame) {
  if (client.services.count(frame.service_id) == 0) {
    log_warn("cabind", "notify_from_non_provider",
             {Field("service_id", static_cast<std::int64_t>(frame.service_id)),
              Field("client_id", static_cast<std::int64_t>(client.id))});
    return;
  }
  for (auto& target : clients_) {
    if (target->fd < 0)
      continue;
    if (target->subscriptions.count({frame.service_id, frame.method_id}) == 0)
      continue;
    send_to(*target, MsgType::kNotify, frame.service_id, frame.method_id, frame.seq,
            frame.trace_id, frame.payload);
  }
}

void Broker::broadcast(std::uint16_t service_id, MsgType type, const Bytes& payload) {
  for (auto& client : clients_) {
    if (client->fd < 0)
      continue;
    send_to(*client, type, service_id, 0, 0, 0, payload);
  }
}

void Broker::send_to(Client& client, MsgType type, std::uint16_t service_id,
                     std::uint16_t method_id, std::uint32_t seq, std::uint64_t trace_id,
                     const Bytes& payload) {
  if (client.fd < 0)
    return;
  Frame frame;
  frame.msg_type = type;
  frame.service_id = service_id;
  frame.method_id = method_id;
  frame.seq = seq;
  frame.trace_id = trace_id;
  frame.payload = payload;
  auto encoded = encode_frame(frame);
  if (!encoded) {
    log_error("cabind", "encode_frame_failed",
              {Field("message", encoded.error().message)});
    return;
  }
  enqueue(client, Outgoing{type, std::move(encoded.value()), 0});
}

Broker::Client* Broker::find_client(std::uint64_t id) {
  for (auto& client : clients_) {
    if (client->id == id)
      return client.get();
  }
  return nullptr;
}

void Broker::fail_calls_for(std::uint64_t client_id, const Error& error) {
  for (auto it = routes_.begin(); it != routes_.end();) {
    if (it->second.caller != client_id && it->second.provider != client_id) {
      ++it;
      continue;
    }
    if (it->second.caller != client_id) {
      Client* caller = find_client(it->second.caller);
      if (caller != nullptr && caller->fd >= 0) {
        send_to(*caller, MsgType::kError, 0, 0, it->second.original_seq, 0,
                encode_error_payload(error));
      }
    }
    it = routes_.erase(it);
  }
}

} // namespace cabin
