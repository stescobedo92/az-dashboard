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

auto compute_projection(double current_total,
                        ProjectionMode mode,
                        std::span<const double> historical_totals) -> double {
  auto now = std::chrono::system_clock::now();
  std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(now)};
  std::chrono::year_month_day last_day{ymd.year() / ymd.month() / std::chrono::last};
  int days_in_month = static_cast<int>(static_cast<unsigned>(last_day.day()));
  int days_elapsed = static_cast<int>(static_cast<unsigned>(ymd.day()));

  if (days_elapsed == 0) return current_total;
  double linear_projection = (current_total / static_cast<double>(days_elapsed)) * static_cast<double>(days_in_month);

  if (mode == ProjectionMode::Linear || historical_totals.empty()) {
    return linear_projection;
  }

  if (mode == ProjectionMode::HoltWinters) {
    if (historical_totals.size() < 2) {
      return linear_projection;
    }
    // Holt's linear trend method (double exponential smoothing):
    constexpr double alpha = 0.5; // Level smoothing parameter
    constexpr double beta = 0.3;  // Trend smoothing parameter

    double level = historical_totals[0];
    double trend = historical_totals[1] - historical_totals[0];

    for (std::size_t t = 1; t < historical_totals.size(); ++t) {
      double prev_level = level;
      level = alpha * historical_totals[t] + (1.0 - alpha) * (level + trend);
      trend = beta * (level - prev_level) + (1.0 - beta) * trend;
    }

    double baseline_forecast = std::max(0.0, level + trend);
    double p = static_cast<double>(days_elapsed) / static_cast<double>(days_in_month);
    if (p >= 1.0) {
      return current_total;
    }
    return current_total + (1.0 - p) * baseline_forecast;
  }

  // Weighted projection using Bayesian shrinkage against historical baseline:
  double total_weight = 0.0;
  double weighted_sum = 0.0;
  for (std::size_t i = 0; i < historical_totals.size(); ++i) {
    double weight = static_cast<double>(i + 1);
    weighted_sum += historical_totals[i] * weight;
    total_weight += weight;
  }
  double baseline = total_weight > 0.0 ? (weighted_sum / total_weight) : 0.0;

  double p = static_cast<double>(days_elapsed) / static_cast<double>(days_in_month);
  if (p >= 1.0) {
    return current_total;
  }

  return current_total + (1.0 - p) * baseline;
}

auto compute_projection(double current_total) -> double {
  return compute_projection(current_total, ProjectionMode::Linear, {});
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

auto assess_cost_anomaly_with_attribution(
    std::span<const MonthCost> past_months,
    const MonthCost& current_month,
    double zscore_threshold,
    ProjectionMode mode) -> CostAnomalyAssessment {
  std::vector<double> past_totals;
  past_totals.reserve(past_months.size());
  for (const auto& m : past_months) {
    past_totals.push_back(m.total);
  }

  const double projected = compute_projection(current_month.total, mode, past_totals);
  auto assessment = assess_cost_anomaly(past_totals, projected, zscore_threshold);
  assessment.currency = current_month.currency.empty() ? "USD" : current_month.currency;

  if (past_months.empty()) {
    return assessment;
  }

  // Calculate historical baseline average per service across past months:
  std::map<std::string, double> service_sum;
  for (const auto& m : past_months) {
    for (const auto& s : m.services) {
      service_sum[s.service] += s.cost;
    }
  }

  const double num_months = static_cast<double>(past_months.size());
  std::map<std::string, double> service_baseline;
  for (const auto& [svc, sum] : service_sum) {
    service_baseline[svc] = sum / num_months;
  }

  // Current month costs per service:
  std::map<std::string, double> current_by_service;
  for (const auto& s : current_month.services) {
    current_by_service[s.service] += s.cost;
  }

  // Identify drivers with positive cost deltas (spikes):
  struct CandidateDriver {
    std::string service;
    double current_cost{0.0};
    double baseline{0.0};
    double delta{0.0};
    double pct_change{0.0};
  };

  std::vector<CandidateDriver> candidates;
  double total_positive_delta = 0.0;

  for (const auto& [svc, current_cost] : current_by_service) {
    double base = 0.0;
    if (auto it = service_baseline.find(svc); it != service_baseline.end()) {
      base = it->second;
    }
    double delta = current_cost - base;
    if (delta > 0.001) {
      double pct_change = base > 0.0 ? (delta / base) * 100.0 : 100.0;
      candidates.push_back({svc, current_cost, base, delta, pct_change});
      total_positive_delta += delta;
    }
  }

  std::ranges::sort(candidates, [](const auto& a, const auto& b) {
    return a.delta > b.delta;
  });

  for (const auto& c : candidates) {
    double contrib = total_positive_delta > 0.0 ? (c.delta / total_positive_delta) * 100.0 : 0.0;
    std::string impact;
    if (contrib >= 50.0) {
      impact = "Critical";
    } else if (contrib >= 25.0) {
      impact = "High";
    } else if (contrib >= 10.0) {
      impact = "Medium";
    } else {
      impact = "Low";
    }

    assessment.root_causes.push_back(CostAnomalyDriver{
        .service = c.service,
        .current_cost = c.current_cost,
        .baseline_mean = c.baseline,
        .cost_delta = c.delta,
        .percentage_change = c.pct_change,
        .contribution_percent = contrib,
        .impact = std::move(impact),
        .currency = assessment.currency,
    });
  }

  return assessment;
}

} // namespace azdash
