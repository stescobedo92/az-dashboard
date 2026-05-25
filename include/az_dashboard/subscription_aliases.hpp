#pragma once

#include "az_dashboard/models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Local subscription alias store.
 */
class SubscriptionAliasStore {
public:
  explicit SubscriptionAliasStore(std::filesystem::path path);

  [[nodiscard]] auto list() const -> std::vector<SubscriptionAlias>;
  [[nodiscard]] auto resolve(const std::string& selector) const -> std::optional<std::string>;
  void set(const std::string& alias, const std::string& subscription) const;
  [[nodiscard]] auto remove(const std::string& alias) const -> bool;

  [[nodiscard]] auto path() const -> const std::filesystem::path&;

private:
  std::filesystem::path path_;
};

/**
 * @brief Returns the default per-user alias file path.
 */
[[nodiscard]] auto default_subscription_alias_path() -> std::filesystem::path;

/**
 * @brief Validates an alias-sub name.
 */
[[nodiscard]] auto is_valid_subscription_alias(const std::string& alias) -> bool;

} // namespace azdash
