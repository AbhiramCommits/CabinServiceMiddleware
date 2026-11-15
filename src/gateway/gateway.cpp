#include "cabin/gateway/gateway.hpp"

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "cabin/clock.hpp"
#include "cabin/json.hpp"
#include "cabin/log.hpp"
#include "cabin/trace.hpp"

namespace cabin::gateway {
namespace {

std::string percent_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '%' && index + 2 < text.size()) {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      const int high = hex(text[index + 1]);
      const int low = hex(text[index + 2]);
      if (high >= 0 && low >= 0) {
        out.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        continue;
      }
    }
    out.push_back(text[index]);
  }
  return out;
}

std::vector<std::string> split_path(std::string_view path) {
  std::vector<std::string> segments;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string_view part = path.substr(
        start, slash == std::string_view::npos ? path.size() - start : slash - start);
    if (!part.empty())
      segments.emplace_back(part);
    if (slash == std::string_view::npos)
      break;
    start = slash + 1;
  }
  return segments;
}

HttpResponse json_response(int status, const JsonValue& body) {
  HttpResponse response;
  response.status = status;
  response.content_type = "application/json";
  response.body = body.dump(0) + "\n";
  return response;
}

HttpResponse error_response(const Error& error) {
  int status = 500;
  switch (error.code) {
    case ErrorCode::kNotFound:
      status = 404;
      break;
    case ErrorCode::kInvalidArgument:
    case ErrorCode::kOutOfRange:
    case ErrorCode::kParse:
      status = 400;
      break;
    case ErrorCode::kTimeout:
      status = 504;
      break;
    case ErrorCode::kUnavailable:
      status = 503;
      break;
    default:
      status = 500;
      break;
  }
  JsonValue root = JsonValue::object();
  JsonValue detail = JsonValue::object();
  detail.set("code", JsonValue::string(error_code_name(error.code)));
  detail.set("message", JsonValue::string(error.message));
  root.set("error", std::move(detail));
  return json_response(status, root);
}

HttpResponse method_not_allowed(const std::string& allow) {
  HttpResponse response = error_response(
      make_error(ErrorCode::kUnsupported, "method not allowed for this path"));
  response.status = 405;
  response.headers["Allow"] = allow;
  return response;
}

Result<double> required_number(const JsonValue& object, const char* name) {
  const JsonValue* value = object.find(name);
  if (value == nullptr || !value->is_number()) {
    return make_error(ErrorCode::kInvalidArgument,
                      std::string("missing or non-numeric field ") + name);
  }
  return value->number_value();
}

Result<gen::Profile> profile_from_body(const JsonValue& body,
                                       const std::string& driver_id) {
  if (!body.is_object()) {
    return make_error(ErrorCode::kInvalidArgument, "body must be a JSON object");
  }
  gen::Profile profile;
  profile.driver_id = driver_id;
  auto seat = required_number(body, "seat_position_mm");
  if (!seat)
    return seat.error();
  profile.seat_position_mm = static_cast<std::uint16_t>(seat.value());
  auto recline = required_number(body, "seat_recline_deg");
  if (!recline)
    return recline.error();
  profile.seat_recline_deg = static_cast<std::uint8_t>(recline.value());
  auto setpoint = required_number(body, "hvac_setpoint_c");
  if (!setpoint)
    return setpoint.error();
  profile.hvac_setpoint_c = static_cast<float>(setpoint.value());
  auto fan = required_number(body, "hvac_fan_level");
  if (!fan)
    return fan.error();
  profile.hvac_fan_level = static_cast<std::uint8_t>(fan.value());
  auto level = required_number(body, "light_level");
  if (!level)
    return level.error();
  profile.light_level = static_cast<std::uint8_t>(level.value());

  const JsonValue* color = body.find("light_color");
  if (color == nullptr || !color->is_string()) {
    return make_error(ErrorCode::kInvalidArgument,
                      "missing or non-string field light_color");
  }
  auto parsed_color = gen::light_color_from_string(color->string_value());
  if (!parsed_color.has_value()) {
    return make_error(ErrorCode::kInvalidArgument,
                      "light_color must be WARM, NEUTRAL or COOL");
  }
  profile.light_color = parsed_color.value();
  profile.schema_version = static_cast<std::uint16_t>(body.get_int("schema_version", 1));
  profile.preferred_language = body.get_string("preferred_language", "en");

  auto valid = gen::validate(profile);
  if (!valid)
    return valid.error();
  return profile;
}

JsonValue profile_to_json(const gen::Profile& profile) {
  JsonValue object = JsonValue::object();
  object.set("driver_id", JsonValue::string(profile.driver_id));
  object.set("seat_position_mm", JsonValue::number(profile.seat_position_mm));
  object.set("seat_recline_deg", JsonValue::number(profile.seat_recline_deg));
  object.set("hvac_setpoint_c", JsonValue::number(profile.hvac_setpoint_c));
  object.set("hvac_fan_level", JsonValue::number(profile.hvac_fan_level));
  object.set("light_level", JsonValue::number(profile.light_level));
  object.set("light_color", JsonValue::string(gen::to_string(profile.light_color)));
  object.set("schema_version", JsonValue::number(profile.schema_version));
  object.set("preferred_language", JsonValue::string(profile.preferred_language));
  return object;
}

