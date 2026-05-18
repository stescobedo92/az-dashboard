#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Supported command output formats.
 */
enum class OutputFormat {
  Table,
  Json,
  Csv
};

/**
 * @brief Top-level command selected by the user.
 */
enum class CommandKind {
  Cost,
  Trend,
  Waste,
  ReportCost,
  ReportTrend,
  ReportWaste,
  Version,
  Update,
  Help
};

/**
 * @brief Parsed command-line options shared by all workflows.
 */
struct CliOptions {
  CommandKind command{CommandKind::Help};
  OutputFormat output{OutputFormat::Table};
  std::string subscription;
  std::string tenant;
  std::string report_path;
  std::vector<std::string> selectors;
  int function_memory_threshold_percent{10};
  int secrets_idle_days{90};
};

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
 * @brief Complete analysis payload rendered by output and report services.
 */
struct AnalysisSnapshot {
  AccountInfo account;
  std::vector<CostComparisonRow> costs;
  std::vector<MonthCost> trends;
  std::vector<WasteFinding> waste;
};

/**
 * @brief Result of executing an external process.
 */
struct CommandResult {
  int exit_code{0};
  std::string stdout_text;
  std::string stderr_text;
};

} // namespace azdash
