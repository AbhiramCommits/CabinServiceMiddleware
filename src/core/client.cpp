#include "cabin/client.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include "cabin/log.hpp"
#include "cabin/trace.hpp"

namespace cabin {
namespace {

#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif

void sleep_ms(int milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

std::uint64_t current_or_new_trace() {
  const TraceId trace = current_trace_id();
  return trace != 0 ? trace : new_trace_id();
}

} // namespace

ServiceClient::ServiceClient(ServiceClientOptions options)
    : options_(std::move(options)) {
  if (options_.runtime_dir.empty())
    options_.runtime_dir = default_runtime_dir();
}

ServiceClient::~ServiceClient() {
  close();
}

Result<std::shared_ptr<ServiceClient>> ServiceClient::connect(
    const ServiceClientOptions& options) {
  auto client = std::shared_ptr<ServiceClient>(new ServiceClient(options));
  auto status = client->open_socket();
  if (!status)
    return status.error();
  status = client->start_threads();
  if (!status) {
    client->close();
    return status.error();
  }
  return client;
}

Result<void> ServiceClient::open_socket() {
  const std::string path = broker_socket_path(options_.runtime_dir);
  sockaddr_un address{};
  if (path.size() >= sizeof(address.sun_path)) {
    return make_error(ErrorCode::kInvalidArgument, "socket path too long");
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(options_.connect_retry_ms);
  int fd = -1;
  while (true) {
    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return make_error(ErrorCode::kIo, std::string("socket: ") + std::strerror(errno));
    }
#ifdef SO_NOSIGPIPE
    int enable = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
#endif
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      break;
    }
    const int saved = errno;
    ::close(fd);
    fd = -1;
    if ((saved == ENOENT || saved == ECONNREFUSED || saved == EAGAIN) &&
        std::chrono::steady_clock::now() < deadline) {
      sleep_ms(5);
      continue;
    }
    return make_error(ErrorCode::kUnavailable,
                      "connect " + path + " failed: " + std::strerror(saved));
  }
  struct timeval timeout{};
  timeout.tv_sec = 2;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  std::lock_guard<std::mutex> lock(fd_mutex_);
  fd_ = fd;
  return ok_status();
}

Result<void> ServiceClient::start_threads() {
  if (threads_running_.exchange(true))
    return ok_status();
  reader_ = std::thread([this] { reader_loop(); });
  request_worker_ = std::thread([this] { request_loop(); });
  event_worker_ = std::thread([this] { event_loop(); });
  return ok_status();
}

void ServiceClient::stop_threads() {
  const bool was_running = threads_running_.exchange(false);
  if (!was_running && !reader_.joinable())
    return;
  if (fd_ >= 0)
    ::shutdown(fd_, SHUT_RDWR);
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_cv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_cv_.notify_all();
  }
  fail_pending(make_error(ErrorCode::kUnavailable, "client disconnected"));
  if (reader_.joinable())
    reader_.join();
  if (request_worker_.joinable())
    request_worker_.join();
  if (event_worker_.joinable())
    event_worker_.join();
}

void ServiceClient::close() {
  if (fd_ < 0 && !threads_running_.load() && !reader_.joinable())
    return;
  stop_threads();
  std::lock_guard<std::mutex> lock(fd_mutex_);
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

Result<void> ServiceClient::reconnect() {
  stop_threads();
  {
    std::lock_guard<std::mutex> lock(fd_mutex_);
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    available_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    requests_.clear();
    events_.clear();
  }
  auto status = open_socket();
  if (!status)
    return status;
  status = start_threads();
  if (!status)
    return status;

  std::vector<std::uint16_t> services;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    for (const auto& entry : services_)
      services.push_back(entry.first);
    for (std::uint16_t id : raw_services_)
      services.push_back(id);
  }
  for (std::uint16_t id : services) {
    auto offered = offer_frame(id);
    if (!offered)
      return offered;
  }
  std::vector<std::pair<std::uint16_t, std::uint16_t>> subscriptions;
  {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    for (const auto& entry : subscriptions_)
      subscriptions.push_back(entry.first);
  }
  for (const auto& entry : subscriptions) {
    Frame frame;
    frame.msg_type = MsgType::kSubscribe;
    frame.service_id = entry.first;
    frame.method_id = entry.second;
    frame.trace_id = current_or_new_trace();
    auto sent = send_frame(frame);
    if (!sent)
      return sent;
  }
  log_info("client", "reconnected", {Field("socket", socket_path())});
  return ok_status();
}

