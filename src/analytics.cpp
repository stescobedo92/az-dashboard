#include "az_dashboard/analytics.hpp"

namespace azdash {

auto compare_costs(const std::vector<ServiceCost>& current,
                   const std::vector<ServiceCost>& previous) -> std::vector<CostComparisonRow> {
  return compare_costs_by(current,
                          previous,
                          [](const ServiceCost& row) { return row.service; },
                          [](const ServiceCost& row) { return row.cost; });
}

auto total_cost(const std::vector<ServiceCost>& costs) -> double {
  double total = 0.0;
  for (const auto& row : costs) {
    total += row.cost;
  }
  return total;
}

} // namespace azdash
