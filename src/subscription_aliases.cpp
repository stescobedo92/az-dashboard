#include "az_dashboard/subscription_aliases.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace azdash {
namespace {

[[nodiscard]] auto env_path(const char* name) -> std::filesystem::path {
  const auto* value = std::getenv(name);
  if (value == nullptr || std::string(value).empty()) {
    return {};
  }
  return std::filesystem::path(value);
}

[[nodiscard]] auto load_aliases(const std::filesystem::path& path) -> std::unordered_map<std::string, std::string> {
  if (!std::filesystem::exists(path)) {
    return {};
  }

  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("failed to open alias-sub store: " + path.string());
  }

  auto payload = nlohmann::json{};
  file >> payload;
  if (!payload.is_object()) {
    throw std::runtime_error("alias-sub store must be a JSON object: " + path.string());
  }

  std::unordered_map<std::string, std::string> aliases;
  for (const auto& [alias, subscription] : payload.items()) {
    if (subscription.is_string()) {
      aliases.emplace(alias, subscription.get<std::string>());
    }
  }
  return aliases;
}

void save_aliases(const std::filesystem::path& path, const std::unordered_map<std::string, std::string>& aliases) {
  std::filesystem::create_directories(path.parent_path());

  nlohmann::json payload = nlohmann::json::object();
  for (const auto& [alias, subscription] : aliases) {
    payload[alias] = subscription;
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    throw std::runtime_error("failed to write alias-sub store: " + path.string());
  }
  file << payload.dump(2) << '\n';
}

} // namespace

SubscriptionAliasStore::SubscriptionAliasStore(std::filesystem::path path) : path_(std::move(path)) {}

auto SubscriptionAliasStore::list() const -> std::vector<SubscriptionAlias> {
  auto aliases = load_aliases(path_);
  std::vector<SubscriptionAlias> rows;
  rows.reserve(aliases.size());
  for (const auto& [alias, subscription] : aliases) {
    rows.push_back({alias, subscription});
  }
  std::ranges::sort(rows, {}, &SubscriptionAlias::alias);
  return rows;
}

auto SubscriptionAliasStore::resolve(const std::string& selector) const -> std::optional<std::string> {
  if (selector.empty()) {
    return std::nullopt;
  }

  const auto aliases = load_aliases(path_);
  if (const auto match = aliases.find(selector); match != aliases.end()) {
    return match->second;
  }
  return std::nullopt;
}

void SubscriptionAliasStore::set(const std::string& alias, const std::string& subscription) const {
  if (!is_valid_subscription_alias(alias)) {
    throw std::invalid_argument("alias-sub name must use letters, numbers, dash, underscore, or dot");
  }
  if (subscription.empty()) {
    throw std::invalid_argument("alias-sub subscription is required");
  }

  auto aliases = load_aliases(path_);
  aliases[alias] = subscription;
  save_aliases(path_, aliases);
}

auto SubscriptionAliasStore::remove(const std::string& alias) const -> bool {
  auto aliases = load_aliases(path_);
  const auto removed = aliases.erase(alias) > 0;
  if (removed) {
    save_aliases(path_, aliases);
  }
  return removed;
}

auto SubscriptionAliasStore::path() const -> const std::filesystem::path& {
  return path_;
}

auto default_subscription_alias_path() -> std::filesystem::path {
  if (const auto config_home = env_path("AZDASH_CONFIG_HOME"); !config_home.empty()) {
    return config_home / "subscription-aliases.json";
  }
  if (const auto config_home = env_path("XDG_CONFIG_HOME"); !config_home.empty()) {
    return config_home / "azdash" / "subscription-aliases.json";
  }
  if (const auto home = env_path("HOME"); !home.empty()) {
    return home / ".config" / "azdash" / "subscription-aliases.json";
  }
  return std::filesystem::current_path() / ".azdash" / "subscription-aliases.json";
}

auto is_valid_subscription_alias(const std::string& alias) -> bool {
  if (alias.empty() || alias.size() > 64) {
    return false;
  }

  for (const auto character : alias) {
    const auto is_letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    const auto is_digit = character >= '0' && character <= '9';
    const auto is_safe_symbol = character == '-' || character == '_' || character == '.';
    if (!is_letter && !is_digit && !is_safe_symbol) {
      return false;
    }
  }
  return true;
}

} // namespace azdash