std::string ServiceClient::socket_path() const {
  return broker_socket_path(options_.runtime_dir);
}

Result<void> ServiceClient::send_frame(const Frame& frame) {
  auto encoded = encode_frame(frame);
  if (!encoded)
    return encoded.error();
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (fd_ < 0) {
    return make_error(ErrorCode::kUnavailable, "client socket is closed");
  }
  std::size_t sent = 0;
  while (sent < encoded.value().size()) {
    const ssize_t result = ::send(fd_, encoded.value().data() + sent,
                                  encoded.value().size() - sent, MSG_NOSIGNAL);
    if (result > 0) {
      sent += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR)
      continue;
    return make_error(ErrorCode::kIo,
                      std::string("send failed: ") + std::strerror(errno));
  }
  return ok_status();
}

Result<void> ServiceClient::find(std::uint16_t service_id, int timeout_ms) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (available_.count(service_id) != 0)
      return ok_status();
  }
  Frame frame;
  frame.msg_type = MsgType::kFindService;
  frame.service_id = service_id;
  frame.trace_id = current_or_new_trace();
  auto sent = send_frame(frame);
  if (!sent)
    return sent;

  std::unique_lock<std::mutex> lock(state_mutex_);
  const bool found = state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
    return available_.count(service_id) != 0 || !threads_running_.load();
  });
  if (!threads_running_.load()) {
    return make_error(ErrorCode::kUnavailable, "client is not running");
  }
  if (!found) {
    return make_error(ErrorCode::kTimeout,
                      "service " + std::to_string(service_id) + " not discovered");
  }
  return ok_status();
}

bool ServiceClient::available(std::uint16_t service_id) const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return available_.count(service_id) != 0;
}

Result<void> ServiceClient::subscribe(std::uint16_t service_id, std::uint16_t event_id,
                                      NotifyCallback callback) {
  {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_[{service_id, event_id}] = std::move(callback);
  }
  Frame frame;
  frame.msg_type = MsgType::kSubscribe;
  frame.service_id = service_id;
  frame.method_id = event_id;
  frame.trace_id = current_or_new_trace();
  auto sent = send_frame(frame);
  if (!sent) {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.erase({service_id, event_id});
  }
  return sent;
}

Result<void> ServiceClient::unsubscribe(std::uint16_t service_id,
                                        std::uint16_t event_id) {
  {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.erase({service_id, event_id});
  }
  Frame frame;
  frame.msg_type = MsgType::kUnsubscribe;
  frame.service_id = service_id;
  frame.method_id = event_id;
  return send_frame(frame);
}

Result<Bytes> ServiceClient::call(std::uint16_t service_id, std::uint16_t method_id,
                                  const Bytes& request, int timeout_ms) {
  if (!threads_running_.load()) {
    return make_error(ErrorCode::kUnavailable, "client is not running");
  }
  std::uint32_t seq = next_seq_.fetch_add(1);
  if (seq == 0)
    seq = next_seq_.fetch_add(1);

  auto pending = std::make_shared<PendingCall>();
  auto future = pending->promise.get_future();
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_[seq] = pending;
  }

  Frame frame;
  frame.msg_type = MsgType::kRequest;
  frame.service_id = service_id;
  frame.method_id = method_id;
  frame.seq = seq;
  frame.trace_id = current_or_new_trace();
  frame.payload = request;
  auto sent = send_frame(frame);
  if (!sent) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.erase(seq);
    return sent.error();
  }

  if (future.wait_for(std::chrono::milliseconds(timeout_ms)) !=
      std::future_status::ready) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.erase(seq);
    return make_error(ErrorCode::kTimeout,
                      "rpc timeout for service " + std::to_string(service_id));
  }
  return future.get();
}

Result<void> ServiceClient::offer_frame(std::uint16_t service_id) {
  Frame frame;
  frame.msg_type = MsgType::kOfferService;
  frame.service_id = service_id;
  frame.trace_id = current_or_new_trace();
  return send_frame(frame);
}

