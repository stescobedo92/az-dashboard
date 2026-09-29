#include "az_dashboard/azure_rest.hpp"

#include "az_dashboard/analytics.hpp"
#include "az_dashboard/cache.hpp"
#include "az_dashboard/concurrency.hpp"

#include <cstdlib>
#include <format>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azdash {

namespace {

auto safe_env(const char* name) -> std::string {
#ifdef _WIN32
  char* buf = nullptr;
  std::size_t sz = 0;
  if (_dupenv_s(&buf, &sz, name) == 0 && buf != nullptr) {
    std::string val(buf);
    free(buf);
    return val;
  }
  return {};
#else
  const char* val = std::getenv(name);
  return val ? std::string(val) : std::string{};
#endif
}

auto parse_curl_output(const std::string& output) -> HttpResponse {
  const auto last_nl = output.rfind('\n');
  if (last_nl == std::string::npos) {
    return {0, output};
  }
  std::string code_str = output.substr(last_nl + 1);
  std::string body = output.substr(0, last_nl);
  int code = 0;
  try {
    code = std::stoi(code_str);
  } catch (...) {
  }
  return {code, std::move(body)};
}

auto extract_json_array(const nlohmann::json& payload) -> nlohmann::json {
  if (payload.is_array()) {
    return payload;
  }
  if (payload.contains("value") && payload.at("value").is_array()) {
    return payload.at("value");
  }
  return nlohmann::json::array();
}

} // namespace

auto AzureRestCredentials::from_env() -> AzureRestCredentials {
  AzureRestCredentials creds;
  creds.bearer_token = safe_env("AZURE_BEARER_TOKEN");
  if (creds.bearer_token.empty()) {
    creds.bearer_token = safe_env("AZURE_TOKEN");
  }
  creds.tenant_id = safe_env("AZURE_TENANT_ID");
  creds.client_id = safe_env("AZURE_CLIENT_ID");
  creds.client_secret = safe_env("AZURE_CLIENT_SECRET");
  return creds;
}

CurlHttpRequester::CurlHttpRequester(std::shared_ptr<ICommandRunner> runner) : runner_(std::move(runner)) {
  if (!runner_) {
    runner_ = std::make_shared<ShellCommandRunner>();
  }
}

auto CurlHttpRequester::get(const std::string& url, const std::vector<std::string>& headers) const -> HttpResponse {
  ProcessCommand cmd;
  cmd.executable = "curl";
  cmd.arguments = {"-s", "-S", "-w", "\n%{http_code}", "-X", "GET", url};
  for (const auto& h : headers) {
    cmd.arguments.emplace_back("-H");
    cmd.arguments.push_back(h);
  }
  const auto res = runner_->run(cmd);
  if (res.exit_code != 0) {
    throw std::runtime_error("HTTP GET request failed for URL " + url + ": " + res.stderr_text);
  }
  return parse_curl_output(res.stdout_text);
}

auto CurlHttpRequester::post(const std::string& url, const std::string& body,
                             const std::vector<std::string>& headers) const -> HttpResponse {
  ProcessCommand cmd;
  cmd.executable = "curl";
  cmd.arguments = {"-s", "-S", "-w", "\n%{http_code}", "-X", "POST", url, "-d", body};
  for (const auto& h : headers) {
    cmd.arguments.emplace_back("-H");
    cmd.arguments.push_back(h);
  }
  const auto res = runner_->run(cmd);
  if (res.exit_code != 0) {
    throw std::runtime_error("HTTP POST request failed for URL " + url + ": " + res.stderr_text);
  }
  return parse_curl_output(res.stdout_text);
}

AzureRestClient::AzureRestClient(AzureRestCredentials credentials,
                                 std::shared_ptr<IHttpRequester> requester,
                                 std::shared_ptr<ITrendCacheStore> cache)
    : credentials_(std::move(credentials)), requester_(std::move(requester)), cache_(std::move(cache)) {
  if (!requester_) {
    requester_ = std::make_shared<CurlHttpRequester>();
  }
}

