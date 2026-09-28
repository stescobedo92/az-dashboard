#include "az_dashboard/azure_cli.hpp"

#include "az_dashboard/analytics.hpp"
#include "az_dashboard/cache.hpp"
#include "az_dashboard/concurrency.hpp"

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

auto parse_tags(const nlohmann::json& object) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> tags;
  auto try_parse = [&](const nlohmann::json& obj) {
    if (obj.contains("tags")) {
      if (obj.at("tags").is_object()) {
        for (const auto& [key, value] : obj.at("tags").items()) {
          if (value.is_string()) tags[key] = value.get<std::string>();
        }
      } else if (obj.at("tags").is_string()) {
        try {
          auto parsed = nlohmann::json::parse(obj.at("tags").get<std::string>());
          if (parsed.is_object()) {
            for (const auto& [key, value] : parsed.items()) {
              if (value.is_string()) tags[key] = value.get<std::string>();
            }
          }
        } catch (...) {}
      }
    }
  };
  try_parse(object);
  if (object.contains("properties") && object.at("properties").is_object()) {
    try_parse(object.at("properties"));
  }
  return tags;
}

auto normalize_usage_item(const nlohmann::json& item) -> nlohmann::json {
  if (item.contains("properties") && item.at("properties").is_object()) {
    return item.at("properties");
  }
  return item;
}

auto resource_group_from_usage(const nlohmann::json& properties) -> std::string {
  constexpr std::string_view kUngrouped{"ungrouped"};
  const auto direct = json_string(properties, {"resourceGroup", "resourceGroupName"});
  if (!direct.empty()) {
    return detail::normalize_selector(direct);
  }

  const auto id = detail::normalize_selector(json_string(properties, {"instanceId", "resourceId"}));
  constexpr std::string_view marker{"/resourcegroups/"};
  const auto marker_start = id.find(marker);
  if (marker_start == std::string::npos) {
    return std::string{kUngrouped};
  }
  const auto name_start = marker_start + marker.size();
  const auto name_end = id.find('/', name_start);
  const auto name = id.substr(name_start, name_end == std::string::npos ? std::string::npos : name_end - name_start);
  return name.empty() ? std::string{kUngrouped} : name;
}

enum class TagFilterOp {
  Exists,
  NotExists,
  EqualAny,
  NotEqual,
};

struct CompiledTagFilter {
  std::string key;
  TagFilterOp op;
  std::vector<std::string> values;
};

[[nodiscard]] auto compile_tag_filters(std::span<const std::string> filter_tags) -> std::vector<CompiledTagFilter> {
  std::vector<CompiledTagFilter> compiled;
  compiled.reserve(filter_tags.size());
  for (const auto& filter : filter_tags) {
    if (filter.empty()) {
      continue;
    }
    if (filter.front() == '!') {
      compiled.push_back({filter.substr(1), TagFilterOp::NotExists, {}});
      continue;
    }
    const auto ne_pos = filter.find("!=");
    if (ne_pos != std::string::npos) {
      compiled.push_back({filter.substr(0, ne_pos), TagFilterOp::NotEqual, {filter.substr(ne_pos + 2)}});
      continue;
    }
    const auto eq_pos = filter.find('=');
    if (eq_pos != std::string::npos) {
      auto key = filter.substr(0, eq_pos);
      auto val_str = filter.substr(eq_pos + 1);
      std::vector<std::string> values;
      std::size_t start = 0;
      std::size_t comma = 0;
      while ((comma = val_str.find(',', start)) != std::string::npos) {
        values.push_back(val_str.substr(start, comma - start));
        start = comma + 1;
      }
      values.push_back(val_str.substr(start));
      compiled.push_back({std::move(key), TagFilterOp::EqualAny, std::move(values)});
      continue;
    }
    compiled.push_back({filter, TagFilterOp::Exists, {}});
  }
  return compiled;
}