Result<void> ServiceClient::offer(const std::shared_ptr<gen::IService>& service) {
  if (!service)
    return make_error(ErrorCode::kInvalidArgument, "null service");
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    services_[service->service_id()] = service;
  }
  auto status = offer_frame(service->service_id());
  if (!status) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    services_.erase(service->service_id());
  }
  return status;
}

Result<void> ServiceClient::offer_raw(std::uint16_t service_id) {
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    raw_services_.insert(service_id);
  }
  return offer_frame(service_id);
}

Result<void> ServiceClient::withdraw(std::uint16_t service_id) {
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    services_.erase(service_id);
    raw_services_.erase(service_id);
  }
  Frame frame;
  frame.msg_type = MsgType::kServiceUnavailable;
  frame.service_id = service_id;
  return send_frame(frame);
}

Result<void> ServiceClient::publish(std::uint16_t service_id, std::uint16_t event_id,
                                    const Bytes& payload) {
  Frame frame;
  frame.msg_type = MsgType::kNotify;
  frame.service_id = service_id;
  frame.method_id = event_id;
  frame.trace_id = current_or_new_trace();
  frame.payload = payload;
  return send_frame(frame);
}

void ServiceClient::send_error(std::uint16_t service_id, std::uint16_t method_id,
                               std::uint32_t seq, std::uint64_t trace_id,
                               const Error& error) {
  Frame frame;
  frame.msg_type = MsgType::kError;
  frame.service_id = service_id;
  frame.method_id = method_id;
  frame.seq = seq;
  frame.trace_id = trace_id;
  frame.payload = encode_error_payload(error);
  auto status = send_frame(frame);
  if (!status) {
    log_warn("client", "send_error_failed", {Field("message", status.error().message)});
  }
}

void ServiceClient::handle_frame(Frame frame) {
  switch (frame.msg_type) {
    case MsgType::kResponse: {
      std::shared_ptr<PendingCall> pending;
      {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_.find(frame.seq);
        if (it == pending_.end())
          return;
        pending = it->second;
        pending_.erase(it);
      }
      pending->promise.set_value(std::move(frame.payload));
      break;
    }
    case MsgType::kError: {
      std::shared_ptr<PendingCall> pending;
      {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_.find(frame.seq);
        if (it == pending_.end())
          return;
        pending = it->second;
        pending_.erase(it);
      }
      pending->promise.set_value(decode_error_payload(frame.payload));
      break;
    }
    case MsgType::kServiceAvailable: {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        available_.insert(frame.service_id);
      }
      state_cv_.notify_all();
      log_debug("client", "service_available",
                {Field("service_id", static_cast<std::int64_t>(frame.service_id))});
      break;
    }
    case MsgType::kServiceUnavailable: {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        available_.erase(frame.service_id);
      }
      state_cv_.notify_all();
      log_debug("client", "service_unavailable",
                {Field("service_id", static_cast<std::int64_t>(frame.service_id))});
      break;
    }
    case MsgType::kNotify: {
      bool overflowed = false;
      std::uint64_t overflow_count = 0;
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (events_.size() >= event_queue_limit_) {
          events_.pop_front();
          overflowed = true;
          overflow_count = ++event_overflow_count_;
        }
        events_.push_back(
            IncomingEvent{frame.service_id, frame.method_id, std::move(frame.payload)});
        queue_cv_.notify_all();
      }
      if (overflowed && (overflow_count % 1024 == 1)) {
        log_warn("client", "event_queue_overflow",
                 {Field("service_id", static_cast<std::int64_t>(frame.service_id)),
                  Field("dropped_total", static_cast<std::int64_t>(overflow_count))});
      }
      break;
    }
    case MsgType::kRequest: {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      requests_.push_back(IncomingRequest{std::move(frame)});
      queue_cv_.notify_all();
      break;
    }
    case MsgType::kPong:
      break;
    default:
      log_warn("client", "unexpected_message_type",
               {Field("msg_type", msg_type_name(frame.msg_type))});
      break;
  }
}