auto AzureRestClient::acquire_token() const -> std::string {
  if (!credentials_.bearer_token.empty()) {
    return credentials_.bearer_token;
  }
  if (!cached_token_.empty()) {
    return cached_token_;
  }

  if (credentials_.tenant_id.empty() || credentials_.client_id.empty() || credentials_.client_secret.empty()) {
    throw std::runtime_error("Azure REST client requires bearer token or (tenant_id, client_id, client_secret)");
  }

  const std::string url = "https://login.microsoftonline.com/" + credentials_.tenant_id + "/oauth2/v2.0/token";
  const std::string body = "grant_type=client_credentials&client_id=" + credentials_.client_id +
                           "&client_secret=" + credentials_.client_secret +
                           "&scope=https%3A%2F%2Fmanagement.azure.com%2F.default";

  const auto resp = requester_->post(url, body, {"Content-Type: application/x-www-form-urlencoded"});
  if (resp.status_code != 200) {
    std::ostringstream err;
    err << "Failed to acquire Azure OAuth2 token (HTTP " << resp.status_code << "): " << resp.body;
    throw std::runtime_error(err.str());
  }

  try {
    const auto payload = nlohmann::json::parse(resp.body);
    if (!payload.contains("access_token") || !payload.at("access_token").is_string()) {
      throw std::runtime_error("OAuth2 token response missing access_token");
    }
    cached_token_ = payload.at("access_token").get<std::string>();
    return cached_token_;
  } catch (const nlohmann::json::exception& e) {
    throw std::runtime_error(std::string("Invalid OAuth2 JSON response: ") + e.what());
  }
}

auto AzureRestClient::request_json(const std::string& url) const -> nlohmann::json {
  const auto token = acquire_token();
  const auto resp = requester_->get(url, {"Authorization: Bearer " + token, "Accept: application/json"});
  if (resp.status_code < 200 || resp.status_code >= 300) {
    std::ostringstream err;
    err << "Azure REST request failed (HTTP " << resp.status_code << ") for " << url << ": " << resp.body;
    throw std::runtime_error(err.str());
  }
  try {
    return nlohmann::json::parse(resp.body.empty() ? "{}" : resp.body);
  } catch (const nlohmann::json::exception& e) {
    throw std::runtime_error(std::string("Invalid JSON in Azure REST response: ") + e.what());
  }
}

auto AzureRestClient::get_target_subscriptions(const CliOptions& options) const -> std::vector<std::string> {
  if (!options.subscriptions.empty()) {
    return options.subscriptions;
  }
  try {
    const auto payload = request_json("https://management.azure.com/subscriptions?api-version=2022-12-01");
    auto items = extract_json_array(payload);
    std::vector<std::string> subs;
    for (const auto& item : items) {
      if (item.contains("subscriptionId") && item.at("subscriptionId").is_string()) {
        subs.push_back(item.at("subscriptionId").get<std::string>());
      }
    }
    if (!subs.empty()) {
      return subs;
    }
  } catch (...) {
  }
  return {""};
}

auto AzureRestClient::account(const CliOptions& options) const -> AccountInfo {
  const auto subs = get_target_subscriptions(options);
  const auto sub = subs.front();
  if (sub.empty()) {
    return {"", "Azure REST Account", credentials_.tenant_id, credentials_.client_id};
  }

  const std::string url = "https://management.azure.com/subscriptions/" + sub + "?api-version=2022-12-01";
  try {
    const auto payload = request_json(url);
    std::string name = payload.contains("displayName") && payload.at("displayName").is_string()
                           ? payload.at("displayName").get<std::string>()
                           : sub;
    std::string tenant = payload.contains("tenantId") && payload.at("tenantId").is_string()
                             ? payload.at("tenantId").get<std::string>()
                             : credentials_.tenant_id;
    return {sub, std::move(name), std::move(tenant), credentials_.client_id};
  } catch (...) {
    return {sub, sub, credentials_.tenant_id, credentials_.client_id};
  }
}

