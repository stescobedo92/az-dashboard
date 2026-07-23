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
  Markdown
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
  UI
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
};

/**
 * @brief Month-level cost aggregate used by trend reports.
 */
struct MonthCost {
  std::string month;
  double total{0.0};
  std::vector<ServiceCost> services;
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
};

/**
 * @brief Point-in-time record of a cost run kept in the local history store.
 */
struct CostSnapshot {
  std::string timestamp;
  std::string subscription;
  double total{0.0};
  std::vector<ServiceCost> services;
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
