#include "az_dashboard/history.hpp"

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

[[nodiscard]] auto load_snapshots(const std::filesystem::path& path) -> nlohmann::json {
  if (!std::filesystem::exists(path)) {
    return nlohmann::json::array();
  }

  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("failed to open cost history store: " + path.string());
  }

  nlohmann::json payload;
  try {
    file >> payload;
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error("cost history store is not valid JSON (" + path.string() + "): " + error.what());
  }
  if (!payload.is_array()) {
    throw std::runtime_error("cost history store must be a JSON array: " + path.string());
  }
  return payload;
}

[[nodiscard]] auto snapshot_to_json(const CostSnapshot& snapshot) -> nlohmann::json {
  nlohmann::json services = nlohmann::json::array();
  for (const auto& service : snapshot.services) {
    services.push_back({{"service", service.service}, {"cost", service.cost}});
  }
  return {{"timestamp", snapshot.timestamp},
          {"subscription", snapshot.subscription},
          {"total", snapshot.total},
          {"services", std::move(services)}};
}

[[nodiscard]] auto snapshot_from_json(const nlohmann::json& item) -> CostSnapshot {
  CostSnapshot snapshot;
  if (item.contains("timestamp") && item.at("timestamp").is_string()) {
    snapshot.timestamp = item.at("timestamp").get<std::string>();
  }
  if (item.contains("subscription") && item.at("subscription").is_string()) {
    snapshot.subscription = item.at("subscription").get<std::string>();
  }
  if (item.contains("total") && item.at("total").is_number()) {
    snapshot.total = item.at("total").get<double>();
  }
  if (item.contains("services") && item.at("services").is_array()) {
    for (const auto& service : item.at("services")) {
      if (!service.is_object() || !service.contains("service") || !service.at("service").is_string()) {
        continue;
      }
      ServiceCost cost;
      cost.service = service.at("service").get<std::string>();
      if (service.contains("cost") && service.at("cost").is_number()) {
        cost.cost = service.at("cost").get<double>();
      }
      snapshot.services.push_back(std::move(cost));
    }
  }
  return snapshot;
}

} // namespace

CostHistoryStore::CostHistoryStore(std::filesystem::path path) : path_(std::move(path)) {}

void CostHistoryStore::append(const CostSnapshot& snapshot) const {
  auto payload = load_snapshots(path_);
  payload.push_back(snapshot_to_json(snapshot));

  std::filesystem::create_directories(path_.parent_path());
  std::ofstream file(path_, std::ios::binary | std::ios::trunc);
  if (!file) {
    throw std::runtime_error("failed to write cost history store: " + path_.string());
  }
  file << payload.dump(2) << '\n';
}

auto CostHistoryStore::list() const -> std::vector<CostSnapshot> {
  const auto payload = load_snapshots(path_);
  std::vector<CostSnapshot> snapshots;
  snapshots.reserve(payload.size());
  for (const auto& item : payload) {
    if (item.is_object()) {
      snapshots.push_back(snapshot_from_json(item));
    }
  }
  return snapshots;
}

auto CostHistoryStore::path() const -> const std::filesystem::path& {
  return path_;
}

auto default_cost_history_path() -> std::filesystem::path {
  if (const auto config_home = env_path("AZDASH_CONFIG_HOME"); !config_home.empty()) {
    return config_home / "cost-history.json";
  }
  if (const auto config_home = env_path("XDG_CONFIG_HOME"); !config_home.empty()) {
    return config_home / "azdash" / "cost-history.json";
  }
  if (const auto home = env_path("HOME"); !home.empty()) {
    return home / ".config" / "azdash" / "cost-history.json";
  }
  return std::filesystem::current_path() / ".azdash" / "cost-history.json";
}

} // namespace azdash