auto AzureRestClient::current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> {
  const auto start = detail::civil_date(0, true);
  const auto end = detail::civil_date(0, false);
  const auto subs = get_target_subscriptions(options);

  auto all_costs = parallel_transform(subs, [&](const std::string& sub) -> std::vector<ServiceCost> {
    const std::string url = "https://management.azure.com/subscriptions/" + sub +
                            "/providers/Microsoft.Consumption/usageDetails?$filter=properties/usageStart%20ge%20'" +
                            start + "'%20and%20properties/usageEnd%20le%20'" + end + "'&api-version=2021-10-01";
    try {
      const auto payload = request_json(url);
      return detail::parse_usage_costs(extract_json_array(payload), options);
    } catch (...) {
      return {};
    }
  });

  std::vector<ServiceCost> combined;
  for (auto& c : all_costs) {
    combined.insert(combined.end(), std::make_move_iterator(c.begin()), std::make_move_iterator(c.end()));
  }

  std::map<std::string, double> totals;
  std::map<std::string, std::string> currencies;
  for (const auto& item : combined) {
    totals[item.service] += item.cost;
    if (item.currency != "USD" || !currencies.contains(item.service)) {
      currencies[item.service] = item.currency;
    }
  }

  std::vector<ServiceCost> results;
  results.reserve(totals.size());
  for (const auto& [service, cost] : totals) {
    results.push_back({service, cost, {}, currencies[service]});
  }
  return results;
}

auto AzureRestClient::previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> {
  const auto start = detail::civil_date(-1, true);
  const auto end = detail::civil_date(-1, false);
  const auto subs = get_target_subscriptions(options);

  auto all_costs = parallel_transform(subs, [&](const std::string& sub) -> std::vector<ServiceCost> {
    const std::string url = "https://management.azure.com/subscriptions/" + sub +
                            "/providers/Microsoft.Consumption/usageDetails?$filter=properties/usageStart%20ge%20'" +
                            start + "'%20and%20properties/usageEnd%20le%20'" + end + "'&api-version=2021-10-01";
    try {
      const auto payload = request_json(url);
      return detail::parse_usage_costs(extract_json_array(payload), options);
    } catch (...) {
      return {};
    }
  });

  std::vector<ServiceCost> combined;
  for (auto& c : all_costs) {
    combined.insert(combined.end(), std::make_move_iterator(c.begin()), std::make_move_iterator(c.end()));
  }

  std::map<std::string, double> totals;
  std::map<std::string, std::string> currencies;
  for (const auto& item : combined) {
    totals[item.service] += item.cost;
    if (item.currency != "USD" || !currencies.contains(item.service)) {
      currencies[item.service] = item.currency;
    }
  }

  std::vector<ServiceCost> results;
  results.reserve(totals.size());
  for (const auto& [service, cost] : totals) {
    results.push_back({service, cost, {}, currencies[service]});
  }
  return results;
}

auto AzureRestClient::six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> {
  const auto subs = get_target_subscriptions(options);
  std::string dim = (options.group_by == GroupBy::ResourceGroup) ? "rg" : "service";
  if (!options.group_by_tags.empty()) {
    dim = "tag:";
    for (const auto& t : options.group_by_tags) dim += t + ",";
  }

  const std::array<int, 6> offsets = {-5, -4, -3, -2, -1, 0};

  return parallel_transform(offsets, [&](int offset) -> MonthCost {
    const auto start = detail::civil_date(offset, true);
    const auto end = offset == 0 ? detail::civil_date(0, false) : detail::civil_date(offset + 1, true);
    const auto m_label = detail::month_label(offset);

    std::vector<ServiceCost> combined;
    for (const auto& sub : subs) {
      if (offset < 0 && cache_ && !options.no_cache) {
        if (auto hit = cache_->get(sub, m_label, dim)) {
          combined.insert(combined.end(), hit->begin(), hit->end());
          continue;
        }
      }

      const std::string url = "https://management.azure.com/subscriptions/" + sub +
                              "/providers/Microsoft.Consumption/usageDetails?$filter=properties/usageStart%20ge%20'" +
                              start + "'%20and%20properties/usageEnd%20le%20'" + end + "'&api-version=2021-10-01";
      try {
        const auto payload = request_json(url);
        auto services = detail::parse_usage_costs(extract_json_array(payload), options);
        if (offset < 0 && cache_ && !options.no_cache) {
          cache_->put(sub, m_label, dim, services);
        }
        combined.insert(combined.end(), services.begin(), services.end());
      } catch (...) {
      }
    }

    std::map<std::string, double> totals;
    std::map<std::string, std::string> currencies;
    for (const auto& item : combined) {
      totals[item.service] += item.cost;
      if (item.currency != "USD" || !currencies.contains(item.service)) {
        currencies[item.service] = item.currency;
      }
    }
    std::vector<ServiceCost> aggregated;
    std::string month_currency = "USD";
    for (const auto& [service, cost] : totals) {
      aggregated.push_back({service, cost, {}, currencies[service]});
      if (currencies[service] != "USD") {
        month_currency = currencies[service];
      }
    }
    aggregated = filter_selected(aggregated, options.selectors, [](const ServiceCost& c) { return c.service; });
    return MonthCost{m_label, total_cost(aggregated), aggregated, month_currency};
  });
}

