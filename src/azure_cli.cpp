#include "az_dashboard/azure_cli.hpp"

#include "az_dashboard/analytics.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azdash {
namespace detail {

auto civil_date_for_month(std::chrono::year_month_day anchor, int month_offset, bool month_start) -> std::string;

} // namespace detail

namespace {

constexpr std::string_view kRedacted{"<redacted>"};

auto current_local_date() -> std::chrono::year_month_day {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  return std::chrono::year{local.tm_year + 1900} / std::chrono::month{static_cast<unsigned>(local.tm_mon + 1)} /
         std::chrono::day{static_cast<unsigned>(local.tm_mday)};
}

auto format_date(const std::chrono::year_month_day& date) -> std::string {
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << static_cast<int>(date.year()) << '-' << std::setw(2)
      << static_cast<unsigned>(date.month()) << '-' << std::setw(2) << static_cast<unsigned>(date.day());
  return out.str();
}

auto target_month(std::chrono::year_month_day anchor, int month_offset) -> std::chrono::year_month {
  return std::chrono::year_month{anchor.year(), anchor.month()} + std::chrono::months{month_offset};
}

auto month_label(int month_offset) -> std::string {
  const auto month = target_month(current_local_date(), month_offset);
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << static_cast<int>(month.year()) << '-' << std::setw(2)
      << static_cast<unsigned>(month.month());
  return out.str();
}

auto civil_date(int month_offset, bool month_start) -> std::string {
  return detail::civil_date_for_month(current_local_date(), month_offset, month_start);
}

auto json_string(const nlohmann::json& object, std::initializer_list<const char*> keys) -> std::string {
  for (const auto* key : keys) {
    if (object.contains(key) && object.at(key).is_string()) {
      return object.at(key).get<std::string>();
    }
  }
  return {};
}

auto json_number(const nlohmann::json& object, std::initializer_list<const char*> keys) -> double {
  for (const auto* key : keys) {
    if (!object.contains(key)) {
      continue;
    }
    const auto& value = object.at(key);
    if (value.is_number()) {
      return value.get<double>();
    }
    if (value.is_string()) {
      try {
        return std::stod(value.get<std::string>());
      } catch (const std::exception&) {
      }
    }
  }
  return 0.0;
}

auto normalize_usage_item(const nlohmann::json& item) -> nlohmann::json {
  if (item.contains("properties") && item.at("properties").is_object()) {
    return item.at("properties");
  }
  return item;
}

auto parse_usage_costs(const nlohmann::json& payload) -> std::vector<ServiceCost> {
  std::vector<ServiceCost> raw;
  if (!payload.is_array()) {
    return raw;
  }

  for (const auto& item : payload) {
    const auto properties = normalize_usage_item(item);
    auto service = json_string(properties, {"consumedService", "meterCategory", "serviceName", "publisherName"});
    if (service.empty()) {
      service = "Unclassified";
    }

    const auto cost = json_number(properties, {"pretaxCost", "costInBillingCurrency", "cost", "extendedCost"});
    raw.push_back({service, cost});
  }

  const auto totals = aggregate_by(raw, [](const ServiceCost& cost) { return cost.service; },
                                  [](const ServiceCost& cost) { return cost.cost; });

  std::vector<ServiceCost> costs;
  costs.reserve(totals.size());
  for (const auto& [service, cost] : totals) {
    costs.push_back({service, cost});
  }
  return costs;
}

auto append_advisor_findings(const nlohmann::json& payload, std::vector<WasteFinding>& findings) -> void {
  if (!payload.is_array()) {
    return;
  }

  for (const auto& item : payload) {
    const auto properties = item.contains("properties") ? item.at("properties") : item;
    findings.push_back({
        "advisor",
        json_string(properties, {"resourceMetadata", "resourceId"}),
        json_string(properties, {"impactedField"}),
        json_string(properties, {"impactedValue", "name"}),
        "",
        json_string(properties, {"shortDescription", "recommendationTypeId", "description"}),
        json_number(properties, {"annualSavingsAmount", "savingsAmount"}) / 12.0,
    });
  }
}

auto append_resource_heuristics(const nlohmann::json& payload, std::vector<WasteFinding>& findings) -> void {
  if (!payload.is_array()) {
    return;
  }

  for (const auto& item : payload) {
    const auto type = json_string(item, {"type"});
    const auto name = json_string(item, {"name"});
    const auto id = json_string(item, {"id"});
    const auto location = json_string(item, {"location"});

    if (type == "Microsoft.Compute/disks" && (!item.contains("managedBy") || item.at("managedBy").is_null())) {
      findings.push_back({"compute", id, type, name, location, "Managed disk is not attached to a VM.", 0.0});
    }

    if (type == "Microsoft.Compute/snapshots") {
      findings.push_back({"compute", id, type, name, location, "Review old snapshots and delete unneeded copies.", 0.0});
    }

    if (type == "Microsoft.Network/publicIPAddresses" &&
        (!item.contains("properties") || !item.at("properties").contains("ipConfiguration") ||
         item.at("properties").at("ipConfiguration").is_null())) {
      findings.push_back({"network", id, type, name, location, "Public IP address is not associated to a resource.", 0.0});
    }
  }
}

auto append_vm_heuristics(const nlohmann::json& payload, std::vector<WasteFinding>& findings) -> void {
  if (!payload.is_array()) {
    return;
  }

  for (const auto& item : payload) {
    const auto power_state = json_string(item, {"powerState"});
    if (power_state.find("deallocated") != std::string::npos || power_state.find("stopped") != std::string::npos) {
      findings.push_back({
          "compute",
          json_string(item, {"id"}),
          "Microsoft.Compute/virtualMachines",
          json_string(item, {"name"}),
          json_string(item, {"location"}),
          "VM is stopped or deallocated; validate whether it can be deleted or resized.",
          0.0,
      });
    }
  }
}

using WasteDetector = void (*)(const nlohmann::json&, std::vector<WasteFinding>&);

struct WasteScan final {
  ProcessCommand command;
  WasteDetector detect;
};

auto sensitive_argument_values(const ProcessCommand& command) -> std::vector<std::string> {
  std::vector<std::string> values;
  for (auto index = std::size_t{0}; index + 1 < command.arguments.size(); ++index) {
    if (command.arguments[index] == "--subscription" || command.arguments[index] == "--tenant") {
      values.push_back(command.arguments[index + 1]);
    }
  }
  return values;
}

auto redact_text(std::string text, const ProcessCommand& command) -> std::string {
  for (const auto& value : sensitive_argument_values(command)) {
    if (value.empty()) {
      continue;
    }
    auto position = std::size_t{0};
    while ((position = text.find(value, position)) != std::string::npos) {
      text.replace(position, value.size(), kRedacted);
      position += kRedacted.size();
    }
  }
  return text;
}

auto command_summary(const ProcessCommand& command) -> std::string {
  std::string summary = command.executable.empty() ? "<missing-executable>" : command.executable;
  auto redact_next = false;
  for (const auto& argument : command.arguments) {
    summary += ' ';
    if (redact_next) {
      summary += kRedacted;
      redact_next = false;
      continue;
    }
    summary += argument;
    redact_next = argument == "--subscription" || argument == "--tenant";
  }
  return summary;
}

auto append_process_output(std::ostringstream& message, const ProcessCommand& command, const CommandResult& result)
    -> void {
  if (!result.stdout_text.empty()) {
    message << "\nstdout: " << redact_text(result.stdout_text, command);
  }
  if (!result.stderr_text.empty()) {
    message << "\nstderr: " << redact_text(result.stderr_text, command);
  }
}

class AzureCommandBuilder final {
public:
  explicit AzureCommandBuilder(const CliOptions& options) : options_(options) {}

