#include "az_dashboard/analytics.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace azdash {

auto compare_costs(const std::vector<ServiceCost>& current,
                   const std::vector<ServiceCost>& previous) -> std::vector<CostComparisonRow> {
  std::map<std::string, double> current_by_service;
  std::map<std::string, double> previous_by_service;
  std::set<std::string> services;

  for (const auto& row : current) {
    current_by_service[row.service] += row.cost;
    services.insert(row.service);
  }

  for (const auto& row : previous) {
    previous_by_service[row.service] += row.cost;
    services.insert(row.service);
  }

  std::vector<CostComparisonRow> rows;
  rows.reserve(services.size());

  for (const auto& service : services) {
    const auto previous_cost = previous_by_service[service];
    const auto current_cost = current_by_service[service];
    const auto delta = current_cost - previous_cost;
    const auto percent = previous_cost == 0.0 ? (current_cost == 0.0 ? 0.0 : 100.0)
                                              : (delta / previous_cost) * 100.0;
    rows.push_back({service, previous_cost, current_cost, delta, percent});
  }

  std::ranges::sort(rows, [](const auto& lhs, const auto& rhs) {
    return lhs.current > rhs.current;
  });

  return rows;
}

auto total_cost(const std::vector<ServiceCost>& costs) -> double {
  double total = 0.0;
  for (const auto& row : costs) {
    total += row.cost;
  }
  return total;
}

} // namespace azdash
