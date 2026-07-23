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

auto compute_projection(double current_total) -> double {
  auto now = std::chrono::system_clock::now();
  std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(now)};
  std::chrono::year_month_day last_day{ymd.year() / ymd.month() / std::chrono::last};
  int days_in_month = static_cast<int>(static_cast<unsigned>(last_day.day()));
  int days_elapsed = static_cast<int>(static_cast<unsigned>(ymd.day()));
  
  if (days_elapsed == 0) return current_total;
  return (current_total / days_elapsed) * days_in_month;
}

auto mean(const std::vector<double>& values) -> double {
  if (values.empty()) {
    return 0.0;
  }
  double total = 0.0;
  for (const auto value : values) {
    total += value;
  }
  return total / static_cast<double>(values.size());
}

auto sample_stddev(const std::vector<double>& values) -> double {
  if (values.size() < 2) {
    return 0.0;
  }
  const auto average = mean(values);
  double squared_deltas = 0.0;
  for (const auto value : values) {
    squared_deltas += (value - average) * (value - average);
  }
  return std::sqrt(squared_deltas / static_cast<double>(values.size() - 1));
}

auto assess_cost_anomaly(const std::vector<double>& past_totals,
                         double evaluated_total,
                         double zscore_threshold) -> CostAnomalyAssessment {
  CostAnomalyAssessment assessment;
  assessment.evaluated_total = evaluated_total;
  if (past_totals.size() < 2) {
    return assessment;
  }

  assessment.enough_data = true;
  assessment.mean = mean(past_totals);
  assessment.stddev = sample_stddev(past_totals);

  if (assessment.stddev > 0.0) {
    assessment.zscore = (evaluated_total - assessment.mean) / assessment.stddev;
    assessment.anomalous = std::abs(assessment.zscore) >= zscore_threshold;
    return assessment;
  }

  // Zero-variance baseline: the z-score is undefined, so fall back to a
  // 20 percent deviation rule against the mean.
  if (assessment.mean > 0.0) {
    assessment.anomalous = std::abs(evaluated_total - assessment.mean) > assessment.mean * 0.2;
  } else {
    assessment.anomalous = evaluated_total > 0.0;
  }
  return assessment;
}

} // namespace azdash
