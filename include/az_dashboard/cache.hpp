#pragma once

#include "az_dashboard/models.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Interface for caching historical closed-month usage data.
 */
class ITrendCacheStore {
public:
  virtual ~ITrendCacheStore() = default;

  [[nodiscard]] virtual auto get(const std::string& subscription,
                                 const std::string& month,
                                 const std::string& dimension) const
      -> std::optional<std::vector<ServiceCost>> = 0;

  virtual void put(const std::string& subscription,
                   const std::string& month,
                   const std::string& dimension,
                   const std::vector<ServiceCost>& services) const = 0;
};

/**
 * @brief Local file-based JSON implementation of trend cache store.
 */
class LocalTrendCacheStore final : public ITrendCacheStore {
public:
  explicit LocalTrendCacheStore(std::filesystem::path path);

  [[nodiscard]] auto get(const std::string& subscription,
                         const std::string& month,
                         const std::string& dimension) const
      -> std::optional<std::vector<ServiceCost>> override;

  void put(const std::string& subscription,
           const std::string& month,
           const std::string& dimension,
           const std::vector<ServiceCost>& services) const override;

  [[nodiscard]] auto path() const -> const std::filesystem::path&;

private:
  std::filesystem::path path_;
  mutable std::mutex mutex_;
};

/**
 * @brief Resolves default cache path for historical months.
 */
[[nodiscard]] auto default_trend_cache_path() -> std::filesystem::path;

} // namespace azdash
