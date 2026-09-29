#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace azdash {

// CLI contract models.

/**
 * @brief Supported command output formats.
 */
enum class OutputFormat {
  Table,
  Json,
  Csv,
  Markdown,
  Html
};

/**
 * @brief Top-level command selected by the user.
 */
enum class CommandKind {
  Help,
  Cost,
  Trend,
  Waste,
  ReportCost,
  ReportTrend,
  ReportWaste,
  AliasSub,
  Version,
  Update,
  Report,
  CostAnomaly,
  History,
  LinkAccount,
  UI,
  Budget,
  Commitments,
  Compliance,
  Audit,
  Carbon
};

/**
 * @brief Dimension used to aggregate cost rows.
 */
enum class GroupBy {
  Service,
  ResourceGroup
};

/**
 * @brief Subscription alias command action.
 */
enum class AliasSubAction {
  List,
  Set,
  Remove
};

/**
 * @brief Local alias for an Azure subscription selector.
 */
struct SubscriptionAlias {
  std::string alias;
  std::string subscription;
};

enum class ProjectionMode {
  Linear,
  Weighted,
  HoltWinters
};

/**
 * @brief Parsed command-line options shared by all workflows.
 *
 * Global flags populate Azure subscription and tenant selectors, report output
 * location, display format, and bounded compatibility thresholds used by waste
 * analysis.
 */
struct CliOptions {
  CommandKind command{CommandKind::Help};
  OutputFormat output{OutputFormat::Table};
  std::vector<std::string> subscriptions;
  bool all_subscriptions{false};
  std::string tenant;
  std::string report_path;
  AliasSubAction alias_action{AliasSubAction::List};
  std::string alias_name;
  std::string alias_subscription;
  std::vector<std::string> selectors;
  GroupBy group_by{GroupBy::Service};
  std::vector<std::string> group_by_tags;
  std::vector<std::string> filter_tags;
  int function_memory_threshold_percent{10};
  int secrets_idle_days{90};
  std::optional<double> fail_if_exceeds_cost;
  std::string remediation_path;
  bool no_cache{false};
  bool fast_query{false};
  bool dry_run{false};
  std::string webhook_url;
  bool interactive{false};
  ProjectionMode projection_mode{ProjectionMode::Linear};
  std::string management_group;
  std::string budget_filter;
  std::string commitment_term;
  double min_savings{0.0};
  bool use_rest{false};
  std::vector<std::string> required_tags;
  double min_compliance_percent{0.0};
  std::string config_path;
  double anomaly_threshold{2.0};
  bool fail_on_anomaly{false};
  std::string remediation_format{"bash"};
  double min_audit_score{0.0};
  std::optional<double> fail_if_carbon_exceeds;
  std::string default_region{"eastus"};
};

// Azure analysis domain models.

/**
 * @brief Azure subscription identity information.
 */
struct AccountInfo {
  std::string subscription_id;
  std::string subscription_name;
  std::string tenant_id;
  std::string user_name;
};

/**
 * @brief Cost amount for a single Azure service in a period.
 */
struct ServiceCost {
  std::string service;
  double cost{0.0};
  std::map<std::string, std::string> tags;
  std::string currency{"USD"};
};

/**
 * @brief Month-level cost aggregate used by trend reports.
 */
struct MonthCost {
  std::string month;
  double total{0.0};
  std::vector<ServiceCost> services;
  std::string currency{"USD"};
};

/**
 * @brief Comparison row between current and previous billing windows.
 */
struct CostComparisonRow {
  std::string service;
  double previous{0.0};
  double current{0.0};
  double delta{0.0};
  double delta_percent{0.0};
  std::string currency{"USD"};
};

/**
 * @brief Waste or optimization recommendation detected for an Azure resource.
 */
struct WasteFinding {
  std::string check;
  std::string resource_id;
  std::string resource_type;
  std::string name;
  std::string location;
  std::string recommendation;
  double estimated_monthly_savings{0.0};
  std::string currency{"USD"};
};

/**
 * @brief Azure consumption budget information.
 */
struct BudgetInfo {
  std::string name;
  double amount{0.0};
  double current_spend{0.0};
  std::string time_grain{"Monthly"};
  std::string start_date;
  std::string end_date;
  std::string currency{"USD"};
};

/**
 * @brief Commitment-based discount recommendation (Reserved Instances and Savings Plans).
 */
struct CommitmentRecommendation {
  std::string id;
  std::string type;            // "ReservedInstance" or "SavingsPlan"
  std::string resource_type;   // "Microsoft.Compute/virtualMachines", etc.
  std::string sku;             // "Standard_D4s_v5", etc.
  std::string region;          // "eastus", etc.
  std::string term;            // "1 Year" or "3 Years"
  double estimated_monthly_savings{0.0};
  double estimated_monthly_cost{0.0};
  std::string currency{"USD"};
  std::string details;
};