auto AzureRestClient::waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> {
  const auto subs = get_target_subscriptions(options);

  auto all_findings = parallel_transform(subs, [&](const std::string& sub) -> std::vector<WasteFinding> {
    std::vector<WasteFinding> findings;

    // 1. Advisor Cost Recommendations
    try {
      const std::string adv_url =
          "https://management.azure.com/subscriptions/" + sub +
          "/providers/Microsoft.Advisor/recommendations?$filter=Category%20eq%20'Cost'&api-version=2020-01-01";
      const auto payload = request_json(adv_url);
      detail::append_advisor_findings(extract_json_array(payload), findings);
    } catch (...) {
    }

    // 2. Resource heuristics
    try {
      const std::string res_url =
          "https://management.azure.com/subscriptions/" + sub + "/resources?api-version=2021-04-01";
      const auto payload = request_json(res_url);
      detail::append_resource_heuristics(extract_json_array(payload), findings);
    } catch (...) {
    }

    return findings;
  });

  std::vector<WasteFinding> combined;
  for (auto& f : all_findings) {
    combined.insert(combined.end(), std::make_move_iterator(f.begin()), std::make_move_iterator(f.end()));
  }

  return filter_selected(combined, options.selectors, [](const WasteFinding& finding) { return finding.check; });
}

auto AzureRestClient::budgets(const CliOptions& options) const -> std::vector<BudgetInfo> {
  const auto subs = get_target_subscriptions(options);

  auto all_budgets = parallel_transform(subs, [&](const std::string& sub) -> std::vector<BudgetInfo> {
    const std::string url =
        "https://management.azure.com/subscriptions/" + sub + "/providers/Microsoft.Consumption/budgets?api-version=2021-10-01";
    try {
      const auto payload = request_json(url);
      return detail::parse_budget_items(extract_json_array(payload), options);
    } catch (...) {
      return {};
    }
  });

  std::vector<BudgetInfo> combined;
  for (auto& b : all_budgets) {
    combined.insert(combined.end(), std::make_move_iterator(b.begin()), std::make_move_iterator(b.end()));
  }
  return combined;
}

