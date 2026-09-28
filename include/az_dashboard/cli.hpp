#pragma once

#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/models.hpp"

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Account lookup required by CLI workflows that talk to Azure.
 */
class ICliAccountProvider {
public:
  virtual ~ICliAccountProvider() = default;

  [[nodiscard]] virtual auto account(const CliOptions& options) const -> AccountInfo = 0;
};

/**
 * @brief Cost data required by cost workflows.
 */
class ICliCostProvider {
public:
  virtual ~ICliCostProvider() = default;

  [[nodiscard]] virtual auto current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> = 0;
  [[nodiscard]] virtual auto previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> = 0;
};

/**
 * @brief Trend data required by trend workflows.
 */
class ICliTrendProvider {
public:
  virtual ~ICliTrendProvider() = default;

  [[nodiscard]] virtual auto six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> = 0;
};

/**
 * @brief Waste data required by waste workflows.
 */
class ICliWasteProvider {
public:
  virtual ~ICliWasteProvider() = default;

  [[nodiscard]] virtual auto waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> = 0;
};

/**
 * @brief Azure consumption budget provider required by budget workflows.
 */
class ICliBudgetProvider {
public:
  virtual ~ICliBudgetProvider() = default;

  [[nodiscard]] virtual auto budgets(const CliOptions& options) const -> std::vector<BudgetInfo> = 0;
};

/**
 * @brief Azure commitment discount recommendations provider.
 */
class ICliCommitmentProvider {
public:
  virtual ~ICliCommitmentProvider() = default;

  [[nodiscard]] virtual auto commitment_recommendations(const CliOptions& options) const
      -> std::vector<CommitmentRecommendation> = 0;
};

/**
 * @brief Report sink required by PDF report workflows.
 */
class ICliReportWriter {
public:
  virtual ~ICliReportWriter() = default;

  [[nodiscard]] virtual auto resolve_path(const std::string& requested_path,
                                          const std::string& default_filename) const -> std::filesystem::path = 0;
  virtual void write_cost(const std::filesystem::path& path,
                          const AccountInfo& account,
                          const std::vector<CostComparisonRow>& rows) const = 0;
  virtual void write_trend(const std::filesystem::path& path,
                           const AccountInfo& account,
                           const std::vector<MonthCost>& rows) const = 0;
  virtual void write_waste(const std::filesystem::path& path,
                           const AccountInfo& account,
                           const std::vector<WasteFinding>& rows) const = 0;
};

/**
 * @brief Subscription alias persistence required by alias-sub workflows.
 */
class ICliSubscriptionAliasStore {
public:
  virtual ~ICliSubscriptionAliasStore() = default;

  [[nodiscard]] virtual auto list() const -> std::vector<SubscriptionAlias> = 0;
  [[nodiscard]] virtual auto resolve(const std::string& selector) const -> std::string = 0;
  virtual void set(const std::string& alias, const std::string& subscription) const = 0;
  [[nodiscard]] virtual auto remove(const std::string& alias) const -> bool = 0;
};

/**
 * @brief Cost snapshot history required by cost and history workflows.
 */
class ICliCostHistoryStore {
public:
  virtual ~ICliCostHistoryStore() = default;

  virtual void record(const CostSnapshot& snapshot) const = 0;
  [[nodiscard]] virtual auto snapshots() const -> std::vector<CostSnapshot> = 0;
};

class IWebhookSender;

/**
 * @brief Adapter wrapping an IAzureClient to satisfy CLI account, cost, trend, and waste providers.
 */
class AzureClientAdapter : public ICliAccountProvider,
                           public ICliCostProvider,
                           public ICliTrendProvider,
                           public ICliWasteProvider,
                           public ICliBudgetProvider,
                           public ICliCommitmentProvider {
public:
  explicit AzureClientAdapter(std::shared_ptr<IAzureClient> client) : client_(std::move(client)) {}

  [[nodiscard]] auto account(const CliOptions& options) const -> AccountInfo override {
    return client_->account(options);
  }

  [[nodiscard]] auto current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> override {
    return client_->current_month_costs(options);
  }

  [[nodiscard]] auto previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> override {
    return client_->previous_month_costs(options);
  }

  [[nodiscard]] auto six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> override {
    return client_->six_month_trends(options);
  }

  [[nodiscard]] auto waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> override {
    return client_->waste_findings(options);
  }

  [[nodiscard]] auto budgets(const CliOptions& options) const -> std::vector<BudgetInfo> override {
    return client_->budgets(options);
  }

  [[nodiscard]] auto commitment_recommendations(const CliOptions& options) const
      -> std::vector<CommitmentRecommendation> override {
    return client_->commitment_recommendations(options);
  }

private:
  std::shared_ptr<IAzureClient> client_;
};

/**
 * @brief Runtime dependencies used by the CLI dispatcher.
 */
struct CliRuntime {
  std::ostream& out;
  std::ostream& err;
  const ICliAccountProvider& account_provider;
  const ICliCostProvider& cost_provider;
  const ICliTrendProvider& trend_provider;
  const ICliWasteProvider& waste_provider;
  const ICliReportWriter& report_writer;
  const ICliSubscriptionAliasStore& alias_store;
  const ICliCostHistoryStore& history_store;
  const IWebhookSender* webhook_sender{nullptr};
  std::istream* in{nullptr};
  const ICommandRunner* runner{nullptr};
  const ICliBudgetProvider* budget_provider{nullptr};
  const ICliCommitmentProvider* commitment_provider{nullptr};
};

/**
 * @brief Parses process arguments into workflow options.
 * @param args Command-line arguments without argv[0]. Global flags may appear
 * before commands or after selector arguments.
 * @return Parsed options.
 *
 * Throws std::invalid_argument for unknown commands or flags, missing flag
 * values, unsupported output formats, and out-of-range threshold values.
 */
[[nodiscard]] auto parse_args(std::span<const std::string> args) -> CliOptions;

/**
 * @brief Runs the selected workflow and writes output to stdout.
 * @param options Parsed command-line options.
 * @return Process exit code.
 */
auto run(const CliOptions& options) -> int;

/**
 * @brief Runs the selected workflow with injectable dependencies.
 * @param options Parsed command-line options.
 * @param runtime CLI dependency bundle.
 * @return Process exit code.
 */
auto run(const CliOptions& options, const CliRuntime& runtime) -> int;

/**
 * @brief Returns help text for the command-line interface.
 * @return User-facing help text.
 */
[[nodiscard]] auto help_text() -> std::string;

/**
 * @brief Converts an output format into a stable display string.
 * @param format Output format.
 * @return String representation.
 */
[[nodiscard]] auto to_string(OutputFormat format) -> std::string;

} // namespace azdash