/**
 * @brief Breakdown of a specific service or resource driving a cost anomaly spike.
 */
struct CostAnomalyDriver {
  std::string service;
  double current_cost{0.0};
  double baseline_mean{0.0};
  double cost_delta{0.0};
  double percentage_change{0.0};
  double contribution_percent{0.0};
  std::string impact; // "Critical", "High", "Medium", "Low"
  std::string currency{"USD"};
};

/**
 * @brief Statistical verdict for a cost anomaly check.
 */
struct CostAnomalyAssessment {
  bool enough_data{false};
  bool anomalous{false};
  double zscore{0.0};
  double mean{0.0};
  double stddev{0.0};
  double evaluated_total{0.0};
  std::string currency{"USD"};
  std::vector<CostAnomalyDriver> root_causes;
};

/**
 * @brief Point-in-time record of a cost run kept in the local history store.
 */
struct CostSnapshot {
  std::string timestamp;
  std::string subscription;
  double total{0.0};
  std::vector<ServiceCost> services;
  std::string currency{"USD"};
};

/**
 * @brief Complete analysis payload rendered by output and report services.
 */
struct AnalysisSnapshot {
  AccountInfo account;
  std::vector<CostComparisonRow> costs;
  std::vector<MonthCost> trends;
  std::vector<WasteFinding> waste;
};

/**
 * @brief Individual non-compliant or untagged resource detail.
 */
struct ResourceComplianceItem {
  std::string resource_name;
  std::string resource_group;
  std::string resource_type;
  double cost{0.0};
  std::string currency{"USD"};
  std::vector<std::string> missing_tags;
  std::map<std::string, std::string> tags;
};

/**
 * @brief Aggregated tag compliance and cost allocation summary.
 */
struct TagComplianceSummary {
  std::size_t total_resources{0};
  std::size_t compliant_resources{0};
  std::size_t non_compliant_resources{0};
  double total_spend{0.0};
  double allocated_spend{0.0};
  double unallocated_spend{0.0};
  double compliance_percentage{0.0};
  std::string currency{"USD"};
  std::map<std::string, std::size_t> missing_tag_counts;
  std::map<std::string, double> missing_tag_costs;
  std::vector<ResourceComplianceItem> non_compliant_items;
};

/**
 * @brief Assessment of a single FinOps capability pillar.
 */
struct FinOpsPillarScore {
  std::string name;
  double score{0.0}; // 0 - 100
  double weight{0.2}; // Weight in overall score
  std::string status; // "Healthy", "Warning", "Critical"
  std::string summary;
  std::vector<std::string> recommendations;
};

/**
 * @brief Complete FinOps Foundation maturity scorecard and governance audit.
 */
struct FinOpsAuditReport {
  double overall_score{0.0}; // 0 - 100
  std::string maturity_stage; // "Crawl", "Walk", "Run"
  std::string grade; // "A+", "A", "B", "C", "D", "F"
  double total_spend{0.0};
  double potential_savings{0.0};
  std::string currency{"USD"};
  std::vector<FinOpsPillarScore> pillars;
  std::vector<std::string> key_takeaways;
};

/**
 * @brief Carbon footprint and energy consumption breakdown for an Azure service.
 */
struct ServiceCarbonItem {
  std::string service;
  double cost{0.0};
  double energy_kwh{0.0};
  double emissions_kg{0.0};
  double embodied_emissions_kg{0.0};
  double total_emissions_kg{0.0};
  double avoidable_carbon_kg{0.0};
};

/**
 * @brief Enterprise GreenOps carbon footprint assessment and avoidance model.
 */
struct CarbonFootprintAssessment {
  double total_emissions_kg{0.0};
  double total_emissions_mt{0.0}; // Metric tons CO2e
  double scope2_location_based_kg{0.0};
  double scope2_market_based_kg{0.0};
  double scope3_embodied_kg{0.0};
  double total_energy_kwh{0.0};
  double avoidable_emissions_kg{0.0};
  double avoidable_emissions_percentage{0.0};
  double equivalent_cars_per_year{0.0};
  double equivalent_tree_seedlings{0.0};
  std::string region{"eastus"};
  std::vector<ServiceCarbonItem> services;
  std::vector<std::string> sustainability_tips;
};

// External process execution models.

/**
 * @brief Typed external process invocation.
 */
struct ProcessCommand {
  std::string executable;
  std::vector<std::string> arguments;
};

/**
 * @brief Options shared by external process runners.
 */
struct ProcessRunnerOptions {
  std::chrono::milliseconds timeout{std::chrono::seconds{30}};
};

/**
 * @brief Result of executing an external process.
 *
 * `stdout_text` carries normal process output, usually JSON from the Azure CLI.
 * `stderr_text` carries diagnostic output when a runner can capture it.
 */
struct CommandResult {
  int exit_code{0};
  std::string stdout_text;
  std::string stderr_text;
  bool timed_out{false};
};

} // namespace azdash
