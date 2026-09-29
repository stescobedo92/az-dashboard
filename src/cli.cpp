#include "az_dashboard/cli.hpp"

#include "az_dashboard/analytics.hpp"
#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/azure_rest.hpp"
#include "az_dashboard/cache.hpp"
#include "az_dashboard/history.hpp"
#include "az_dashboard/render.hpp"
#include "az_dashboard/report.hpp"
#include "az_dashboard/subscription_aliases.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "az_dashboard/ui.hpp"
#include "az_dashboard/webhook.hpp"

namespace azdash {
namespace {

auto make_client(const CliOptions& options = {}) -> std::shared_ptr<IAzureClient> {
  const auto creds = AzureRestCredentials::from_env();
  if (options.use_rest || creds.is_valid()) {
    return std::make_shared<AzureRestClient>(creds, std::make_shared<CurlHttpRequester>(),
                                             std::make_shared<LocalTrendCacheStore>(default_trend_cache_path()));
  }
  return std::make_shared<AzureCliClient>(std::make_shared<ShellCommandRunner>(),
                                          std::make_shared<LocalTrendCacheStore>(default_trend_cache_path()));
}

class AzureCliRuntimeProvider final : public AzureClientAdapter {
public:
  AzureCliRuntimeProvider()
      : AzureClientAdapter(make_client()) {}
  explicit AzureCliRuntimeProvider(const CliOptions& options)
      : AzureClientAdapter(make_client(options)) {}
  explicit AzureCliRuntimeProvider(std::shared_ptr<IAzureClient> client)
      : AzureClientAdapter(std::move(client)) {}
};

class PdfReportWriter final : public ICliReportWriter {
public:
  [[nodiscard]] auto resolve_path(const std::string& requested_path,
                                  const std::string& default_filename) const -> std::filesystem::path override {
    return resolve_report_path(requested_path, default_filename);
  }

  void write_cost(const std::filesystem::path& path,
                  const AccountInfo& account,
                  const std::vector<CostComparisonRow>& rows) const override {
    write_cost_pdf(path, account, rows);
  }

  void write_trend(const std::filesystem::path& path,
                   const AccountInfo& account,
                   const std::vector<MonthCost>& rows) const override {
    write_trend_pdf(path, account, rows);
  }

  void write_waste(const std::filesystem::path& path,
                   const AccountInfo& account,
                   const std::vector<WasteFinding>& rows) const override {
    write_waste_pdf(path, account, rows);
  }
};

class LocalSubscriptionAliasStore final : public ICliSubscriptionAliasStore {
public:
  LocalSubscriptionAliasStore() : store_(default_subscription_alias_path()) {}

  [[nodiscard]] auto list() const -> std::vector<SubscriptionAlias> override {
    return store_.list();
  }

  [[nodiscard]] auto resolve(const std::string& selector) const -> std::string override {
    if (const auto alias = store_.resolve(selector)) {
      return *alias;
    }
    return selector;
  }

  void set(const std::string& alias, const std::string& subscription) const override {
    store_.set(alias, subscription);
  }

  [[nodiscard]] auto remove(const std::string& alias) const -> bool override {
    return store_.remove(alias);
  }

private:
  SubscriptionAliasStore store_;
};

class LocalCostHistoryStore final : public ICliCostHistoryStore {
public:
  LocalCostHistoryStore() : store_(default_cost_history_path()) {}

  void record(const CostSnapshot& snapshot) const override {
    store_.append(snapshot);
  }

