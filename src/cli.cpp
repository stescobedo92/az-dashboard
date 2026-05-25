#include "az_dashboard/cli.hpp"

#include "az_dashboard/analytics.hpp"
#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/render.hpp"
#include "az_dashboard/report.hpp"
#include "az_dashboard/subscription_aliases.hpp"

#include <array>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace azdash {
namespace {

auto parse_output(const std::string& value) -> OutputFormat {
  if (value == "json") {
    return OutputFormat::Json;
  }
  if (value == "csv") {
    return OutputFormat::Csv;
  }
  if (value == "table") {
    return OutputFormat::Table;
  }
  throw std::invalid_argument("unsupported output format: " + value);
}

auto require_value(std::span<const std::string> args, std::size_t& index, const std::string& flag) -> std::string {
  if (index + 1 >= args.size() || args[index + 1].starts_with('-')) {
    throw std::invalid_argument("missing value for " + flag);
  }
  ++index;
  return args[index];
}

auto is_flag(const std::string& value) -> bool {
  return value.starts_with('-');
}

auto is_help_token(const std::string& value) -> bool {
  return value == "help" || value == "--help" || value == "-h";
}

auto parse_bounded_int(std::span<const std::string> args,
                       std::size_t& index,
                       const std::string& flag,
                       int min_value,
                       int max_value) -> int {
  if (index + 1 >= args.size()) {
    throw std::invalid_argument("missing value for " + flag);
  }

  ++index;
  const auto& raw_value = args[index];
  int value{};
  const auto* begin = raw_value.data();
  const auto* end = begin + raw_value.size();
  const auto [parsed, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || parsed != end) {
    throw std::invalid_argument(flag + " must be an integer");
  }

  if (value < min_value || value > max_value) {
    throw std::invalid_argument(flag + " must be between " + std::to_string(min_value) + " and " +
                                std::to_string(max_value));
  }
  return value;
}

void parse_global_flag(CliOptions& options, std::span<const std::string> args, std::size_t& index) {
  const auto& token = args[index];
  if (token == "--subscription") {
    options.subscription = require_value(args, index, token);
  } else if (token == "--tenant") {
    options.tenant = require_value(args, index, token);
  } else if (token == "--output" || token == "-o") {
    options.output = parse_output(require_value(args, index, token));
  } else if (token == "--path") {
    options.report_path = require_value(args, index, token);
  } else if (token == "--function-memory-threshold" || token == "--lambda-memory-threshold") {
    options.function_memory_threshold_percent = parse_bounded_int(args, index, token, 0, 100);
  } else if (token == "--secrets-idle-days") {
    options.secrets_idle_days = parse_bounded_int(args, index, token, 0, 3650);
  } else {
    throw std::invalid_argument("unknown flag: " + token);
  }
}

void collect_selectors(CliOptions& options, std::span<const std::string> args, std::size_t start) {
  for (auto index = start; index < args.size(); ++index) {
    if (is_help_token(args[index])) {
      options.command = CommandKind::Help;
      return;
    }
    if (is_flag(args[index])) {
      parse_global_flag(options, args, index);
    } else {
      options.selectors.push_back(args[index]);
    }
  }
}

void parse_flags_only(CliOptions& options,
                      std::span<const std::string> args,
                      std::size_t start,
                      const std::string& command) {
  for (auto index = start; index < args.size(); ++index) {
    if (is_help_token(args[index])) {
      options.command = CommandKind::Help;
      return;
    }
    if (!is_flag(args[index])) {
      throw std::invalid_argument(command + " does not accept argument: " + args[index]);
    }
    parse_global_flag(options, args, index);
  }
}

auto make_client() -> AzureCliClient {
  return AzureCliClient(std::make_shared<ShellCommandRunner>());
}

class ArgumentParser {
public:
  [[nodiscard]] auto parse(std::span<const std::string> args) const -> CliOptions {
    CliOptions options;
    std::size_t index = 0;

    if (args.empty() || is_help_token(args[index])) {
      options.command = CommandKind::Help;
      return options;
    }

    while (index < args.size() && is_flag(args[index])) {
      if (is_help_token(args[index])) {
        options.command = CommandKind::Help;
        return options;
      }
      parse_global_flag(options, args, index);
      ++index;
    }

    if (index >= args.size() || is_help_token(args[index])) {
      options.command = CommandKind::Help;
      return options;
    }

    parse_command(options, args, index);
    return options;
  }

private:
  static void parse_command(CliOptions& options, std::span<const std::string> args, std::size_t& index) {
    const auto command = args[index++];
    if (command == "cost") {
      options.command = CommandKind::Cost;
      parse_flags_only(options, args, index, "cost");
    } else if (command == "trend") {
      options.command = CommandKind::Trend;
      collect_selectors(options, args, index);
    } else if (command == "waste") {
      options.command = CommandKind::Waste;
      collect_selectors(options, args, index);
    } else if (command == "version") {
      options.command = CommandKind::Version;
      parse_flags_only(options, args, index, "version");
    } else if (command == "update") {
      options.command = CommandKind::Update;
      parse_flags_only(options, args, index, "update");
    } else if (command == "report") {
      parse_report_command(options, args, index);
    } else if (command == "alias-sub") {
      parse_alias_sub_command(options, args, index);
    } else {
      throw std::invalid_argument("unknown command: " + command);
    }
  }

