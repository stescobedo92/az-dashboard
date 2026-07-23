#pragma once

#include "az_dashboard/models.hpp"

#include <filesystem>
#include <vector>

namespace azdash {

/**
 * @brief Local append-only store of cost snapshots.
 *
 * Snapshots are persisted as a JSON array so past runs can be inspected and
 * compared without calling the Azure Consumption API again.
 */
class CostHistoryStore {
public:
  explicit CostHistoryStore(std::filesystem::path path);

  /**
   * @brief Appends a snapshot to the store, creating the file when missing.
   */
  void append(const CostSnapshot& snapshot) const;

  /**
   * @brief Lists stored snapshots in append order.
   */
  [[nodiscard]] auto list() const -> std::vector<CostSnapshot>;

  [[nodiscard]] auto path() const -> const std::filesystem::path&;

private:
  std::filesystem::path path_;
};

/**
 * @brief Returns the default per-user cost history file path.
 */
[[nodiscard]] auto default_cost_history_path() -> std::filesystem::path;

} // namespace azdash