  [[nodiscard]] auto snapshots() const -> std::vector<CostSnapshot> override {
    return store_.list();
  }

private:
  CostHistoryStore store_;
};

[[nodiscard]] auto resolve_subscription_alias(const CliOptions& options,
                                              const ICliSubscriptionAliasStore& alias_store) -> CliOptions {
  auto resolved = options;
  for (auto& sub : resolved.subscriptions) {
    sub = alias_store.resolve(sub);
  }
  return resolved;
}

[[nodiscard]] auto current_utc_timestamp() -> std::string {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

[[nodiscard]] auto subscription_label(const std::vector<std::string>& subscriptions) -> std::string {
  if (subscriptions.empty()) {
    return "default";
  }
  std::string label = subscriptions.front();
  for (auto it = subscriptions.begin() + 1; it != subscriptions.end(); ++it) {
    label += "," + *it;
  }
  return label;
}

void record_cost_snapshot(const CliOptions& resolved_options,
                          const std::vector<ServiceCost>& current,
                          const CliRuntime& runtime) {
  try {
    std::string currency = current.empty() ? "USD" : current.front().currency;
    runtime.history_store.record({current_utc_timestamp(), subscription_label(resolved_options.subscriptions),
                                  total_cost(current), current, currency});
  } catch (const std::exception& error) {
    runtime.err << "warning: could not record cost history: " << error.what() << '\n';
  }
}

auto execute_cost(const CliOptions& options, const CliRuntime& runtime) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  const auto current = runtime.cost_provider.current_month_costs(resolved_options);
  const auto previous = runtime.cost_provider.previous_month_costs(resolved_options);
  double projected_total = 0.0;
  if (options.projection_mode == ProjectionMode::Weighted) {
    const auto trends = runtime.trend_provider.six_month_trends(resolved_options);
    std::vector<double> past_totals;
    for (std::size_t i = 0; i + 1 < trends.size(); ++i) {
      past_totals.push_back(trends[i].total);
    }
    projected_total = compute_projection(total_cost(current), options.projection_mode, past_totals);
  } else {
    projected_total = compute_projection(total_cost(current));
  }
  render_costs(compare_costs(current, previous), projected_total,
               options.output, runtime.out);
  record_cost_snapshot(resolved_options, current, runtime);

  if (!options.webhook_url.empty() && runtime.webhook_sender) {
    const double total = total_cost(current);
    const std::string currency = current.empty() ? "USD" : current.front().currency;
    std::string status = "info";
    std::ostringstream msg;
    msg << std::fixed << std::setprecision(2);
    msg << "Current month cost: " << total << " " << currency;
    if (options.fail_if_exceeds_cost.has_value() && total > options.fail_if_exceeds_cost.value()) {
      status = "danger";
      msg << " (Budget EXCEEDED: limit " << options.fail_if_exceeds_cost.value() << " " << currency << ")";
    }
    std::ostringstream det;
    det << std::fixed << std::setprecision(2);
    det << "Projected month-end: " << projected_total << " " << currency;
    if (!runtime.webhook_sender->send(options.webhook_url, WebhookPayload{
        .title = "Azure Cost Alert",
        .status = status,
        .subscription = subscription_label(resolved_options.subscriptions),
        .message = msg.str(),
        .details = det.str(),
    })) {
      runtime.err << "warning: failed to send webhook alert to " << options.webhook_url << '\n';
    }
  }

  if (runtime.budget_provider && !options.budget_filter.empty()) {
    auto budgets = runtime.budget_provider->budgets(resolved_options);
    for (const auto& b : budgets) {
      if (b.name == options.budget_filter || b.name.find(options.budget_filter) != std::string::npos) {
        if (b.amount > 0.0 && projected_total > b.amount) {
          std::ostringstream warn;
          warn << std::fixed << std::setprecision(2);
          warn << "\n[!] Azure Budget Warning: Projected spend (" << projected_total << " " << b.currency
               << ") exceeds budget '" << b.name << "' (" << b.amount << " " << b.currency << ")\n";
          runtime.out << warn.str();
        }
      }
    }
  }

  if (options.fail_if_exceeds_cost.has_value()) {
      double total = total_cost(current);
      if (total > options.fail_if_exceeds_cost.value()) {
          return 2;
      }
  }
  return 0;
}

auto execute_history(const CliOptions& options, const CliRuntime& runtime) -> int {
  render_cost_history(runtime.history_store.snapshots(), options.output, runtime.out);
  return 0;
}

auto execute_link_account(const CliOptions& options, const CliRuntime& runtime) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);

  AccountInfo account;
  try {
    account = runtime.account_provider.account(resolved_options);
  } catch (const std::exception& error) {
    std::ostringstream detail;
    detail << "Could not reach the Azure CLI. Make sure the Azure CLI is installed and run 'az login' first.\n"
           << error.what();
    render_error(detail.str(), runtime.err);
    return 1;
  }