  static void parse_report_command(CliOptions& options, std::span<const std::string> args, std::size_t& index) {
    if (index >= args.size()) {
      throw std::invalid_argument("report requires one of: cost, trend, waste");
    }

    if (is_help_token(args[index])) {
      options.command = CommandKind::Help;
      return;
    }

    const auto report_kind = args[index++];
    if (report_kind == "cost") {
      options.command = CommandKind::ReportCost;
      parse_flags_only(options, args, index, "report cost");
    } else if (report_kind == "trend") {
      options.command = CommandKind::ReportTrend;
      collect_selectors(options, args, index);
    } else if (report_kind == "waste") {
      options.command = CommandKind::ReportWaste;
      collect_selectors(options, args, index);
    } else {
      throw std::invalid_argument("unknown report kind: " + report_kind);
    }
  }

  static void parse_alias_sub_command(CliOptions& options, std::span<const std::string> args, std::size_t& index) {
    options.command = CommandKind::AliasSub;
    if (index >= args.size() || args[index] == "list") {
      options.alias_action = AliasSubAction::List;
      if (index < args.size()) {
        ++index;
      }
      parse_flags_only(options, args, index, "alias-sub list");
      return;
    }

    const auto action = args[index++];
    if (action == "set") {
      if (index + 1 >= args.size()) {
        throw std::invalid_argument("alias-sub set requires <alias> <subscription>");
      }
      options.alias_action = AliasSubAction::Set;
      options.alias_name = args[index++];
      options.alias_subscription = args[index++];
      parse_flags_only(options, args, index, "alias-sub set");
      return;
    }

    if (action == "remove" || action == "rm") {
      if (index >= args.size()) {
        throw std::invalid_argument("alias-sub remove requires <alias>");
      }
      options.alias_action = AliasSubAction::Remove;
      options.alias_name = args[index++];
      parse_flags_only(options, args, index, "alias-sub remove");
      return;
    }

    if (index < args.size()) {
      options.alias_action = AliasSubAction::Set;
      options.alias_name = action;
      options.alias_subscription = args[index++];
      parse_flags_only(options, args, index, "alias-sub");
      return;
    }

    throw std::invalid_argument("unknown alias-sub action: " + action);
  }
};

class AzureCliRuntimeProvider final : public ICliAccountProvider,
                                      public ICliCostProvider,
                                      public ICliTrendProvider,
                                      public ICliWasteProvider {
public:
  AzureCliRuntimeProvider() : client_(make_client()) {}

  [[nodiscard]] auto account(const CliOptions& options) const -> AccountInfo override {
    return client_.account(options);
  }

  [[nodiscard]] auto current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> override {
    return client_.current_month_costs(options);
  }

  [[nodiscard]] auto previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> override {
    return client_.previous_month_costs(options);
  }

  [[nodiscard]] auto six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> override {
    return client_.six_month_trends(options);
  }

  [[nodiscard]] auto waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> override {
    return client_.waste_findings(options);
  }

private:
  AzureCliClient client_;
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

[[nodiscard]] auto resolve_subscription_alias(const CliOptions& options,
                                              const ICliSubscriptionAliasStore& alias_store) -> CliOptions {
  auto resolved = options;
  if (!resolved.subscription.empty()) {
    resolved.subscription = alias_store.resolve(resolved.subscription);
  }
  return resolved;
}

auto execute_cost(const CliOptions& options, const CliRuntime& runtime) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  render_costs(compare_costs(runtime.cost_provider.current_month_costs(resolved_options),
                             runtime.cost_provider.previous_month_costs(resolved_options)),
               options.output, runtime.out);
  return 0;
}

auto execute_trend(const CliOptions& options, const CliRuntime& runtime) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  render_trends(runtime.trend_provider.six_month_trends(resolved_options), options.output, runtime.out);
  return 0;
}

auto execute_waste(const CliOptions& options, const CliRuntime& runtime) -> int {
  const auto resolved_options = resolve_subscription_alias(options, runtime.alias_store);
  render_waste(runtime.waste_provider.waste_findings(resolved_options), options.output, runtime.out);
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
    ScreenWorkflowDefinition{CommandKind::Trend, execute_trend},
    ScreenWorkflowDefinition{CommandKind::Waste, execute_waste},
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

auto parse_args(std::span<const std::string> args) -> CliOptions {
  return ArgumentParser{}.parse(args);
}

auto run(const CliOptions& options) -> int {
  auto provider = AzureCliRuntimeProvider();
  auto report_writer = PdfReportWriter();
  auto alias_store = LocalSubscriptionAliasStore();
  auto runtime = CliRuntime{std::cout, std::cerr, provider, provider, provider, provider, report_writer, alias_store};
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

auto help_text() -> std::string {
  return R"(azdash - Azure cost, trend, and waste diagnostics

Usage:
  azdash [global flags] cost
  azdash [global flags] trend [services...]
  azdash [global flags] waste [checks...]
  azdash [global flags] report cost [--path file-or-directory]
  azdash [global flags] report trend [services...] [--path file-or-directory]
  azdash [global flags] report waste [checks...] [--path file-or-directory]
  azdash alias-sub [list]
  azdash alias-sub set <alias> <subscription-id-or-name>
  azdash alias-sub remove <alias>
  azdash version
  azdash update

Global flags:
  --subscription <id-name-or-alias>    Azure subscription override.
  --tenant <id>                       Reserved for tenant-aware providers.
  -o, --output <table|json|csv>        Output format. Defaults to table.
  --path <file-or-directory>          Report output path.
  --function-memory-threshold <pct>   Compatibility threshold for function checks.
  --secrets-idle-days <days>          Compatibility threshold for secret checks.

Waste checks:
  advisor compute network storage appservice database containers keyvault

Alias examples:
  azdash alias-sub set prod 00000000-0000-0000-0000-000000000000
  azdash --subscription prod cost
)";
}

auto to_string(OutputFormat format) -> std::string {
  switch (format) {
    case OutputFormat::Json:
      return "json";
    case OutputFormat::Csv:
      return "csv";
    case OutputFormat::Table:
    default:
      return "table";
  }
}

} // namespace azdash
