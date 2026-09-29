#include "az_dashboard/analytics.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>

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

auto evaluate_finops_audit(
    const AccountInfo& /*account*/,
    const std::vector<ServiceCost>& current_costs,
    const TagComplianceSummary& tag_compliance,
    const std::vector<WasteFinding>& waste_findings,
    const std::vector<BudgetInfo>& budgets,
    const std::vector<CommitmentRecommendation>& commitments,
    const CostAnomalyAssessment& anomaly) -> FinOpsAuditReport {
  FinOpsAuditReport report;
  report.currency = !current_costs.empty() ? current_costs.front().currency : "USD";

  double spend = 0.0;
  for (const auto& s : current_costs) {
    spend += s.cost;
  }
  if (spend <= 0.0 && tag_compliance.total_spend > 0.0) {
    spend = tag_compliance.total_spend;
  }
  report.total_spend = spend;

  double waste_savings = 0.0;
  for (const auto& w : waste_findings) {
    waste_savings += w.estimated_monthly_savings;
  }
  double commitment_savings = 0.0;
  for (const auto& c : commitments) {
    commitment_savings += c.estimated_monthly_savings;
  }
  report.potential_savings = waste_savings + commitment_savings;

  // Pillar 1: Tag Allocation & Hygiene (Weight: 25%)
  {
    FinOpsPillarScore pillar;
    pillar.name = "Tag Allocation & Hygiene";
    pillar.weight = 0.25;

    if (tag_compliance.total_resources == 0) {
      pillar.score = 100.0;
      pillar.status = "Healthy";
      pillar.summary = "No untagged or non-compliant resources detected.";
    } else {
      const double comp_score = tag_compliance.compliance_percentage;
      const double alloc_ratio = tag_compliance.total_spend > 0.0
                                     ? (tag_compliance.allocated_spend / tag_compliance.total_spend) * 100.0
                                     : 100.0;
      pillar.score = std::clamp(0.60 * comp_score + 0.40 * alloc_ratio, 0.0, 100.0);
      pillar.status = pillar.score >= 80.0 ? "Healthy" : (pillar.score >= 50.0 ? "Warning" : "Critical");

      std::ostringstream ss;
      ss << std::fixed << std::setprecision(1) << "Compliance at " << comp_score << "%, with "
         << tag_compliance.non_compliant_resources << " untagged or non-compliant resources ($"
         << tag_compliance.unallocated_spend << " unallocated).";
      pillar.summary = ss.str();

      if (tag_compliance.non_compliant_resources > 0) {
        pillar.recommendations.push_back("Enforce required tags (Owner, Environment, CostCenter) via Azure Policy.");
        pillar.recommendations.push_back("Remediate untagged resources using 'azdash compliance --generate-remediation'.");
      }
    }
    report.pillars.push_back(std::move(pillar));
  }

  // Pillar 2: Waste & Idle Efficiency (Weight: 25%)
  {
    FinOpsPillarScore pillar;
    pillar.name = "Waste & Idle Efficiency";
    pillar.weight = 0.25;

    if (waste_findings.empty()) {
      pillar.score = 100.0;
      pillar.status = "Healthy";
      pillar.summary = "Zero idle or orphan resources detected.";
    } else {
      const double waste_ratio = spend > 0.0 ? (waste_savings / spend) : 0.0;
      double base_score = 100.0;
      if (waste_ratio <= 0.02) {
        base_score = 95.0;
      } else if (waste_ratio <= 0.05) {
        base_score = 85.0;
      } else if (waste_ratio <= 0.10) {
        base_score = 70.0;
      } else if (waste_ratio <= 0.20) {
        base_score = 50.0;
      } else {
        base_score = std::max(10.0, 50.0 - (waste_ratio - 0.20) * 100.0);
      }
      pillar.score = std::clamp(base_score - static_cast<double>(waste_findings.size()) * 2.0, 0.0, 100.0);
      pillar.status = pillar.score >= 80.0 ? "Healthy" : (pillar.score >= 50.0 ? "Warning" : "Critical");

      std::ostringstream ss;
      ss << std::fixed << std::setprecision(2) << waste_findings.size()
         << " orphan/idle resources detected ($" << waste_savings << "/mo estimated waste).";
      pillar.summary = ss.str();

      pillar.recommendations.push_back("Clean up unattached disks, idle NICs, and empty App Service Plans.");
      pillar.recommendations.push_back("Use 'azdash waste --generate-remediation' or interactive mode to reclaim spend.");
    }
    report.pillars.push_back(std::move(pillar));
  }

  // Pillar 3: Rate Optimization & Commitments (Weight: 20%)
  {
    FinOpsPillarScore pillar;
    pillar.name = "Commitment & Rate Optimization";
    pillar.weight = 0.20;

    if (commitments.empty()) {
      pillar.score = 100.0;
      pillar.status = "Healthy";
      pillar.summary = "No unpurchased Reserved Instance or Savings Plan recommendations.";
    } else {
      const double comm_ratio = spend > 0.0 ? (commitment_savings / spend) : 0.0;
      pillar.score = std::clamp(100.0 - comm_ratio * 120.0 - static_cast<double>(commitments.size()) * 4.0, 15.0, 100.0);
      pillar.status = pillar.score >= 80.0 ? "Healthy" : (pillar.score >= 50.0 ? "Warning" : "Critical");

      std::ostringstream ss;
      ss << std::fixed << std::setprecision(2) << commitments.size()
         << " commitment opportunities available ($" << commitment_savings << "/mo potential savings).";
      pillar.summary = ss.str();

      pillar.recommendations.push_back("Evaluate 1-year or 3-year Reserved Instances for stable compute workloads.");
      pillar.recommendations.push_back("Purchase Azure Savings Plans to achieve flexible cross-family rate discounts.");
    }
    report.pillars.push_back(std::move(pillar));
  }

  // Pillar 4: Budgeting & Guardrails (Weight: 15%)
  {
    FinOpsPillarScore pillar;
    pillar.name = "Budget Governance & Guardrails";
    pillar.weight = 0.15;

    if (budgets.empty()) {
      pillar.score = 25.0;
      pillar.status = "Critical";
      pillar.summary = "No active Azure budgets configured.";
      pillar.recommendations.push_back("Configure Azure Consumption Budgets with automated email/webhook alerts.");
    } else {
      bool has_breach = false;
      for (const auto& b : budgets) {
        if (b.amount > 0.0 && b.current_spend > b.amount) {
          has_breach = true;
          break;
        }
      }
      if (has_breach) {
        pillar.score = 55.0;
        pillar.status = "Warning";
        pillar.summary = "Budgets are configured, but one or more active budgets have exceeded 100% of allocation.";
        pillar.recommendations.push_back("Adjust spending velocity or update budget allocations to resolve overruns.");
      } else {
        pillar.score = 100.0;
        pillar.status = "Healthy";
        pillar.summary = "Active budgets configured and currently tracking within spending limits.";
      }
    }
    report.pillars.push_back(std::move(pillar));
  }

  // Pillar 5: Trend & Anomaly Readiness (Weight: 15%)
  {
    FinOpsPillarScore pillar;
    pillar.name = "Variance & Anomaly Control";
    pillar.weight = 0.15;

    if (!anomaly.enough_data) {
      pillar.score = 70.0;
      pillar.status = "Warning";
      pillar.summary = "Insufficient historical billing cycles for statistical anomaly baseline (Crawl stage).";
      pillar.recommendations.push_back("Retain at least 3-6 billing cycles of cost history for statistical baselining.");
    } else if (anomaly.anomalous) {
      pillar.score = 40.0;
      pillar.status = "Critical";
      std::ostringstream ss;
      ss << std::fixed << std::setprecision(2) << "Active cost anomaly detected (Z-Score: "
         << anomaly.zscore << ") exceeding baseline mean ($" << anomaly.mean << ").";
      pillar.summary = ss.str();
      pillar.recommendations.push_back("Investigate root cause drivers with 'azdash anomaly' and set up CI/CD fail gates.");
    } else {
      pillar.score = 100.0;
      pillar.status = "Healthy";
      std::ostringstream ss;
      ss << std::fixed << std::setprecision(2) << "Spend is stable and within baseline variance (Z-Score: "
         << anomaly.zscore << ").";
      pillar.summary = ss.str();
    }
    report.pillars.push_back(std::move(pillar));
  }

  // Calculate weighted overall score
  double total_weight = 0.0;
  double weighted_sum = 0.0;
  for (const auto& p : report.pillars) {
    weighted_sum += p.score * p.weight;
    total_weight += p.weight;
  }
  report.overall_score = total_weight > 0.0 ? (weighted_sum / total_weight) : 0.0;
  report.overall_score = std::clamp(report.overall_score, 0.0, 100.0);

  // FinOps Maturity Stage (FinOps Foundation standard)
  if (report.overall_score >= 80.0) {
    report.maturity_stage = "Run";
  } else if (report.overall_score >= 50.0) {
    report.maturity_stage = "Walk";
  } else {
    report.maturity_stage = "Crawl";
  }

  // Letter Grade
  if (report.overall_score >= 95.0) {
    report.grade = "A+";
  } else if (report.overall_score >= 90.0) {
    report.grade = "A";
  } else if (report.overall_score >= 80.0) {
    report.grade = "B";
  } else if (report.overall_score >= 70.0) {
    report.grade = "C";
  } else if (report.overall_score >= 60.0) {
    report.grade = "D";
  } else {
    report.grade = "F";
  }

  // Key takeaways
  if (waste_savings > 0.0) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << "Reclaim up to $" << waste_savings
       << "/month by eliminating " << waste_findings.size() << " idle/orphan resources.";
    report.key_takeaways.push_back(ss.str());
  }
  if (commitment_savings > 0.0) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << "Save up to $" << commitment_savings
       << "/month with commitment-based discount recommendations.";
    report.key_takeaways.push_back(ss.str());
  }
  if (tag_compliance.unallocated_spend > 0.0) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << "Allocate $" << tag_compliance.unallocated_spend
       << " of untagged spend to improve department accountability.";
    report.key_takeaways.push_back(ss.str());
  }
  if (report.key_takeaways.empty()) {
    report.key_takeaways.push_back("FinOps posture is excellent! Maintain continuous monitoring and guardrails.");
  }

  return report;
}