  std::ostringstream detail;
  detail << "Subscription: " << (account.subscription_name.empty() ? "<unknown>" : account.subscription_name) << " ("
         << account.subscription_id << ")\n"
         << "Tenant: " << account.tenant_id << "\n"
         << "User: " << (account.user_name.empty() ? "<unknown>" : account.user_name) << "\n"
         << "azdash will operate against this account. Use --subscription to target another one.";
  render_success("Connected to Azure", detail.str(), runtime.out);
  return 0;
}

auto execute_anomaly(const CliOptions& options, const CliRuntime& runtime) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  const auto trends = runtime.trend_provider.six_month_trends(resolved_options);

  std::vector<double> past_totals;
  for (std::size_t index = 0; index + 1 < trends.size(); ++index) {
    past_totals.push_back(trends[index].total);
  }

  const auto projected =
      trends.empty() ? 0.0 : compute_projection(trends.back().total, options.projection_mode, past_totals);
  const auto assessment = assess_cost_anomaly(past_totals, projected);
  if (!assessment.enough_data) {
    render_error("Not enough data to detect anomalies.", runtime.err);
    return 1;
  }

  std::ostringstream message;
  message << std::fixed << std::setprecision(2);
  if (assessment.anomalous) {
    message << "Anomaly detected: projected end-of-month cost " << assessment.evaluated_total
            << " deviates from the six-month baseline (mean " << assessment.mean << ", stddev "
            << assessment.stddev << ", z-score " << assessment.zscore << ").";
  } else {
    message << "No anomalies detected: projected end-of-month cost " << assessment.evaluated_total
            << " is within the six-month baseline (mean " << assessment.mean << ", stddev "
            << assessment.stddev << ", z-score " << assessment.zscore << ").";
  }
  if (!options.webhook_url.empty() && runtime.webhook_sender) {
    if (!runtime.webhook_sender->send(options.webhook_url, WebhookPayload{
        .title = "Azure Cost Anomaly Alert",
        .status = assessment.anomalous ? "warning" : "info",
        .subscription = subscription_label(resolved_options.subscriptions),
        .message = message.str(),
        .details = "Z-Score: " + std::to_string(assessment.zscore),
    })) {
      runtime.err << "warning: failed to send webhook alert to " << options.webhook_url << '\n';
    }
  }

  render_success("Cost Anomaly", message.str(), runtime.out);
  return 0;
}

auto execute_trend(const CliOptions& options, const CliRuntime& runtime) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  render_trends(runtime.trend_provider.six_month_trends(resolved_options), options.output, runtime.out);
  return 0;
}

[[nodiscard]] auto get_remediation_command(const WasteFinding& finding) -> std::string {
  if (finding.resource_type == "Microsoft.Compute/disks") {
    return "az disk delete --ids \"" + finding.resource_id + "\" --yes";
  } else if (finding.resource_type == "Microsoft.Network/publicIPAddresses") {
    return "az network public-ip delete --ids \"" + finding.resource_id + "\"";
  } else if (finding.resource_type == "Microsoft.Compute/snapshots") {
    return "az snapshot delete --ids \"" + finding.resource_id + "\"";
  } else if (finding.resource_type == "Microsoft.Network/networkSecurityGroups") {
    return "az network nsg delete --ids \"" + finding.resource_id + "\"";
  } else if (finding.resource_type == "Microsoft.Network/routeTables") {
    return "az network route-table delete --ids \"" + finding.resource_id + "\"";
  } else if (finding.resource_type == "Microsoft.Network/natGateways") {
    return "az network nat gateway delete --ids \"" + finding.resource_id + "\"";
  } else if (finding.resource_type == "Microsoft.Web/serverfarms") {
    return "az appservice plan delete --ids \"" + finding.resource_id + "\" --yes";
  } else if (finding.resource_type == "Microsoft.Compute/virtualMachines") {
    return "az vm delete --ids \"" + finding.resource_id + "\" --yes";
  }
  return "az resource delete --ids \"" + finding.resource_id + "\"";
}

[[nodiscard]] auto parse_command_tokens(const std::string& cmd_line) -> ProcessCommand {
  ProcessCommand cmd;
  std::istringstream stream(cmd_line);
  std::string token;
  while (stream >> std::quoted(token)) {
    if (cmd.executable.empty()) {
      cmd.executable = token;
    } else {
      cmd.arguments.push_back(token);
    }
  }
  return cmd;
}

