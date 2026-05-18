#pragma once

#include "az_dashboard/models.hpp"

#include <filesystem>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Abstract process runner used to isolate shell execution from Azure parsing logic.
 */
class ICommandRunner {
public:
  virtual ~ICommandRunner() = default;

  /**
   * @brief Executes a command and captures the process output.
   * @param command Full command line to execute.
   * @return Process exit code and captured output.
   */
  [[nodiscard]] virtual auto run(const std::string& command) const -> CommandResult = 0;
};

/**
 * @brief Cross-platform shell command runner based on popen.
 */
class ShellCommandRunner final : public ICommandRunner {
public:
  /**
   * @brief Executes a command through the platform shell.
   * @param command Full command line to execute.
   * @return Process exit code and captured stdout.
   */
  [[nodiscard]] auto run(const std::string& command) const -> CommandResult override;
};

/**
 * @brief Azure data provider implemented through the Azure CLI.
 */
class AzureCliClient {
public:
  /**
   * @brief Creates a client with a process runner.
   * @param runner Runner used for Azure CLI commands.
   */
  explicit AzureCliClient(std::shared_ptr<ICommandRunner> runner);

  /**
   * @brief Reads the active Azure account.
   * @param options Parsed CLI options.
   * @return Account information from az account show.
   */
  [[nodiscard]] auto account(const CliOptions& options) const -> AccountInfo;

  /**
   * @brief Reads current-month costs grouped by service.
   * @param options Parsed CLI options.
   * @return Service costs for the current billing window.
   */
  [[nodiscard]] auto current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost>;

  /**
   * @brief Reads previous-month costs grouped by service for the same day window.
   * @param options Parsed CLI options.
   * @return Service costs for the previous comparable billing window.
   */
  [[nodiscard]] auto previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost>;

  /**
   * @brief Reads six months of cost trends.
   * @param options Parsed CLI options.
   * @return Monthly cost aggregates.
   */
  [[nodiscard]] auto six_month_trends(const CliOptions& options) const -> std::vector<MonthCost>;

  /**
   * @brief Detects Azure waste from Advisor cost recommendations and resource heuristics.
   * @param options Parsed CLI options.
   * @return Waste findings.
   */
  [[nodiscard]] auto waste_findings(const CliOptions& options) const -> std::vector<WasteFinding>;

private:
  std::shared_ptr<ICommandRunner> runner_;

  [[nodiscard]] auto az_base(const CliOptions& options) const -> std::string;
  [[nodiscard]] auto subscription_arg(const CliOptions& options) const -> std::string;
  [[nodiscard]] auto run_json(const std::string& command) const -> nlohmann::json;
};

} // namespace azdash
