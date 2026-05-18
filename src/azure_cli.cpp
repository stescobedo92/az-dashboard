#include "az_dashboard/azure_cli.hpp"

#include "az_dashboard/analytics.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace azdash {
namespace {

auto quote(std::string value) -> std::string {
  constexpr std::string_view unsafe_shell_chars{"\"\r\n&|;<>(){}[]`$\\!%"};
  if (value.find_first_of(unsafe_shell_chars) != std::string::npos) {
    throw std::invalid_argument("CLI argument contains unsafe shell characters");
  }

  std::string escaped = "\"";
  for (const auto c : value) {
    escaped += c;
  }
  escaped += '"';
  return escaped;
}

auto read_pipe(const std::string& command) -> CommandResult {
  std::array<char, 4096> buffer{};
  std::string output;

#ifdef _WIN32
  auto* pipe = _popen(command.c_str(), "r");
#else
  auto* pipe = popen(command.c_str(), "r");
#endif

  if (pipe == nullptr) {
    return {1, "", "failed to open process"};
  }

  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

#ifdef _WIN32
  const auto exit_code = _pclose(pipe);
#else
  const auto exit_code = pclose(pipe);
#endif

  return {exit_code, output, ""};
}

auto civil_date(int month_offset, bool month_start) -> std::string {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  local.tm_mon += month_offset;
  if (month_start) {
    local.tm_mday = 1;
  }
  std::mktime(&local);

  std::ostringstream out;
  out << std::put_time(&local, "%Y-%m-%d");
  return out.str();
}

auto month_label(int month_offset) -> std::string {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  local.tm_mon += month_offset;
  local.tm_mday = 1;
  std::mktime(&local);

  std::ostringstream out;
  out << std::put_time(&local, "%Y-%m");
  return out.str();
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

} // namespace

auto ShellCommandRunner::run(const std::string& command) const -> CommandResult {
  return read_pipe(command);
}

AzureCliClient::AzureCliClient(std::shared_ptr<ICommandRunner> runner) : runner_(std::move(runner)) {
  if (!runner_) {
    throw std::invalid_argument("runner is required");
  }
}

auto AzureCliClient::az_base(const CliOptions& options) const -> std::string {
  (void)options;
  return "az";
}

auto AzureCliClient::subscription_arg(const CliOptions& options) const -> std::string {
  if (!options.subscription.empty()) {
    return " --subscription " + quote(options.subscription);
  }
  return "";
}

auto AzureCliClient::run_json(const std::string& command) const -> nlohmann::json {
  const auto result = runner_->run(command);
  if (result.exit_code != 0) {
    throw std::runtime_error("Azure CLI command failed: " + command + "\n" + result.stdout_text + result.stderr_text);
  }
  try {
    return nlohmann::json::parse(result.stdout_text.empty() ? "null" : result.stdout_text);
  } catch (const nlohmann::json::exception& error) {
    throw std::runtime_error("Azure CLI returned invalid JSON for command: " + command + "\n" + error.what());
  }
}

auto AzureCliClient::account(const CliOptions& options) const -> AccountInfo {
  const auto payload = run_json(az_base(options) + " account show" + subscription_arg(options) + " -o json");
  return {
      json_string(payload, {"id"}),
      json_string(payload, {"name"}),
      json_string(payload, {"tenantId"}),
      payload.contains("user") ? json_string(payload.at("user"), {"name"}) : "",
  };
}

auto AzureCliClient::current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> {
  const auto start = civil_date(0, true);
  const auto end = civil_date(0, false);
  const auto payload = run_json(az_base(options) + " consumption usage list --start-date " + quote(start) +
                                " --end-date " + quote(end) + subscription_arg(options) + " -o json");
  return parse_usage_costs(payload);
}

auto AzureCliClient::previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> {
  const auto start = civil_date(-1, true);
  const auto end = civil_date(-1, false);
  const auto payload = run_json(az_base(options) + " consumption usage list --start-date " + quote(start) +
                                " --end-date " + quote(end) + subscription_arg(options) + " -o json");
  return parse_usage_costs(payload);
}

auto AzureCliClient::six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> {
  std::vector<MonthCost> trends;
  for (auto offset = -5; offset <= 0; ++offset) {
    const auto start = civil_date(offset, true);
    const auto end = offset == 0 ? civil_date(0, false) : civil_date(offset + 1, true);
    const auto payload = run_json(az_base(options) + " consumption usage list --start-date " + quote(start) +
                                  " --end-date " + quote(end) + subscription_arg(options) + " -o json");
    auto services = parse_usage_costs(payload);
    services = filter_selected(services, options.selectors, [](const ServiceCost& cost) { return cost.service; });
    trends.push_back({month_label(offset), total_cost(services), services});
  }
  return trends;
}

auto AzureCliClient::waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> {
  std::vector<WasteFinding> findings;

  append_advisor_findings(
      run_json(az_base(options) + " advisor recommendation list --category Cost" + subscription_arg(options) + " -o json"),
      findings);
  append_resource_heuristics(run_json(az_base(options) + " resource list" + subscription_arg(options) + " -o json"),
                             findings);
  append_vm_heuristics(run_json(az_base(options) + " vm list -d" + subscription_arg(options) + " -o json"), findings);

  return filter_selected(findings, options.selectors, [](const WasteFinding& finding) { return finding.check; });
}

} // namespace azdash