[[nodiscard]] auto execute_remediation_command(const std::string& cmd_line, const ICommandRunner* runner) -> bool {
  auto cmd = parse_command_tokens(cmd_line);
  if (cmd.executable.empty()) {
    return false;
  }
  if (runner) {
    try {
      auto res = runner->run(cmd);
      return res.exit_code == 0;
    } catch (...) {
      return false;
    }
  }
  ShellCommandRunner default_runner;
  try {
    auto res = default_runner.run(cmd);
    return res.exit_code == 0;
  } catch (...) {
    return false;
  }
}

auto execute_waste(const CliOptions& options, const CliRuntime& runtime) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  auto findings = runtime.waste_provider.waste_findings(resolved_options);
  render_waste(findings, options.output, runtime.out);

  if (!options.webhook_url.empty() && runtime.webhook_sender) {
    double potential_savings = 0.0;
    for (const auto& finding : findings) {
      potential_savings += finding.estimated_monthly_savings;
    }
    const std::string currency = findings.empty() ? "USD" : findings.front().currency;
    std::ostringstream msg;
    msg << std::fixed << std::setprecision(2);
    msg << "Detected " << findings.size() << " waste items. Potential monthly savings: "
        << potential_savings << " " << currency;
    const std::string status = findings.empty() ? "info" : "warning";
    if (!runtime.webhook_sender->send(options.webhook_url, WebhookPayload{
        .title = "Azure FinOps Waste Alert",
        .status = status,
        .subscription = subscription_label(resolved_options.subscriptions),
        .message = msg.str(),
        .details = options.dry_run ? "Dry-run execution" : (!options.remediation_path.empty() ? "Remediation script generated: " + options.remediation_path : ""),
    })) {
      runtime.err << "warning: failed to send webhook alert to " << options.webhook_url << '\n';
    }
  }

  if (options.interactive) {
    auto& input_stream = runtime.in ? *runtime.in : std::cin;
    std::size_t remediated_count = 0;
    runtime.out << "\n[Interactive Remediation Mode]\n";
    runtime.out << "Found " << findings.size() << " waste items to review:\n\n";

    for (std::size_t i = 0; i < findings.size(); ++i) {
      const auto& finding = findings[i];
      runtime.out << "[" << (i + 1) << "/" << findings.size() << "] "
                  << finding.check << ": " << finding.name << " (" << finding.resource_type << ")\n"
                  << "  Recommendation: " << finding.recommendation << "\n"
                  << "  Monthly Savings: " << finding.estimated_monthly_savings << " " << finding.currency << "\n";

      const auto cmd_str = get_remediation_command(finding);
      runtime.out << "  Command: " << cmd_str << "\n"
                  << "  Apply remediation? [y/N]: ";
      runtime.out.flush();

      std::string answer;
      if (std::getline(input_stream, answer)) {
        while (!answer.empty() && std::isspace(static_cast<unsigned char>(answer.front()))) answer.erase(answer.begin());
        while (!answer.empty() && std::isspace(static_cast<unsigned char>(answer.back()))) answer.pop_back();

        if (answer == "y" || answer == "Y" || answer == "yes" || answer == "YES") {
          if (options.dry_run) {
            runtime.out << "  [Dry-run] Would execute: " << cmd_str << "\n\n";
            ++remediated_count;
          } else {
            runtime.out << "  Executing: " << cmd_str << " ...\n";
            bool success = execute_remediation_command(cmd_str, runtime.runner);
            if (success) {
              runtime.out << "  -> Remediation successful.\n\n";
              ++remediated_count;
            } else {
              runtime.out << "  -> Remediation failed.\n\n";
            }
          }
        } else {
          runtime.out << "  -> Skipped.\n\n";
        }
      }
    }
    render_success("Interactive Remediation Complete",
                   std::to_string(remediated_count) + " of " + std::to_string(findings.size()) + " resources remediated.",
                   runtime.out);
    return 0;
  }

  if (options.dry_run) {
    std::ostringstream msg;
    msg << "Dry run: detected " << findings.size() << " waste items.";
    if (!options.remediation_path.empty()) {
      msg << " Remediation script generation to " << options.remediation_path << " skipped.";
    }
    render_success("Dry Run Complete", msg.str(), runtime.out);
    return 0;
  }
  
  if (!options.remediation_path.empty()) {
      const std::filesystem::path rem_path(options.remediation_path);
      if (!rem_path.parent_path().empty()) {
        std::filesystem::create_directories(rem_path.parent_path());
      }
      std::ofstream out(rem_path);
      out << "#!/bin/bash\n\n";
      for (const auto& finding : findings) {
          if (finding.resource_type == "Microsoft.Compute/disks") {
              out << "az disk delete --ids \"" << finding.resource_id << "\" --yes\n";
          } else if (finding.resource_type == "Microsoft.Network/publicIPAddresses") {
              out << "az network public-ip delete --ids \"" << finding.resource_id << "\"\n";
          } else if (finding.resource_type == "Microsoft.Compute/snapshots") {
              out << "az snapshot delete --ids \"" << finding.resource_id << "\"\n";
          } else if (finding.resource_type == "Microsoft.Network/networkSecurityGroups") {
              out << "az network nsg delete --ids \"" << finding.resource_id << "\"\n";
          } else if (finding.resource_type == "Microsoft.Network/routeTables") {
              out << "az network route-table delete --ids \"" << finding.resource_id << "\"\n";
          } else if (finding.resource_type == "Microsoft.Network/natGateways") {
              out << "az network nat gateway delete --ids \"" << finding.resource_id << "\"\n";
          } else if (finding.resource_type == "Microsoft.Web/serverfarms") {
              out << "# az appservice plan delete --ids \"" << finding.resource_id << "\" --yes\n";
          } else if (finding.resource_type == "Microsoft.Compute/virtualMachines") {
              out << "# az vm delete --ids \"" << finding.resource_id << "\" --yes\n";
          } else {
              out << "# Recommendation: " << finding.recommendation << "\n";
              out << "# az resource delete --ids \"" << finding.resource_id << "\"\n";
          }
      }
      render_success("Remediation generated", "Remediation script saved to " + options.remediation_path, runtime.out);
  }

  return 0;
}

