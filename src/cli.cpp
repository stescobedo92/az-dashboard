#include "az_dashboard/cli.hpp"

#include "az_dashboard/analytics.hpp"
#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/render.hpp"
#include "az_dashboard/report.hpp"

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
  if (index + 1 >= args.size()) {
    throw std::invalid_argument("missing value for " + flag);
  }
  ++index;
  return args[index];
}

auto is_flag(const std::string& value) -> bool {
  return value.starts_with("--");
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
    options.function_memory_threshold_percent = std::stoi(require_value(args, index, token));
  } else if (token == "--secrets-idle-days") {
    options.secrets_idle_days = std::stoi(require_value(args, index, token));
  } else {
    throw std::invalid_argument("unknown flag: " + token);
  }
}

void collect_selectors(CliOptions& options, std::span<const std::string> args, std::size_t start) {
  for (auto index = start; index < args.size(); ++index) {
    if (is_flag(args[index])) {
      parse_global_flag(options, args, index);
    } else {
      options.selectors.push_back(args[index]);
    }
  }
}

auto make_client() -> AzureCliClient {
  return AzureCliClient(std::make_shared<ShellCommandRunner>());
}

} // namespace

auto parse_args(std::span<const std::string> args) -> CliOptions {
  CliOptions options;
  std::size_t index = 0;

  while (index < args.size() && is_flag(args[index])) {
    parse_global_flag(options, args, index);
    ++index;
  }

  if (index >= args.size() || args[index] == "help" || args[index] == "--help" || args[index] == "-h") {
    options.command = CommandKind::Help;
    return options;
  }

  const auto command = args[index++];
  if (command == "cost") {
    options.command = CommandKind::Cost;
  } else if (command == "trend") {
    options.command = CommandKind::Trend;
    collect_selectors(options, args, index);
  } else if (command == "waste") {
    options.command = CommandKind::Waste;
    collect_selectors(options, args, index);
  } else if (command == "version") {
    options.command = CommandKind::Version;
  } else if (command == "update") {
    options.command = CommandKind::Update;
  } else if (command == "report") {
    if (index >= args.size()) {
      throw std::invalid_argument("report requires one of: cost, trend, waste");
    }
    const auto report_kind = args[index++];
    if (report_kind == "cost") {
      options.command = CommandKind::ReportCost;
    } else if (report_kind == "trend") {
      options.command = CommandKind::ReportTrend;
      collect_selectors(options, args, index);
    } else if (report_kind == "waste") {
      options.command = CommandKind::ReportWaste;
      collect_selectors(options, args, index);
    } else {
      throw std::invalid_argument("unknown report kind: " + report_kind);
    }
  } else {
    throw std::invalid_argument("unknown command: " + command);
  }

  while (index < args.size()) {
    if (is_flag(args[index])) {
      parse_global_flag(options, args, index);
    }
    ++index;
  }

  return options;
}

auto run(const CliOptions& options) -> int {
  try {
    if (options.command == CommandKind::Help) {
      std::cout << help_text();
      return 0;
    }

    if (options.command == CommandKind::Version) {
      render_version(std::cout);
      return 0;
    }

    if (options.command == CommandKind::Update) {
      std::cout << "Install the latest release with vcpkg, Docker, or the repository release artifacts.\n";
      std::cout << "vcpkg update && vcpkg upgrade az-dashboard\n";
      return 0;
    }

    auto client = make_client();
    auto account = client.account(options);

    switch (options.command) {
      case CommandKind::Cost: {
        render_costs(compare_costs(client.current_month_costs(options), client.previous_month_costs(options)),
                     options.output, std::cout);
        break;
      }
      case CommandKind::Trend: {
        render_trends(client.six_month_trends(options), options.output, std::cout);
        break;
      }
      case CommandKind::Waste: {
        render_waste(client.waste_findings(options), options.output, std::cout);
        break;
      }
      case CommandKind::ReportCost: {
        const auto rows = compare_costs(client.current_month_costs(options), client.previous_month_costs(options));
        const auto path = resolve_report_path(options.report_path, "azdash-cost.pdf");
        write_cost_pdf(path, account, rows);
        std::cout << "Report written to " << path.string() << '\n';
        break;
      }
      case CommandKind::ReportTrend: {
        const auto rows = client.six_month_trends(options);
        const auto path = resolve_report_path(options.report_path, "azdash-trend.pdf");
        write_trend_pdf(path, account, rows);
        std::cout << "Report written to " << path.string() << '\n';
        break;
      }
      case CommandKind::ReportWaste: {
        const auto rows = client.waste_findings(options);
        const auto path = resolve_report_path(options.report_path, "azdash-waste.pdf");
        write_waste_pdf(path, account, rows);
        std::cout << "Report written to " << path.string() << '\n';
        break;
      }
      default:
        std::cout << help_text();
        break;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
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
  azdash version
  azdash update

Global flags:
  --subscription <id-or-name>          Azure subscription override.
  --tenant <id>                       Reserved for tenant-aware providers.
  -o, --output <table|json|csv>        Output format. Defaults to table.
  --path <file-or-directory>          Report output path.
  --function-memory-threshold <pct>   Compatibility threshold for function checks.
  --secrets-idle-days <days>          Compatibility threshold for secret checks.

Waste checks:
  advisor compute network storage appservice database containers keyvault
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