  [[nodiscard]] auto account_show() const -> ProcessCommand {
    return build({"account", "show"});
  }

  [[nodiscard]] auto consumption_usage(std::string start_date, std::string end_date) const -> ProcessCommand {
    return build({"consumption", "usage", "list", "--start-date", std::move(start_date), "--end-date",
                  std::move(end_date)});
  }

  [[nodiscard]] auto advisor_cost_recommendations() const -> ProcessCommand {
    return build({"advisor", "recommendation", "list", "--category", "Cost"});
  }

  [[nodiscard]] auto resource_list() const -> ProcessCommand {
    return build({"resource", "list"});
  }

  [[nodiscard]] auto vm_list_with_power_state() const -> ProcessCommand {
    return build({"vm", "list", "-d"});
  }

private:
  const CliOptions& options_;

  [[nodiscard]] auto build(std::vector<std::string> arguments) const -> ProcessCommand {
    if (!options_.subscription.empty()) {
      arguments.emplace_back("--subscription");
      arguments.push_back(options_.subscription);
    }
    if (!options_.tenant.empty()) {
      arguments.emplace_back("--tenant");
      arguments.push_back(options_.tenant);
    }
    arguments.emplace_back("-o");
    arguments.emplace_back("json");
    return {"az", std::move(arguments)};
  }
};

class AzureJsonCommandExecutor final {
public:
  explicit AzureJsonCommandExecutor(const ICommandRunner& runner) : runner_(runner) {}