auto execute_cost_report(const CliOptions& options, const CliRuntime& runtime, const AccountInfo& account) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  const auto rows = compare_costs(runtime.cost_provider.current_month_costs(resolved_options),
                                  runtime.cost_provider.previous_month_costs(resolved_options));
  const auto path = runtime.report_writer.resolve_path(options.report_path, "azdash-cost.pdf");
  runtime.report_writer.write_cost(path, account, rows);
  render_success("Report written", "Cost report written to " + path.string(), runtime.out);
  return 0;
}

auto execute_trend_report(const CliOptions& options, const CliRuntime& runtime, const AccountInfo& account) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  const auto rows = runtime.trend_provider.six_month_trends(resolved_options);
  const auto path = runtime.report_writer.resolve_path(options.report_path, "azdash-trend.pdf");
  runtime.report_writer.write_trend(path, account, rows);
  render_success("Report written", "Trend report written to " + path.string(), runtime.out);
  return 0;
}

auto execute_waste_report(const CliOptions& options, const CliRuntime& runtime, const AccountInfo& account) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  const auto rows = runtime.waste_provider.waste_findings(resolved_options);
  const auto path = runtime.report_writer.resolve_path(options.report_path, "azdash-waste.pdf");
  runtime.report_writer.write_waste(path, account, rows);
  render_success("Report written", "Waste report written to " + path.string(), runtime.out);
  return 0;
}

auto execute_alias_sub(const CliOptions& options, const CliRuntime& runtime) -> int {
  switch (options.alias_action) {
  case AliasSubAction::Set:
    runtime.alias_store.set(options.alias_name, options.alias_subscription);
    render_success("Alias saved", "alias-sub '" + options.alias_name + "' is ready for --subscription", runtime.out);
    return 0;
  case AliasSubAction::Remove:
    if (runtime.alias_store.remove(options.alias_name)) {
      render_success("Alias removed", "alias-sub '" + options.alias_name + "' was removed", runtime.out);
      return 0;
    }
    render_error("alias-sub not found: " + options.alias_name, runtime.err);
    return 1;
  case AliasSubAction::List:
    render_subscription_aliases(runtime.alias_store.list(), options.output, runtime.out);
    return 0;
  }
  return 1;
}

