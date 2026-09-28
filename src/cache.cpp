#include "az_dashboard/cache.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace azdash {
namespace {

[[nodiscard]] auto env_path(const char* name) -> std::filesystem::path {
  const auto* value = std::getenv(name);
  if (value == nullptr || std::string(value).empty()) {
    return {};
  }
  return std::filesystem::path(value);
}

[[nodiscard]] auto cache_key(const std::string& subscription,
                             const std::string& month,
                             const std::string& dimension) -> std::string {
  return subscription + "|" + month + "|" + dimension;
}

[[nodiscard]] auto load_cache_json(const std::filesystem::path& path) -> nlohmann::json {
  if (!std::filesystem::exists(path)) {
    return nlohmann::json::object();
  }

  std::ifstream file(path);
  if (!file) {
    return nlohmann::json::object();
  }

  try {
    nlohmann::json payload;
    file >> payload;
    if (payload.is_object()) {
      return payload;
    }
  } catch (...) {
  }
  return nlohmann::json::object();
}

} // namespace

LocalTrendCacheStore::LocalTrendCacheStore(std::filesystem::path path) : path_(std::move(path)) {}

auto LocalTrendCacheStore::get(const std::string& subscription,
                               const std::string& month,
                               const std::string& dimension) const
    -> std::optional<std::vector<ServiceCost>> {
  const auto key = cache_key(subscription, month, dimension);
  const auto payload = load_cache_json(path_);
  if (!payload.contains(key) || !payload.at(key).is_array()) {
    return std::nullopt;
  }

  std::vector<ServiceCost> services;
  for (const auto& item : payload.at(key)) {
    if (!item.is_object() || !item.contains("service") || !item.contains("cost")) {
      continue;
    }
    std::string service = item.at("service").is_string() ? item.at("service").get<std::string>() : "Unknown";
    double cost = item.at("cost").is_number() ? item.at("cost").get<double>() : 0.0;
    std::string currency = item.contains("currency") && item.at("currency").is_string()
                               ? item.at("currency").get<std::string>()
                               : "USD";
    services.push_back({std::move(service), cost, {}, std::move(currency)});
  }
  return services;
}

void LocalTrendCacheStore::put(const std::string& subscription,
                               const std::string& month,
                               const std::string& dimension,
                               const std::vector<ServiceCost>& services) const {
  const auto key = cache_key(subscription, month, dimension);
  auto payload = load_cache_json(path_);

  nlohmann::json array = nlohmann::json::array();
  for (const auto& service : services) {
    array.push_back({
        {"service", service.service},
        {"cost", service.cost},
        {"currency", service.currency},
    });
  }
  payload[key] = std::move(array);

  try {
    if (!path_.parent_path().empty()) {
      std::filesystem::create_directories(path_.parent_path());
    }
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    if (file) {
      file << payload.dump(2) << '\n';
    }
  } catch (...) {
  }
}

auto LocalTrendCacheStore::path() const -> const std::filesystem::path& {
  return path_;
}

auto default_trend_cache_path() -> std::filesystem::path {
  if (const auto cache_home = env_path("AZDASH_CACHE_HOME"); !cache_home.empty()) {
    return cache_home / "trend-cache.json";
  }
  if (const auto cache_home = env_path("XDG_CACHE_HOME"); !cache_home.empty()) {
    return cache_home / "azdash" / "trend-cache.json";
  }
  if (const auto home = env_path("HOME"); !home.empty()) {
    return home / ".cache" / "azdash" / "trend-cache.json";
  }
  if (const auto config_home = env_path("AZDASH_CONFIG_HOME"); !config_home.empty()) {
    return config_home / "trend-cache.json";
  }
  return std::filesystem::current_path() / ".azdash" / "trend-cache.json";
}

} // namespace azdash