auto get_regional_grid_intensity(std::string_view region) -> double {
  std::string lower_region;
  lower_region.reserve(region.size());
  for (char ch : region) {
    lower_region.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (lower_region == "swedencentral") {
    return 18.0;
  }
  if (lower_region == "norwayeast" || lower_region == "norwaywest") {
    return 25.0;
  }
  if (lower_region == "francecentral" || lower_region == "francesouth") {
    return 55.0;
  }
  if (lower_region == "switzerlandnorth" || lower_region == "switzerlandwest") {
    return 60.0;
  }
  if (lower_region == "brazilsouth") {
    return 115.0;
  }
  if (lower_region == "canadacentral" || lower_region == "canadaeast") {
    return 120.0;
  }
  if (lower_region == "northeurope") {
    return 165.0;
  }
  if (lower_region == "uksouth" || lower_region == "ukwest") {
    return 210.0;
  }
  if (lower_region == "westeurope") {
    return 215.0;
  }
  if (lower_region == "westus" || lower_region == "westus2" || lower_region == "westus3") {
    return 240.0;
  }
  if (lower_region == "eastus" || lower_region == "eastus2") {
    return 380.0;
  }
  if (lower_region == "southcentralus") {
    return 410.0;
  }
  if (lower_region == "southeastasia" || lower_region == "eastasia") {
    return 420.0;
  }
  if (lower_region == "japaneast" || lower_region == "japanwest") {
    return 470.0;
  }
  if (lower_region == "centralus" || lower_region == "northcentralus") {
    return 490.0;
  }
  if (lower_region == "australiaeast" || lower_region == "australiasoutheast") {
    return 630.0;
  }
  return 350.0;
}

auto estimate_carbon_footprint(
    std::span<const ServiceCost> costs,
    std::span<const WasteFinding> waste,
    std::string_view region) -> CarbonFootprintAssessment {
  CarbonFootprintAssessment assessment;
  assessment.region = region.empty() ? "eastus" : std::string(region);

  const double grid_intensity = get_regional_grid_intensity(assessment.region);
  constexpr double azure_pue = 1.15; // Azure hyperscale datacenter Power Usage Effectiveness
  constexpr double market_based_multiplier = 0.15; // Azure renewable energy certificate match factor
  constexpr double embodied_carbon_ratio = 0.25;   // Scope 3 hardware manufacturing factor

  double total_energy = 0.0;
  double total_scope2_loc = 0.0;
  double total_scope3 = 0.0;

  auto get_energy_intensity = [](std::string_view svc_name) -> double {
    std::string lower;
    lower.reserve(svc_name.size());
    for (char c : svc_name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower.find("virtual machine") != std::string::npos || lower.find("compute") != std::string::npos) {
      return 0.35; // kWh / USD
    }
    if (lower.find("storage") != std::string::npos || lower.find("disk") != std::string::npos) {
      return 0.08;
    }
    if (lower.find("database") != std::string::npos || lower.find("sql") != std::string::npos || lower.find("cosmos") != std::string::npos) {
      return 0.28;
    }
    if (lower.find("app service") != std::string::npos || lower.find("function") != std::string::npos) {
      return 0.22;
    }
    if (lower.find("network") != std::string::npos || lower.find("bandwidth") != std::string::npos) {
      return 0.12;
    }
    return 0.20;
  };

  for (const auto& c : costs) {
    const double kwh_per_dollar = get_energy_intensity(c.service);
    const double kwh = c.cost * kwh_per_dollar;
    const double scope2_loc_kg = (kwh * azure_pue * grid_intensity) / 1000.0;
    const double scope3_kg = scope2_loc_kg * embodied_carbon_ratio;
    const double total_kg = scope2_loc_kg + scope3_kg;

    total_energy += kwh;
    total_scope2_loc += scope2_loc_kg;
    total_scope3 += scope3_kg;

    assessment.services.push_back(ServiceCarbonItem{
        .service = c.service,
        .cost = c.cost,
        .energy_kwh = kwh,
        .emissions_kg = scope2_loc_kg,
        .embodied_emissions_kg = scope3_kg,
        .total_emissions_kg = total_kg,
        .avoidable_carbon_kg = 0.0,
    });
  }

  // Sort services by total emissions descending
  std::ranges::sort(assessment.services, [](const auto& a, const auto& b) {
    return a.total_emissions_kg > b.total_emissions_kg;
  });

  // Calculate waste carbon avoidance
  double total_waste_savings = 0.0;
  for (const auto& w : waste) {
    total_waste_savings += w.estimated_monthly_savings;
  }

  // Average compute/storage blend of 0.25 kWh/USD for orphan/idle resources
  const double avoidable_kwh = total_waste_savings * 0.25;
  const double avoidable_kg = (avoidable_kwh * azure_pue * grid_intensity) / 1000.0 * (1.0 + embodied_carbon_ratio);

  assessment.total_energy_kwh = total_energy;
  assessment.scope2_location_based_kg = total_scope2_loc;
  assessment.scope2_market_based_kg = total_scope2_loc * market_based_multiplier;
  assessment.scope3_embodied_kg = total_scope3;
  assessment.total_emissions_kg = total_scope2_loc + total_scope3;
  assessment.total_emissions_mt = assessment.total_emissions_kg / 1000.0;
  assessment.avoidable_emissions_kg = avoidable_kg;
  assessment.avoidable_emissions_percentage = assessment.total_emissions_kg > 0.0
                                                 ? std::min(100.0, (avoidable_kg / assessment.total_emissions_kg) * 100.0)
                                                 : 0.0;

  // EPA GHG Equivalencies:
  // 1 passenger car driven 1 year ~ 4,600 kg CO2e
  // 1 tree seedling grown 10 years ~ 60 kg CO2e sequestered
  assessment.equivalent_cars_per_year = assessment.total_emissions_kg / 4600.0;
  assessment.equivalent_tree_seedlings = assessment.total_emissions_kg / 60.0;

  // Sustainability recommendations
  if (avoidable_kg > 0.0) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << "Remediating detected waste saves ~"
       << avoidable_kg << " kg CO2e/month (" << assessment.avoidable_emissions_percentage
       << "% carbon reduction).";
    assessment.sustainability_tips.push_back(ss.str());
  }

  if (grid_intensity > 200.0) {
    assessment.sustainability_tips.push_back(
        "Consider shifting flexible or batch workloads to low-carbon Azure regions like Sweden Central (18 g/kWh) or France Central (55 g/kWh).");
  }
  assessment.sustainability_tips.push_back(
      "Enable auto-shutdown and right-size underutilized compute instances to reduce Scope 2 electricity consumption.");

  return assessment;
}

} // namespace azdash