void ServiceClient::reader_loop() {
  FrameDecoder decoder;
  while (threads_running_.load()) {
    pollfd entry{};
    entry.fd = fd_;
    entry.events = POLLIN;
    const int ready = ::poll(&entry, 1, 50);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (ready == 0)
      continue;
    if ((entry.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
        (entry.revents & POLLIN) == 0) {
      break;
    }
    std::uint8_t buffer[8192];
    const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
    if (received > 0) {
      decoder.feed(buffer, static_cast<std::size_t>(received));
      while (true) {
        auto frame = decoder.next();
        if (!frame) {
          log_warn("client", "frame_decode_error",
                   {Field("message", frame.error().message)});
          fail_pending(make_error(ErrorCode::kParse, "broker sent a malformed frame"));
          return;
        }
        if (!frame.value().has_value())
          break;
        handle_frame(std::move(frame.value().value()));
      }
      continue;
    }
    if (received == 0)
      break;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      continue;
    if (errno == EINTR)
      continue;
    break;
  }
  fail_pending(make_error(ErrorCode::kUnavailable, "broker connection closed"));
}

void ServiceClient::request_loop() {
  while (true) {
    IncomingRequest request;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock,
                     [&] { return !threads_running_.load() || !requests_.empty(); });
      if (!threads_running_.load())
        return;
      request = std::move(requests_.front());
      requests_.pop_front();
    }
    std::shared_ptr<gen::IService> service;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      auto it = services_.find(request.frame.service_id);
      if (it != services_.end())
        service = it->second;
    }
    if (!service) {
      send_error(request.frame.service_id, request.frame.method_id, request.frame.seq,
                 request.frame.trace_id,
                 make_error(ErrorCode::kNotFound, "service not hosted"));
      continue;
    }
    const TraceId previous = current_trace_id();
    set_current_trace_id(request.frame.trace_id);
    auto result = service->dispatch(request.frame.method_id, request.frame.payload);
    Frame response;
    response.service_id = request.frame.service_id;
    response.method_id = request.frame.method_id;
    response.seq = request.frame.seq;
    response.trace_id = request.frame.trace_id;
    if (result) {
      response.msg_type = MsgType::kResponse;
      response.payload = std::move(result.value());
    } else {
      response.msg_type = MsgType::kError;
      response.payload = encode_error_payload(result.error());
    }
    auto sent = send_frame(response);
    if (!sent) {
      log_warn("client", "response_send_failed",
               {Field("message", sent.error().message)});
    }
    set_current_trace_id(previous);
  }
}

void ServiceClient::event_loop() {
  while (true) {
    IncomingEvent event;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [&] { return !threads_running_.load() || !events_.empty(); });
      if (!threads_running_.load())
        return;
      event = std::move(events_.front());
      events_.pop_front();
    }
    NotifyCallback callback;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      auto it = subscriptions_.find({event.service_id, event.event_id});
      if (it != subscriptions_.end())
        callback = it->second;
    }
    if (callback)
      callback(event.service_id, event.event_id, event.payload);
  }
}

void ServiceClient::fail_pending(const Error& error) {
  std::map<std::uint32_t, std::shared_ptr<PendingCall>> pending;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending.swap(pending_);
  }
  for (auto& entry : pending) {
    entry.second->promise.set_value(error);
  }
}

ServiceHost::ServiceHost(std::shared_ptr<ServiceClient> client,
                         std::shared_ptr<gen::IService> service)
    : client_(std::move(client)), service_(std::move(service)) {
  if (service_)
    service_id_ = service_->service_id();
}

ServiceHost::~ServiceHost() {
  stop();
}

Result<void> ServiceHost::start() {
  if (!client_ || !service_) {
    return make_error(ErrorCode::kInvalidArgument, "host is not configured");
  }
  auto status = client_->offer(service_);
  if (status)
    started_ = true;
  return status;
}

void ServiceHost::stop() {
  if (started_ && client_ && service_) {
    auto status = client_->withdraw(service_id_);
    if (!status) {
      log_warn("client", "withdraw_failed",
               {Field("service_id", static_cast<std::int64_t>(service_id_))});
    }
  }
  started_ = false;
}

Result<void> ServiceHost::publish(std::uint16_t event_id, const Bytes& payload) {
  if (!client_ || !started_) {
    return make_error(ErrorCode::kUnavailable, "host is not started");
  }
  return client_->publish(service_id_, event_id, payload);
}

} // namespace cabin
