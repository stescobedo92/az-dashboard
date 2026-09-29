#pragma once

#include "az_dashboard/models.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace azdash {

namespace detail {
auto parse_usage_costs(const nlohmann::json& payload, const CliOptions& options) -> std::vector<ServiceCost>;
auto parse_budget_items(const nlohmann::json& payload, const CliOptions& options) -> std::vector<BudgetInfo>;
void append_advisor_findings(const nlohmann::json& payload, std::vector<WasteFinding>& findings);
void append_resource_heuristics(const nlohmann::json& payload, std::vector<WasteFinding>& findings);
void append_vm_heuristics(const nlohmann::json& payload, std::vector<WasteFinding>& findings);
auto normalize_term(std::string_view raw) -> std::string;
auto civil_date(int month_offset, bool month_start) -> std::string;
auto month_label(int month_offset) -> std::string;
auto evaluate_tag_compliance(const nlohmann::json& payload,
                             std::span<const std::string> required_tags,
                             std::string_view default_currency = "USD") -> TagComplianceSummary;
} // namespace detail

/**
 * @brief Abstract process runner used to isolate process execution from Azure parsing logic.
 *
 * Implementations receive typed executable/argument data assembled by AzureCliClient.
 */
class ICommandRunner {
public:
  virtual ~ICommandRunner() = default;

  /**
   * @brief Executes a typed command and captures the process output.
   * @param command Executable and argv-style arguments to execute.
   * @param options Runner options such as timeout.
   * @return Process exit code plus captured stdout and stderr text.
   */
  [[nodiscard]] virtual auto run(const ProcessCommand& command,
                                 const ProcessRunnerOptions& options = {}) const -> CommandResult = 0;
};

/**
 * @brief Cross-platform command runner.
 *
 * POSIX implementations execute without a shell and capture stdout/stderr separately.
 */
class ShellCommandRunner final : public ICommandRunner {
public:
  /**
   * @brief Executes a typed command.
   * @param command Executable and argv-style arguments to execute.
   * @param options Runner options such as timeout.
   * @return Process exit code and captured stdout/stderr where supported.
   */
  [[nodiscard]] auto run(const ProcessCommand& command, const ProcessRunnerOptions& options = {}) const
      -> CommandResult override;
};

/**
 * @brief Azure data provider implemented through the Azure CLI.
 *
 * User-controlled subscription and tenant values are passed as typed argv
 * arguments. Azure CLI failures or invalid JSON are reported as exceptions with
 * sensitive selectors redacted from command summaries.
 */
class ITrendCacheStore;

/**
 * @brief Abstract interface for Azure FinOps data retrieval.
 */
class IAzureClient {
public:
  virtual ~IAzureClient() = default;

  [[nodiscard]] virtual auto account(const CliOptions& options) const -> AccountInfo = 0;
  [[nodiscard]] virtual auto current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> = 0;
  [[nodiscard]] virtual auto previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> = 0;
  [[nodiscard]] virtual auto six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> = 0;
  [[nodiscard]] virtual auto waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> = 0;
  [[nodiscard]] virtual auto budgets(const CliOptions& options) const -> std::vector<BudgetInfo> = 0;
  [[nodiscard]] virtual auto commitment_recommendations(const CliOptions& options) const -> std::vector<CommitmentRecommendation> = 0;
  [[nodiscard]] virtual auto tag_compliance(const CliOptions& options) const -> TagComplianceSummary = 0;
};

/**
 * @brief Azure data provider implemented through the Azure CLI.
 *
 * User-controlled subscription and tenant values are passed as typed argv
 * arguments. Azure CLI failures or invalid JSON are reported as exceptions with
 * sensitive selectors redacted from command summaries.
 */
class AzureCliClient : public IAzureClient {
public:
  /**
   * @brief Creates a client with a process runner and optional trend cache.
   * @param runner Runner used for Azure CLI commands; must not be null.
   * @param cache Optional cache for closed historical month trend data.
   */
  explicit AzureCliClient(std::shared_ptr<ICommandRunner> runner,
                          std::shared_ptr<ITrendCacheStore> cache = nullptr);

  /**
   * @brief Reads the active Azure account.
   * @param options Parsed CLI options.
   * @return Account information from az account show.
   */
  [[nodiscard]] auto account(const CliOptions& options) const -> AccountInfo override;

  /**
   * @brief Reads current-month costs grouped by service.
   * @param options Parsed CLI options.
   * @return Service costs for the current billing window.
   */
  [[nodiscard]] auto current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> override;

  /**
   * @brief Reads previous-month costs grouped by service for the same day window.
   * @param options Parsed CLI options.
   * @return Service costs for the previous comparable billing window.
   */
  [[nodiscard]] auto previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> override;

  /**
   * @brief Reads six months of cost trends.
   * @param options Parsed CLI options.
   * @return Monthly cost aggregates.
   */
  [[nodiscard]] auto six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> override;

  /**
   * @brief Detects Azure waste from Advisor cost recommendations and resource heuristics.
   * @param options Parsed CLI options.
   * @return Waste findings.
   */
  [[nodiscard]] auto waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> override;

  /**
   * @brief Reads Azure consumption budgets.
   * @param options Parsed CLI options.
   * @return Active budget configurations and tracking status.
   */
  [[nodiscard]] auto budgets(const CliOptions& options) const -> std::vector<BudgetInfo> override;

  /**
   * @brief Evaluates commitment discount recommendations (Reserved Instances and Savings Plans).
   * @param options Parsed CLI options.
   * @return Active commitment recommendations.
   */
  [[nodiscard]] auto commitment_recommendations(const CliOptions& options) const -> std::vector<CommitmentRecommendation> override;

  /**
   * @brief Evaluates resource tagging compliance against required organization tags.
   * @param options Parsed CLI options.
   * @return Tag compliance and unallocated spend summary.
   */
  [[nodiscard]] auto tag_compliance(const CliOptions& options) const -> TagComplianceSummary override;

private:
  std::shared_ptr<ICommandRunner> runner_;
  std::shared_ptr<ITrendCacheStore> cache_;
};

} // namespace azdash