[[nodiscard]] auto matches_tag_filters(const std::map<std::string, std::string>& tags,
                                       std::span<const CompiledTagFilter> filters) -> bool {
  for (const auto& filter : filters) {
    const auto it = tags.find(filter.key);
    switch (filter.op) {
      case TagFilterOp::Exists:
        if (it == tags.end()) {
          return false;
        }
        break;
      case TagFilterOp::NotExists:
        if (it != tags.end()) {
          return false;
        }
        break;
      case TagFilterOp::EqualAny:
        if (it == tags.end()) {
          return false;
        }
        if (std::ranges::find(filter.values, it->second) == filter.values.end()) {
          return false;
        }
        break;
      case TagFilterOp::NotEqual:
        if (it != tags.end() && !filter.values.empty() && it->second == filter.values.front()) {
          return false;
        }
        break;
    }
  }
  return true;
}

auto parse_usage_costs(const nlohmann::json& payload, const CliOptions& options) -> std::vector<ServiceCost> {
  std::vector<ServiceCost> raw;
  if (!payload.is_array()) {
    return raw;
  }

  const auto compiled_filters = compile_tag_filters(options.filter_tags);

  for (const auto& item : payload) {
    const auto properties = normalize_usage_item(item);
    auto tags = parse_tags(item);

    if (!matches_tag_filters(tags, compiled_filters)) {
      continue;
    }

    auto service = json_string(properties, {"consumedService", "meterCategory", "serviceName", "publisherName"});
    if (service.empty()) {
      service = "Unclassified";
    }

    if (options.group_by == GroupBy::ResourceGroup) {
      service = resource_group_from_usage(properties);
    }

    if (!options.group_by_tags.empty()) {
       std::string group_name = "";
       for (const auto& tag_key : options.group_by_tags) {
           if (tags.contains(tag_key)) {
               group_name += (group_name.empty() ? "" : " | ") + tags.at(tag_key);
           } else {
               group_name += (group_name.empty() ? "" : " | ") + std::string("Untagged");
           }
       }
       service = group_name;
    }

    const auto cost = json_number(properties, {"pretaxCost", "costInBillingCurrency", "cost", "extendedCost"});
    auto currency = json_string(properties, {"billingCurrency", "billingCurrencyCode", "currency"});
    if (currency.empty()) {
      currency = json_string(item, {"billingCurrency", "billingCurrencyCode", "currency"});
    }
    if (currency.empty()) {
      currency = "USD";
    }
    raw.push_back({service, cost, tags, currency});
  }

  std::map<std::string, double> totals;
  std::map<std::string, std::string> service_currencies;
  for (const auto& item : raw) {
    totals[item.service] += item.cost;
    if (item.currency != "USD" || !service_currencies.contains(item.service)) {
      service_currencies[item.service] = item.currency;
    }
  }

  std::vector<ServiceCost> costs;
  costs.reserve(totals.size());
  for (const auto& [service, cost] : totals) {
    costs.push_back({service, cost, {}, service_currencies[service]});
  }
  return costs;
}

