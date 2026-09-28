#pragma once

#include "az_dashboard/models.hpp"

#include <memory>
#include <vector>

namespace azdash {

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

private:
  std::shared_ptr<ICommandRunner> runner_;
  std::shared_ptr<ITrendCacheStore> cache_;
};

} // namespace azdash