  [[nodiscard]] auto run(const ProcessCommand& command) const -> nlohmann::json {
    const auto result = runner_.run(command);
    if (result.exit_code != 0) {
      std::ostringstream message;
      message << "Azure CLI command failed (" << command_summary(command) << "): exit code " << result.exit_code;
      if (result.timed_out) {
        message << " after timeout";
      }
      append_process_output(message, command, result);
      throw std::runtime_error(message.str());
    }
    try {
      return nlohmann::json::parse(result.stdout_text.empty() ? "null" : result.stdout_text);
    } catch (const nlohmann::json::exception& error) {
      std::ostringstream message;
      message << "Azure CLI returned invalid JSON (" << command_summary(command) << "): " << error.what();
      throw std::runtime_error(message.str());
    }
  }

private:
  const ICommandRunner& runner_;
};

} // namespace

namespace detail {

auto civil_date_for_month(std::chrono::year_month_day anchor, int month_offset, bool month_start) -> std::string {
  const auto month = target_month(anchor, month_offset);
  if (month_start) {
    return format_date(month / std::chrono::day{1});
  }

  const auto last_day = std::chrono::year_month_day{month / std::chrono::last}.day();
  const auto day = anchor.day() > last_day ? last_day : anchor.day();
  return format_date(month / day);
}

} // namespace detail

AzureCliClient::AzureCliClient(std::shared_ptr<ICommandRunner> runner) : runner_(std::move(runner)) {
  if (!runner_) {
    throw std::invalid_argument("runner is required");
  }
}

auto AzureCliClient::account(const CliOptions& options) const -> AccountInfo {
  const AzureCommandBuilder commands{options};
  const AzureJsonCommandExecutor executor{*runner_};
  const auto payload = executor.run(commands.account_show());
  return {
      json_string(payload, {"id"}),
      json_string(payload, {"name"}),
      json_string(payload, {"tenantId"}),
      payload.contains("user") ? json_string(payload.at("user"), {"name"}) : "",
  };
}

auto AzureCliClient::current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> {
  const AzureCommandBuilder commands{options};
  const AzureJsonCommandExecutor executor{*runner_};
  const auto start = civil_date(0, true);
  const auto end = civil_date(0, false);
  const auto payload = executor.run(commands.consumption_usage(start, end));
  return parse_usage_costs(payload);
}

auto AzureCliClient::previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> {
  const AzureCommandBuilder commands{options};
  const AzureJsonCommandExecutor executor{*runner_};
  const auto start = civil_date(-1, true);
  const auto end = civil_date(-1, false);
  const auto payload = executor.run(commands.consumption_usage(start, end));
  return parse_usage_costs(payload);
}

auto AzureCliClient::six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> {
  const AzureCommandBuilder commands{options};
  const AzureJsonCommandExecutor executor{*runner_};
  std::vector<MonthCost> trends;
  for (auto offset = -5; offset <= 0; ++offset) {
    const auto start = civil_date(offset, true);
    const auto end = offset == 0 ? civil_date(0, false) : civil_date(offset + 1, true);
    const auto payload = executor.run(commands.consumption_usage(start, end));
    auto services = parse_usage_costs(payload);
    services = filter_selected(services, options.selectors, [](const ServiceCost& cost) { return cost.service; });
    trends.push_back({month_label(offset), total_cost(services), services});
  }
  return trends;
}

auto AzureCliClient::waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> {
  const AzureCommandBuilder commands{options};
  const AzureJsonCommandExecutor executor{*runner_};
  std::vector<WasteFinding> findings;

  const std::array scans{
      WasteScan{commands.advisor_cost_recommendations(), append_advisor_findings},
      WasteScan{commands.resource_list(), append_resource_heuristics},
      WasteScan{commands.vm_list_with_power_state(), append_vm_heuristics},
  };
  for (const auto& scan : scans) {
    scan.detect(executor.run(scan.command), findings);
  }

  return filter_selected(findings, options.selectors, [](const WasteFinding& finding) { return finding.check; });
}

} // namespace azdash