auto append_advisor_findings(const nlohmann::json& payload, std::vector<WasteFinding>& findings) -> void {
  if (!payload.is_array()) {
    return;
  }

  for (const auto& item : payload) {
    const auto properties = item.contains("properties") ? item.at("properties") : item;
    auto currency = json_string(properties, {"currency", "savingsCurrency"});
    if (currency.empty()) {
      currency = "USD";
    }
    findings.push_back({
        "advisor",
        json_string(properties, {"resourceMetadata", "resourceId"}),
        json_string(properties, {"impactedField"}),
        json_string(properties, {"impactedValue", "name"}),
        "",
        json_string(properties, {"shortDescription", "recommendationTypeId", "description"}),
        json_number(properties, {"annualSavingsAmount", "savingsAmount"}) / 12.0,
        currency,
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
      const auto sku = item.contains("sku") && item.at("sku").is_object() ? json_string(item.at("sku"), {"name"}) : "";
      if (sku.find("Premium") != std::string::npos || sku.find("Ultra") != std::string::npos) {
        findings.push_back({"compute", id, type, name, location, "Unattached Premium/Ultra SSD disk is accruing high storage costs while unused.", 0.0});
      } else {
        findings.push_back({"compute", id, type, name, location, "Managed disk is not attached to a VM.", 0.0});
      }
    }

    if (type == "Microsoft.Compute/snapshots") {
      findings.push_back({"compute", id, type, name, location, "Review old snapshots and delete unneeded copies.", 0.0});
    }

    if (type == "Microsoft.Network/publicIPAddresses" &&
        (!item.contains("properties") || !item.at("properties").contains("ipConfiguration") ||
         item.at("properties").at("ipConfiguration").is_null())) {
      findings.push_back({"network", id, type, name, location, "Public IP address is not associated to a resource.", 0.0});
    }

    if (type == "Microsoft.Network/networkSecurityGroups") {
      const auto& props = item.contains("properties") && item.at("properties").is_object() ? item.at("properties") : item;
      const bool has_subnets = props.contains("subnets") && props.at("subnets").is_array() && !props.at("subnets").empty();
      const bool has_nics = props.contains("networkInterfaces") && props.at("networkInterfaces").is_array() && !props.at("networkInterfaces").empty();
      if (!has_subnets && !has_nics) {
        findings.push_back({"network", id, type, name, location, "Network Security Group is not associated to any subnet or NIC.", 0.0});
      }
    }

    if (type == "Microsoft.Network/routeTables") {
      const auto& props = item.contains("properties") && item.at("properties").is_object() ? item.at("properties") : item;
      const bool has_subnets = props.contains("subnets") && props.at("subnets").is_array() && !props.at("subnets").empty();
      if (!has_subnets) {
        findings.push_back({"network", id, type, name, location, "Route table is not associated with any subnet.", 0.0});
      }
    }

    if (type == "Microsoft.Network/natGateways") {
      const auto& props = item.contains("properties") && item.at("properties").is_object() ? item.at("properties") : item;
      const bool has_subnets = props.contains("subnets") && props.at("subnets").is_array() && !props.at("subnets").empty();
      if (!has_subnets) {
        findings.push_back({"network", id, type, name, location, "NAT Gateway has no subnets attached but incurs hourly gateway charges.", 32.40});
      }
    }

    if (type == "Microsoft.Web/serverfarms") {
      const auto& props = item.contains("properties") && item.at("properties").is_object() ? item.at("properties") : item;
      if (props.contains("numberOfSites") && props.at("numberOfSites").is_number_integer() && props.at("numberOfSites").get<int>() == 0) {
        findings.push_back({"appservice", id, type, name, location, "App Service Plan has 0 hosted apps but reserves dedicated compute capacity.", 0.0});
      }
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

constexpr const char* kFastUsageQuery =
    "[].{consumedService: properties.consumedService, "
    "pretaxCost: properties.pretaxCost, "
    "billingCurrency: properties.billingCurrency, "
    "resourceGroup: properties.resourceGroup, "
    "tags: properties.tags, "
    "instanceId: properties.instanceId, "
    "instanceName: instanceName}";

class AzureCommandBuilder final {
public:
  AzureCommandBuilder() = default;

  [[nodiscard]] auto account_show(const std::string& sub, const std::string& tenant) const -> ProcessCommand {
    return build({"account", "show"}, sub, tenant);
  }

  [[nodiscard]] auto account_list() const -> ProcessCommand {
    return build({"account", "list"}, "", "");
  }

  [[nodiscard]] auto consumption_usage(const std::string& sub,
                                       const std::string& tenant,
                                       std::string start_date,
                                       std::string end_date,
                                       const std::string& query = "") const -> ProcessCommand {
    std::vector<std::string> args = {"consumption", "usage", "list", "--start-date", std::move(start_date), "--end-date",
                                     std::move(end_date)};
    if (!query.empty()) {
      args.emplace_back("--query");
      args.push_back(query);
    }
    return build(std::move(args), sub, tenant);
  }

  [[nodiscard]] auto advisor_cost_recommendations(const std::string& sub, const std::string& tenant) const -> ProcessCommand {
    return build({"advisor", "recommendation", "list", "--category", "Cost"}, sub, tenant);
  }

  [[nodiscard]] auto resource_list(const std::string& sub, const std::string& tenant) const -> ProcessCommand {
    return build({"resource", "list"}, sub, tenant);
  }

  [[nodiscard]] auto vm_list_with_power_state(const std::string& sub, const std::string& tenant) const -> ProcessCommand {
    return build({"vm", "list", "-d"}, sub, tenant);
  }

  [[nodiscard]] auto management_group_show(const std::string& mg_id, const std::string& tenant = "") const -> ProcessCommand {
    return build({"account", "management-group", "show", "--name", mg_id, "--expand", "--recurse"}, "", tenant);
  }

  [[nodiscard]] auto consumption_budget(const std::string& sub, const std::string& tenant) const -> ProcessCommand {
    return build({"consumption", "budget", "list"}, sub, tenant);
  }

private:
  [[nodiscard]] auto build(std::vector<std::string> arguments, const std::string& subscription, const std::string& tenant) const -> ProcessCommand {
    if (!subscription.empty()) {
      arguments.emplace_back("--subscription");
      arguments.push_back(subscription);
    }
    if (!tenant.empty()) {
      arguments.emplace_back("--tenant");
      arguments.push_back(tenant);
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

AzureCliClient::AzureCliClient(std::shared_ptr<ICommandRunner> runner,
                               std::shared_ptr<ITrendCacheStore> cache)
    : runner_(std::move(runner)), cache_(std::move(cache)) {
  if (!runner_) {
    throw std::invalid_argument("runner is required");
  }
}

namespace {
void extract_mg_subscriptions(const nlohmann::json& node, std::vector<std::string>& out_subs) {
  if (node.is_null()) {
    return;
  }
  if (node.is_array()) {
    for (const auto& item : node) {
      extract_mg_subscriptions(item, out_subs);
    }
    return;
  }
  if (!node.is_object()) {
    return;
  }

  auto type = json_string(node, {"type"});
  auto id = json_string(node, {"id", "name"});
  if (type.find("subscriptions") != std::string::npos || id.find("/subscriptions/") != std::string::npos) {
    auto name = json_string(node, {"name"});
    if (name.empty() || name.find('/') != std::string::npos) {
      constexpr std::string_view marker{"/subscriptions/"};
      auto pos = id.find(marker);
      if (pos != std::string::npos) {
        auto sub_id = id.substr(pos + marker.size());
        auto slash = sub_id.find('/');
        name = sub_id.substr(0, slash);
      }
    }
    if (!name.empty() && std::ranges::find(out_subs, name) == out_subs.end()) {
      out_subs.push_back(name);
    }
  }

  auto check_children = [&](const nlohmann::json& obj) {
    if (obj.contains("children") && obj.at("children").is_array()) {
      for (const auto& child : obj.at("children")) {
        extract_mg_subscriptions(child, out_subs);
      }
    }
  };
  check_children(node);
  if (node.contains("properties") && node.at("properties").is_object()) {
    check_children(node.at("properties"));
  }
}

auto parse_budget_items(const nlohmann::json& payload, const CliOptions& options) -> std::vector<BudgetInfo> {
  std::vector<BudgetInfo> budgets;
  if (!payload.is_array()) {
    return budgets;
  }
  for (const auto& item : payload) {
    const auto& props = (item.contains("properties") && item.at("properties").is_object())
                            ? item.at("properties")
                            : item;

    auto name = json_string(item, {"name"});
    if (name.empty()) {
      name = json_string(props, {"name"});
    }

    if (!options.budget_filter.empty() && name.find(options.budget_filter) == std::string::npos) {
      continue;
    }

    double amount = json_number(props, {"amount"});
    double current_spend = 0.0;
    std::string currency = "USD";
    if (props.contains("currentSpend") && props.at("currentSpend").is_object()) {
      current_spend = json_number(props.at("currentSpend"), {"amount"});
      auto cur = json_string(props.at("currentSpend"), {"unit", "currency"});
      if (!cur.empty()) {
        currency = cur;
      }
    } else {
      current_spend = json_number(props, {"currentSpend"});
    }

    std::string time_grain = json_string(props, {"timeGrain"});
    if (time_grain.empty()) {
      time_grain = "Monthly";
    }

    std::string start_date;
    std::string end_date;
    if (props.contains("timePeriod") && props.at("timePeriod").is_object()) {
      start_date = json_string(props.at("timePeriod"), {"startDate"});
      end_date = json_string(props.at("timePeriod"), {"endDate"});
    }

    budgets.push_back(BudgetInfo{
        .name = std::move(name),
        .amount = amount,
        .current_spend = current_spend,
        .time_grain = std::move(time_grain),
        .start_date = std::move(start_date),
        .end_date = std::move(end_date),
        .currency = std::move(currency),
    });
  }
  return budgets;
}

auto get_target_subscriptions(const CliOptions& options, const AzureJsonCommandExecutor& executor) -> std::vector<std::string> {
  if (!options.management_group.empty()) {
    AzureCommandBuilder commands;
    auto payload = executor.run(commands.management_group_show(options.management_group, options.tenant));
    std::vector<std::string> subs;
    extract_mg_subscriptions(payload, subs);
    return subs.empty() ? std::vector<std::string>{""} : subs;
  }
  if (options.all_subscriptions) {
    AzureCommandBuilder commands;
    auto payload = executor.run(commands.account_list());
    std::vector<std::string> subs;
    if (payload.is_array()) {
      for (const auto& item : payload) {
        subs.push_back(json_string(item, {"id"}));
      }
    }
    return subs.empty() ? std::vector<std::string>{""} : subs;
  }
  if (!options.subscriptions.empty()) {
    return options.subscriptions;
  }
  return {""};
}
} // namespace

auto AzureCliClient::account(const CliOptions& options) const -> AccountInfo {
  const AzureCommandBuilder commands;
  const AzureJsonCommandExecutor executor{*runner_};
  auto subs = get_target_subscriptions(options, executor);
  const auto payload = executor.run(commands.account_show(subs.front(), options.tenant));
  return {
      json_string(payload, {"id"}),
      json_string(payload, {"name"}),
      json_string(payload, {"tenantId"}),
      payload.contains("user") ? json_string(payload.at("user"), {"name"}) : "",
  };
}

auto AzureCliClient::current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> {
  const AzureCommandBuilder commands;
  const AzureJsonCommandExecutor executor{*runner_};
  const auto start = civil_date(0, true);
  const auto end = civil_date(0, false);
  auto subs = get_target_subscriptions(options, executor);
  const std::string query = options.fast_query ? kFastUsageQuery : "";

  auto all_costs = parallel_transform(subs, [&](const std::string& sub) -> std::vector<ServiceCost> {
    const auto payload = executor.run(commands.consumption_usage(sub, options.tenant, start, end, query));
    return parse_usage_costs(payload, options);
  });

  std::vector<ServiceCost> combined;
  for (auto& costs : all_costs) {
    combined.insert(combined.end(), std::make_move_iterator(costs.begin()), std::make_move_iterator(costs.end()));
  }

  std::map<std::string, double> totals;
  std::map<std::string, std::string> service_currencies;
  for (const auto& item : combined) {
    totals[item.service] += item.cost;
    if (item.currency != "USD" || !service_currencies.contains(item.service)) {
      service_currencies[item.service] = item.currency;
    }
  }
  std::vector<ServiceCost> results;
  results.reserve(totals.size());
  for (const auto& [service, cost] : totals) {
    results.push_back({service, cost, {}, service_currencies[service]});
  }
  return results;
}

auto AzureCliClient::previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> {
  const AzureCommandBuilder commands;
  const AzureJsonCommandExecutor executor{*runner_};
  const auto start = civil_date(-1, true);
  const auto end = civil_date(-1, false);
  auto subs = get_target_subscriptions(options, executor);
  const std::string query = options.fast_query ? kFastUsageQuery : "";

  auto all_costs = parallel_transform(subs, [&](const std::string& sub) -> std::vector<ServiceCost> {
    const auto payload = executor.run(commands.consumption_usage(sub, options.tenant, start, end, query));
    return parse_usage_costs(payload, options);
  });

  std::vector<ServiceCost> combined;
  for (auto& costs : all_costs) {
    combined.insert(combined.end(), std::make_move_iterator(costs.begin()), std::make_move_iterator(costs.end()));
  }

  std::map<std::string, double> totals;
  std::map<std::string, std::string> service_currencies;
  for (const auto& item : combined) {
    totals[item.service] += item.cost;
    if (item.currency != "USD" || !service_currencies.contains(item.service)) {
      service_currencies[item.service] = item.currency;
    }
  }
  std::vector<ServiceCost> results;
  results.reserve(totals.size());
  for (const auto& [service, cost] : totals) {
    results.push_back({service, cost, {}, service_currencies[service]});
  }
  return results;
}

auto AzureCliClient::six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> {
  const AzureCommandBuilder commands;
  const AzureJsonCommandExecutor executor{*runner_};
  auto subs = get_target_subscriptions(options, executor);

  std::string dim = (options.group_by == GroupBy::ResourceGroup) ? "rg" : "service";
  if (!options.group_by_tags.empty()) {
    dim = "tag:";
    for (const auto& t : options.group_by_tags) dim += t + ",";
  }

  const std::array<int, 6> offsets = {-5, -4, -3, -2, -1, 0};
  const std::string query = options.fast_query ? kFastUsageQuery : "";

  return parallel_transform(offsets, [&](int offset) -> MonthCost {
    const auto start = civil_date(offset, true);
    const auto end = offset == 0 ? civil_date(0, false) : civil_date(offset + 1, true);
    const auto m_label = month_label(offset);

    std::vector<ServiceCost> combined;
    for (const auto& sub : subs) {
      if (offset < 0 && cache_ && !options.no_cache) {
        if (auto hit = cache_->get(sub, m_label, dim)) {
          combined.insert(combined.end(), hit->begin(), hit->end());
          continue;
        }
      }

      const auto payload = executor.run(commands.consumption_usage(sub, options.tenant, start, end, query));
      auto services = parse_usage_costs(payload, options);
      if (offset < 0 && cache_ && !options.no_cache) {
        cache_->put(sub, m_label, dim, services);
      }
      combined.insert(combined.end(), services.begin(), services.end());
    }

    std::map<std::string, double> totals;
    std::map<std::string, std::string> service_currencies;
    for (const auto& item : combined) {
      totals[item.service] += item.cost;
      if (item.currency != "USD" || !service_currencies.contains(item.service)) {
        service_currencies[item.service] = item.currency;
      }
    }
    std::vector<ServiceCost> aggregated_services;
    std::string month_currency = "USD";
    for (const auto& [service, cost] : totals) {
      aggregated_services.push_back({service, cost, {}, service_currencies[service]});
      if (service_currencies[service] != "USD") {
        month_currency = service_currencies[service];
      }
    }

    aggregated_services = filter_selected(aggregated_services, options.selectors, [](const ServiceCost& cost) { return cost.service; });
    return MonthCost{m_label, total_cost(aggregated_services), aggregated_services, month_currency};
  });
}

auto AzureCliClient::waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> {
  const AzureCommandBuilder commands;
  const AzureJsonCommandExecutor executor{*runner_};
  auto subs = get_target_subscriptions(options, executor);

  auto all_findings = parallel_transform(subs, [&](const std::string& sub) -> std::vector<WasteFinding> {
    std::vector<WasteFinding> sub_findings;
    const std::array scans{
        WasteScan{commands.advisor_cost_recommendations(sub, options.tenant), append_advisor_findings},
        WasteScan{commands.resource_list(sub, options.tenant), append_resource_heuristics},
        WasteScan{commands.vm_list_with_power_state(sub, options.tenant), append_vm_heuristics},
    };
    for (const auto& scan : scans) {
      try {
        scan.detect(executor.run(scan.command), sub_findings);
      } catch (const std::exception&) {
        // Skip failures on individual subscriptions for waste
      }
    }
    return sub_findings;
  });

  std::vector<WasteFinding> findings;
  for (auto& sf : all_findings) {
    findings.insert(findings.end(), std::make_move_iterator(sf.begin()), std::make_move_iterator(sf.end()));
  }

  return filter_selected(findings, options.selectors, [](const WasteFinding& finding) { return finding.check; });
}

auto AzureCliClient::budgets(const CliOptions& options) const -> std::vector<BudgetInfo> {
  const AzureCommandBuilder commands;
  const AzureJsonCommandExecutor executor{*runner_};
  auto subs = get_target_subscriptions(options, executor);

  auto all_budgets = parallel_transform(subs, [&](const std::string& sub) -> std::vector<BudgetInfo> {
    try {
      const auto payload = executor.run(commands.consumption_budget(sub, options.tenant));
      return parse_budget_items(payload, options);
    } catch (const std::exception&) {
      return {};
    }
  });

  std::vector<BudgetInfo> combined;
  for (auto& b_list : all_budgets) {
    combined.insert(combined.end(), std::make_move_iterator(b_list.begin()), std::make_move_iterator(b_list.end()));
  }
  return combined;
}

} // namespace azdash
