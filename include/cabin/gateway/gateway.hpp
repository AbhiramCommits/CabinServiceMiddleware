#pragma once

#include <functional>
#include <memory>
#include <string>

#include "cabin/client.hpp"
#include "cabin/gateway/http_server.hpp"
#include "cabin/result.hpp"
#include "cabin_services.hpp"
#include "cabin_types.hpp"

namespace cabin::gateway {

class GatewayBackend {
 public:
  virtual ~GatewayBackend() = default;
  virtual Result<gen::SignalSnapshot> signals() = 0;
  virtual Result<gen::GetProfileResponse> get_profile(const std::string& driver_id) = 0;
  virtual Result<gen::PutProfileResponse> put_profile(const gen::Profile& profile) = 0;
  virtual Result<gen::ApplyProfileResponse> apply_profile(
      const std::string& driver_id) = 0;
};

class RpcGatewayBackend final : public GatewayBackend {
 public:
  explicit RpcGatewayBackend(std::shared_ptr<ServiceClient> client,
                             int timeout_ms = 3000);
  Result<gen::SignalSnapshot> signals() override;
  Result<gen::GetProfileResponse> get_profile(const std::string& driver_id) override;
  Result<gen::PutProfileResponse> put_profile(const gen::Profile& profile) override;
  Result<gen::ApplyProfileResponse> apply_profile(const std::string& driver_id) override;

 private:
  std::shared_ptr<ServiceClient> client_;
  gen::SignalServiceClient signals_;
  gen::PersonalizationServiceClient personalization_;
};

class GatewayApp {
 public:
  explicit GatewayApp(GatewayBackend& backend) : backend_(backend) {}
  HttpResponse handle(const HttpRequest& request);

 private:
  GatewayBackend& backend_;
};

} // namespace cabin::gateway