auto AzureRestClient::commitment_recommendations(const CliOptions& options) const
    -> std::vector<CommitmentRecommendation> {
  const auto subs = get_target_subscriptions(options);

  auto all_recs = parallel_transform(subs, [&](const std::string& sub) -> std::vector<CommitmentRecommendation> {
    std::vector<CommitmentRecommendation> sub_recs;

    // 1. Consumption Reservation Recommendations
    try {
      const std::string url = "https://management.azure.com/subscriptions/" + sub +
                              "/providers/Microsoft.Consumption/reservationRecommendations?api-version=2021-10-01";
      const auto payload = request_json(url);
      const auto items = extract_json_array(payload);
      for (const auto& item : items) {
        const auto& props = item.contains("properties") ? item.at("properties") : item;
        std::string sku = props.contains("skuName") && props.at("skuName").is_string()
                              ? props.at("skuName").get<std::string>()
                              : "";
        std::string term = props.contains("term") && props.at("term").is_string()
                               ? detail::normalize_term(props.at("term").get<std::string>())
                               : "1 Year";
        std::string currency = props.contains("currency") && props.at("currency").is_string()
                                   ? props.at("currency").get<std::string>()
                                   : "USD";

        double savings = props.contains("netSavings") && props.at("netSavings").is_number()
                             ? props.at("netSavings").get<double>()
                             : 0.0;
        double cost = props.contains("totalCostWithReservedInstances") &&
                              props.at("totalCostWithReservedInstances").is_number()
                          ? props.at("totalCostWithReservedInstances").get<double>()
                          : 0.0;
        std::string region = props.contains("region") && props.at("region").is_string()
                                 ? props.at("region").get<std::string>()
                                 : "Global";

        sub_recs.push_back({
            .id = item.contains("id") ? item.at("id").get<std::string>() : "",
            .type = "ReservedInstance",
            .resource_type = "Microsoft.Compute/virtualMachines",
            .sku = std::move(sku),
            .region = std::move(region),
            .term = std::move(term),
            .estimated_monthly_savings = savings,
            .estimated_monthly_cost = cost,
            .currency = std::move(currency),
            .details = "Azure REST reservation recommendation.",
        });
      }
    } catch (...) {
    }

    return sub_recs;
  });

  std::vector<CommitmentRecommendation> combined;
  for (auto& recs : all_recs) {
    combined.insert(combined.end(), std::make_move_iterator(recs.begin()), std::make_move_iterator(recs.end()));
  }

  std::vector<CommitmentRecommendation> filtered;
  for (auto& r : combined) {
    if (options.min_savings > 0.0 && r.estimated_monthly_savings < options.min_savings) {
      continue;
    }
    if (!options.commitment_term.empty()) {
      if (r.term != detail::normalize_term(options.commitment_term)) {
        continue;
      }
    }
    filtered.push_back(std::move(r));
  }

  if (!options.selectors.empty()) {
    filtered = filter_selected(filtered, options.selectors, [](const CommitmentRecommendation& rec) {
      return rec.sku.empty() ? rec.type : rec.sku;
    });
  }

  return filtered;
}

auto AzureRestClient::tag_compliance(const CliOptions& options) const -> TagComplianceSummary {
  const auto start = detail::civil_date(0, true);
  const auto end = detail::civil_date(0, false);
  const auto subs = get_target_subscriptions(options);

  auto all_summaries = parallel_transform(subs, [&](const std::string& sub) -> TagComplianceSummary {
    const std::string url = "https://management.azure.com/subscriptions/" + sub +
                            "/providers/Microsoft.Consumption/usageDetails?$filter=properties/usageStart%20ge%20'" +
                            start + "'%20and%20properties/usageEnd%20le%20'" + end + "'&api-version=2021-10-01";
    try {
      const auto payload = request_json(url);
      return detail::evaluate_tag_compliance(extract_json_array(payload), options.required_tags);
    } catch (...) {
      return {};
    }
  });

  TagComplianceSummary combined;
  if (!all_summaries.empty()) {
    combined.currency = all_summaries.front().currency;
  }
  for (auto& s : all_summaries) {
    combined.total_resources += s.total_resources;
    combined.compliant_resources += s.compliant_resources;
    combined.non_compliant_resources += s.non_compliant_resources;
    combined.total_spend += s.total_spend;
    combined.allocated_spend += s.allocated_spend;
    combined.unallocated_spend += s.unallocated_spend;
    for (const auto& [tag, count] : s.missing_tag_counts) {
      combined.missing_tag_counts[tag] += count;
    }
    for (const auto& [tag, cost] : s.missing_tag_costs) {
      combined.missing_tag_costs[tag] += cost;
    }
    combined.non_compliant_items.insert(combined.non_compliant_items.end(),
                                        std::make_move_iterator(s.non_compliant_items.begin()),
                                        std::make_move_iterator(s.non_compliant_items.end()));
  }

  combined.compliance_percentage = combined.total_resources > 0
                                       ? (100.0 * static_cast<double>(combined.compliant_resources) /
                                          static_cast<double>(combined.total_resources))
                                       : 100.0;
  return combined;
}

} // namespace azdash