JsonValue snapshot_to_json(const gen::SignalSnapshot& snapshot) {
  JsonValue object = JsonValue::object();
  object.set("seat_position_mm", JsonValue::number(snapshot.seat_position_mm));
  object.set("seat_recline_deg", JsonValue::number(snapshot.seat_recline_deg));
  object.set("hvac_setpoint_c", JsonValue::number(snapshot.hvac_setpoint_c));
  object.set("hvac_fan_level", JsonValue::number(snapshot.hvac_fan_level));
  object.set("cabin_light_level", JsonValue::number(snapshot.cabin_light_level));
  object.set("cabin_light_color",
             JsonValue::string(gen::to_string(snapshot.cabin_light_color)));
  object.set("door_state", JsonValue::string(gen::to_string(snapshot.door_state)));
  object.set("ignition_state",
             JsonValue::string(gen::to_string(snapshot.ignition_state)));
  object.set("timestamp_us",
             JsonValue::number(static_cast<double>(snapshot.timestamp_us)));
  return object;
}

} // namespace

RpcGatewayBackend::RpcGatewayBackend(std::shared_ptr<ServiceClient> client,
                                     int timeout_ms)
    : client_(std::move(client)),
      signals_(*client_, timeout_ms),
      personalization_(*client_, timeout_ms) {
  log_debug("gateway", "backend_ready", {Field("runtime_dir", client_->runtime_dir())});
}

Result<gen::SignalSnapshot> RpcGatewayBackend::signals() {
  return signals_.get_signals();
}

Result<gen::GetProfileResponse> RpcGatewayBackend::get_profile(
    const std::string& driver_id) {
  return personalization_.get_profile(gen::GetProfileRequest{driver_id});
}

Result<gen::PutProfileResponse> RpcGatewayBackend::put_profile(
    const gen::Profile& profile) {
  return personalization_.put_profile(gen::PutProfileRequest{profile});
}

Result<gen::ApplyProfileResponse> RpcGatewayBackend::apply_profile(
    const std::string& driver_id) {
  return personalization_.apply_profile(gen::ApplyProfileRequest{driver_id});
}

HttpResponse GatewayApp::handle(const HttpRequest& request) {
  TraceId trace = new_trace_id();
  const std::string incoming = request.header("X-Cabin-Trace-Id");
  if (!incoming.empty()) {
    TraceId parsed = 0;
    if (parse_trace_id(incoming, &parsed))
      trace = parsed;
  }
  ScopedSpan span("gateway", "http_request", trace);
  const std::uint64_t started = now_us();

  std::string path = request.path;
  const std::size_t query = path.find('?');
  if (query != std::string::npos)
    path.resize(query);
  const auto segments = split_path(path);

  HttpResponse response;
  if (segments.size() == 1 && segments[0] == "healthz") {
    if (request.method != "GET") {
      response = method_not_allowed("GET");
    } else {
      JsonValue body = JsonValue::object();
      body.set("status", JsonValue::string("ok"));
      body.set("idl_version", JsonValue::string(gen::kIdlVersionString));
      response = json_response(200, body);
    }
  } else if (segments.size() >= 2 && segments[0] == "v1" && segments[1] == "signals" &&
             segments.size() == 2) {
    if (request.method != "GET") {
      response = method_not_allowed("GET");
    } else {
      auto snapshot = backend_.signals();
      response = snapshot ? json_response(200, snapshot_to_json(snapshot.value()))
                          : error_response(snapshot.error());
    }
  } else if (segments.size() >= 2 && segments[0] == "v1" && segments[1] == "profiles") {
    if (segments.size() == 3) {
      const std::string driver_id = percent_decode(segments[2]);
      if (request.method == "GET") {
        auto profile = backend_.get_profile(driver_id);
        if (!profile) {
          response = error_response(profile.error());
        } else if (!profile.value().found) {
          response = error_response(
              make_error(ErrorCode::kNotFound, "no profile for " + driver_id));
        } else {
          response = json_response(200, profile_to_json(profile.value().profile));
        }
      } else if (request.method == "PUT") {
        auto parsed = parse_json(request.body);
        if (!parsed) {
          response = error_response(parsed.error());
        } else {
          auto profile = profile_from_body(parsed.value(), driver_id);
          if (!profile) {
            response = error_response(profile.error());
          } else {
            auto stored = backend_.put_profile(profile.value());
            if (!stored) {
              response = error_response(stored.error());
            } else {
              JsonValue body = JsonValue::object();
              body.set("accepted", JsonValue::boolean(stored.value().accepted));
              body.set("schema_version",
                       JsonValue::number(stored.value().schema_version));
              response = json_response(200, body);
            }
          }
        }
      } else {
        response = method_not_allowed("GET, PUT");
      }
    } else if (segments.size() == 4 && segments[3] == "apply") {
      if (request.method != "POST") {
        response = method_not_allowed("POST");
      } else {
        const std::string driver_id = percent_decode(segments[2]);
        auto applied = backend_.apply_profile(driver_id);
        if (!applied) {
          response = error_response(applied.error());
        } else {
          JsonValue body = JsonValue::object();
          body.set("applied", JsonValue::boolean(applied.value().applied));
          body.set("latency_us", JsonValue::number(applied.value().latency_us));
          body.set("driver_id", JsonValue::string(driver_id));
          response = json_response(200, body);
        }
      }
    } else {
      response = error_response(make_error(ErrorCode::kNotFound, "unknown path"));
    }
  } else {
    response = error_response(make_error(ErrorCode::kNotFound, "unknown path"));
  }

  const std::uint64_t finished = now_us();
  log_info("gateway", "http_request",
           {Field("method", request.method), Field("path", path),
            Field("status", static_cast<std::int64_t>(response.status)),
            Field("duration_us", static_cast<std::int64_t>(finished - started))});
  response.headers["X-Cabin-Trace-Id"] = trace_id_hex(trace);
  return response;
}

} // namespace cabin::gateway