auto execute_ui(const CliOptions& options, const CliRuntime& runtime) -> int {
  return run_tui(options, runtime);
}

auto execute_budget(const CliOptions& options, const CliRuntime& runtime) -> int {
  if (!runtime.budget_provider) {
    render_error("No budget provider available in runtime.", runtime.err);
    return 1;
  }
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  auto budgets = runtime.budget_provider->budgets(resolved_options);
  if (!options.budget_filter.empty()) {
    std::erase_if(budgets, [&](const BudgetInfo& b) {
      return b.name.find(options.budget_filter) == std::string::npos;
    });
  } else if (!options.selectors.empty()) {
    std::erase_if(budgets, [&](const BudgetInfo& b) {
      return std::ranges::none_of(options.selectors, [&](const std::string& sel) {
        return b.name.find(sel) != std::string::npos;
      });
    });
  }
  render_budgets(budgets, options.output, runtime.out);

  if (!options.webhook_url.empty() && runtime.webhook_sender) {
    std::size_t exceeded_count = 0;
    for (const auto& b : budgets) {
      if (b.amount > 0.0 && b.current_spend > b.amount) {
        ++exceeded_count;
      }
    }
    std::ostringstream msg;
    msg << "Checked " << budgets.size() << " Azure budgets. "
        << exceeded_count << " budget(s) exceeded limit.";
    const std::string status = exceeded_count > 0 ? "warning" : "info";
    if (!runtime.webhook_sender->send(options.webhook_url, WebhookPayload{
        .title = "Azure Budget Alert",
        .status = status,
        .subscription = subscription_label(resolved_options.subscriptions),
        .message = msg.str(),
        .details = std::to_string(exceeded_count) + " exceeded",
    })) {
      runtime.err << "warning: failed to send webhook alert to " << options.webhook_url << '\n';
    }
  }

  if (options.fail_if_exceeds_cost.has_value()) {
    for (const auto& b : budgets) {
      if (b.current_spend > options.fail_if_exceeds_cost.value()) {
        return 2;
      }
    }
  }

  return 0;
}

auto execute_commitments(const CliOptions& options, const CliRuntime& runtime) -> int {
  if (!runtime.commitment_provider) {
    render_error("No commitment provider available in runtime.", runtime.err);
    return 1;
  }
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  auto recs = runtime.commitment_provider->commitment_recommendations(resolved_options);
  render_commitments(recs, options.output, runtime.out);

  if (!options.webhook_url.empty() && runtime.webhook_sender) {
    double total_savings = 0.0;
    for (const auto& r : recs) {
      total_savings += r.estimated_monthly_savings;
    }
    const std::string currency = recs.empty() ? "USD" : recs.front().currency;
    std::ostringstream msg;
    msg << std::fixed << std::setprecision(2);
    msg << "Found " << recs.size() << " commitment discount recommendations. Total potential monthly savings: "
        << total_savings << " " << currency;
    if (!runtime.webhook_sender->send(options.webhook_url, WebhookPayload{
        .title = "Azure Commitment Discounts Alert",
        .status = "info",
        .subscription = subscription_label(resolved_options.subscriptions),
        .message = msg.str(),
        .details = std::to_string(recs.size()) + " recommendations",
    })) {
      runtime.err << "warning: failed to send webhook alert to " << options.webhook_url << '\n';
    }
  }

  return 0;
}

auto execute_compliance(const CliOptions& options, const CliRuntime& runtime) -> int {
  if (!runtime.compliance_provider) {
    runtime.err << "error: compliance provider unavailable\n";
    return 1;
  }

  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  auto summary = runtime.compliance_provider->tag_compliance(resolved_options);
  render_compliance(summary, options.output, runtime.out);

  if (!options.webhook_url.empty() && runtime.webhook_sender) {
    const auto payload = make_compliance_webhook_payload(summary, resolved_options);
    if (!runtime.webhook_sender->send(options.webhook_url, payload)) {
      runtime.err << "warning: failed to send webhook alert to " << options.webhook_url << '\n';
    }
  }

  if (options.min_compliance_percent > 0.0 && summary.compliance_percentage < options.min_compliance_percent) {
    runtime.err << "error: tag compliance " << std::fixed << std::setprecision(1) << summary.compliance_percentage
                << "% is below required minimum threshold " << options.min_compliance_percent << "%\n";
    return 2;
  }

  return 0;
}

