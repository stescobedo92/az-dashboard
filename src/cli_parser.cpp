#include "az_dashboard/cli_parser.hpp"

#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace azdash {
namespace {

auto parse_output(const std::string& value) -> OutputFormat {
  if (value == "json") {
    return OutputFormat::Json;
  }
  if (value == "csv") {
    return OutputFormat::Csv;
  }
  if (value == "markdown" || value == "md") {
    return OutputFormat::Markdown;
  }
  if (value == "table") {
    return OutputFormat::Table;
  }
  throw std::invalid_argument("unsupported output format: " + value);
}

auto parse_group_by(const std::string& value) -> GroupBy {
  if (value == "service") {
    return GroupBy::Service;
  }
  if (value == "resource-group" || value == "rg") {
    return GroupBy::ResourceGroup;
  }
  throw std::invalid_argument("unsupported group-by dimension: " + value);
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

auto parse_double(std::span<const std::string> args,
                  std::size_t& index,
                  const std::string& flag) -> double {
  if (index + 1 >= args.size()) {
    throw std::invalid_argument("missing value for " + flag);
  }

  ++index;
  const auto& raw_value = args[index];
  try {
    std::size_t pos;
    double value = std::stod(raw_value, &pos);
    if (pos != raw_value.size()) {
      throw std::invalid_argument(flag + " must be a number");
    }
    return value;
  } catch (const std::exception&) {
    throw std::invalid_argument(flag + " must be a number");
  }
}

void parse_global_flag(CliOptions& options, std::span<const std::string> args, std::size_t& index) {
  const auto& token = args[index];
  if (token == "--subscription") {
    options.subscriptions.push_back(require_value(args, index, token));
  } else if (token == "--all-subscriptions") {
    options.all_subscriptions = true;
  } else if (token == "--group-by") {
    options.group_by = parse_group_by(require_value(args, index, token));
  } else if (token == "--group-by-tag") {
    options.group_by_tags.push_back(require_value(args, index, token));
  } else if (token == "--filter-tag") {
    options.filter_tags.push_back(require_value(args, index, token));
  } else if (token == "--generate-remediation") {
    options.remediation_path = require_value(args, index, token);
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
  } else if (token == "--fail-if-exceeds") {
    options.fail_if_exceeds_cost = parse_double(args, index, token);
  } else if (token == "--no-cache") {
    options.no_cache = true;
  } else if (token == "--fast") {
    options.fast_query = true;
  } else if (token == "--dry-run") {
    options.dry_run = true;
  } else if (token == "--webhook" || token == "--webhook-url") {
    options.webhook_url = require_value(args, index, token);
  } else if (token == "--interactive" || token == "-i") {
    options.interactive = true;
  } else if (token == "--projection") {
    const auto val = require_value(args, index, token);
    if (val == "weighted") {
      options.projection_mode = ProjectionMode::Weighted;
    } else if (val == "linear") {
      options.projection_mode = ProjectionMode::Linear;
    } else {
      throw std::invalid_argument("unknown projection mode: " + val + " (expected linear or weighted)");
    }
  } else if (token == "--management-group" || token == "--mg") {
    options.management_group = require_value(args, index, token);
  } else if (token == "--budget") {
    options.budget_filter = require_value(args, index, token);
  } else if (token == "--term") {
    options.commitment_term = require_value(args, index, token);
  } else if (token == "--min-savings") {
    options.min_savings = parse_double(args, index, token);
  } else if (token == "--rest" || token == "--use-rest") {
    options.use_rest = true;
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

void parse_report_command(CliOptions& options, std::span<const std::string> args, std::size_t& index) {
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

void parse_alias_sub_command(CliOptions& options, std::span<const std::string> args, std::size_t& index) {
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

void parse_command(CliOptions& options, std::span<const std::string> args, std::size_t& index) {
  const auto command = args[index++];
  if (command == "cost") {
    options.command = CommandKind::Cost;
    parse_flags_only(options, args, index, "cost");
  } else if (command == "anomaly") {
    options.command = CommandKind::CostAnomaly;
    parse_flags_only(options, args, index, "anomaly");
  } else if (command == "ui") {
    options.command = CommandKind::UI;
    parse_flags_only(options, args, index, "ui");
  } else if (command == "history") {
    options.command = CommandKind::History;
    parse_flags_only(options, args, index, "history");
  } else if (command == "link-account" || command == "link") {
    options.command = CommandKind::LinkAccount;
    parse_flags_only(options, args, index, "link-account");
  } else if (command == "trend") {
    options.command = CommandKind::Trend;
    collect_selectors(options, args, index);
  } else if (command == "waste") {
    options.command = CommandKind::Waste;
    collect_selectors(options, args, index);
  } else if (command == "budget") {
    options.command = CommandKind::Budget;
    collect_selectors(options, args, index);
  } else if (command == "commitments" || command == "commitment" || command == "ri" || command == "reservations") {
    options.command = CommandKind::Commitments;
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

} // namespace

auto ArgumentParser::parse(std::span<const std::string> args) const -> CliOptions {
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

auto parse_args(std::span<const std::string> args) -> CliOptions {
  return ArgumentParser{}.parse(args);
}

auto help_text() -> std::string {
  return R"(azdash - Azure cost, trend, and waste diagnostics

Usage:
  azdash link-account
  azdash [global flags] cost
  azdash [global flags] budget [names...]
  azdash [global flags] commitments [skus...]
  azdash [global flags] trend [services...]
  azdash [global flags] waste [checks...]
  azdash [global flags] report cost [--path file-or-directory]
  azdash [global flags] report trend [services...] [--path file-or-directory]
  azdash [global flags] report waste [checks...] [--path file-or-directory]
  azdash alias-sub [list]
  azdash alias-sub set <alias> <subscription-id-or-name>
  azdash alias-sub remove <alias>
  azdash history
  azdash version
  azdash update

Global flags:
  --subscription <id-name-or-alias>   Azure subscription override (can be used multiple times).
  --all-subscriptions                 Analyze all accessible subscriptions.
  --management-group, --mg <id>       Target an Azure Management Group hierarchy recursively.
  --budget <name>                     Filter Azure budget by name.
  --term <1yr|3yr>                    Commitment term filter (1 Year or 3 Years).
  --min-savings <amount>              Minimum monthly savings filter for commitments.
  --rest, --use-rest                  Direct Azure REST API mode (uses OAuth2 client credentials).
  --tenant <id>                       Reserved for tenant-aware providers.
  -o, --output <table|json|csv|markdown>  Output format. Defaults to table.
  --path <file-or-directory>          Report output path.
  --group-by <service|resource-group> Cost aggregation dimension. Defaults to service.
  --group-by-tag <key>                Group costs by a specific Azure tag (overrides --group-by).
  --filter-tag <key=value>            Filter costs by Azure tag.
  --generate-remediation <path>       Generate a bash script to remediate waste findings.
  -i, --interactive                   Review waste findings and apply remediations interactively.
  --projection <linear|weighted>      Cost projection model (linear or weighted by past trends). Defaults to linear.
  --function-memory-threshold <pct>   Compatibility threshold for function checks.
  --secrets-idle-days <days>          Compatibility threshold for secret checks.
  --fail-if-exceeds <cost>            Return exit code 2 if total cost exceeds this amount.
  --no-cache                          Bypass local cache for historical trend data.
  --fast                              Use server-side JMESPath query projection to minimize payload size.
  --dry-run                           Simulate execution without modifying or creating resources.
  --webhook, --webhook-url <url>      Send alert notification payload to Slack, Teams, or generic webhook URL.

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
    case OutputFormat::Markdown:
      return "markdown";
    case OutputFormat::Table:
    default:
      return "table";
  }
}

} // namespace azdash