using ScreenWorkflowExecutor = int (*)(const CliOptions&, const CliRuntime&);
using ReportWorkflowExecutor = int (*)(const CliOptions&, const CliRuntime&, const AccountInfo&);

struct ScreenWorkflowDefinition {
  CommandKind command;
  ScreenWorkflowExecutor execute;
};

struct ReportWorkflowDefinition {
  CommandKind command;
  ReportWorkflowExecutor execute;
};

constexpr auto screen_workflows = std::array{
    ScreenWorkflowDefinition{CommandKind::Cost, execute_cost},
    ScreenWorkflowDefinition{CommandKind::CostAnomaly, execute_anomaly},
    ScreenWorkflowDefinition{CommandKind::History, execute_history},
    ScreenWorkflowDefinition{CommandKind::LinkAccount, execute_link_account},
    ScreenWorkflowDefinition{CommandKind::Trend, execute_trend},
    ScreenWorkflowDefinition{CommandKind::Waste, execute_waste},
    ScreenWorkflowDefinition{CommandKind::UI, execute_ui},
    ScreenWorkflowDefinition{CommandKind::Budget, execute_budget},
    ScreenWorkflowDefinition{CommandKind::Commitments, execute_commitments},
    ScreenWorkflowDefinition{CommandKind::Compliance, execute_compliance},
};

constexpr auto report_workflows = std::array{
    ReportWorkflowDefinition{CommandKind::ReportCost, execute_cost_report},
    ReportWorkflowDefinition{CommandKind::ReportTrend, execute_trend_report},
    ReportWorkflowDefinition{CommandKind::ReportWaste, execute_waste_report},
};

[[nodiscard]] auto find_screen_workflow(CommandKind command) -> const ScreenWorkflowDefinition* {
  for (const auto& workflow : screen_workflows) {
    if (workflow.command == command) {
      return &workflow;
    }
  }
  return nullptr;
}

[[nodiscard]] auto find_report_workflow(CommandKind command) -> const ReportWorkflowDefinition* {
  for (const auto& workflow : report_workflows) {
    if (workflow.command == command) {
      return &workflow;
    }
  }
  return nullptr;
}

class CommandDispatcher {
public:
  explicit CommandDispatcher(const CliRuntime& runtime) : runtime_(runtime) {}

  auto execute(const CliOptions& options) const -> int {
    if (options.command == CommandKind::Help) {
      render_help_screen(runtime_.out);
      return 0;
    }

    if (options.command == CommandKind::Version) {
      render_version(runtime_.out);
      return 0;
    }

    if (options.command == CommandKind::Update) {
      render_update_guidance(runtime_.out);
      return 0;
    }

    if (options.command == CommandKind::AliasSub) {
      return execute_alias_sub(options, runtime_);
    }

    if (const auto* workflow = find_screen_workflow(options.command)) {
      return workflow->execute(options, runtime_);
    }

    if (const auto* workflow = find_report_workflow(options.command)) {
      const auto resolved_options = resolve_subscription_alias(options, runtime_.alias_store);
      const auto account = runtime_.account_provider.account(resolved_options);
      return workflow->execute(options, runtime_, account);
    }

    render_help_screen(runtime_.out);
    return 0;
  }

private:
  const CliRuntime& runtime_;
};

} // namespace

auto run(const CliOptions& options) -> int {
  auto provider = AzureCliRuntimeProvider(options);
  auto report_writer = PdfReportWriter();
  auto alias_store = LocalSubscriptionAliasStore();
  auto history_store = LocalCostHistoryStore();
  auto webhook_sender = DefaultWebhookSender();
  auto runner = ShellCommandRunner();
  auto runtime = CliRuntime{std::cout, std::cerr,     provider,     provider,
                            provider,  provider,      report_writer, alias_store,
                            history_store, &webhook_sender, &std::cin, &runner, &provider, &provider, &provider};
  return run(options, runtime);
}

auto run(const CliOptions& options, const CliRuntime& runtime) -> int {
  try {
    return CommandDispatcher(runtime).execute(options);
  } catch (const std::exception& error) {
    render_error(error.what(), runtime.err);
    return 1;
  }
}

} // namespace azdash
