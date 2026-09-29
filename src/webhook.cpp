#include "az_dashboard/webhook.hpp"
#include "az_dashboard/azure_cli.hpp"

#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>

namespace azdash {

auto format_slack_payload(const WebhookPayload& payload) -> std::string {
  std::string color = "#2eb886"; // Green / info
  if (payload.status == "danger" || payload.status == "error") {
    color = "#e01e5a"; // Red
  } else if (payload.status == "warning") {
    color = "#ecb22e"; // Yellow
  }

  nlohmann::json root;
  root["text"] = "*[azdash]* " + payload.title + ": " + payload.message;
  
  nlohmann::json attachment;
  attachment["color"] = color;
  attachment["fields"] = nlohmann::json::array({
      {{"title", "Subscription"}, {"value", payload.subscription.empty() ? "Default" : payload.subscription}, {"short", true}},
      {{"title", "Status"}, {"value", payload.status}, {"short", true}},
  });
  if (!payload.details.empty()) {
    attachment["fields"].push_back({{"title", "Details"}, {"value", payload.details}, {"short", false}});
  }

  root["attachments"] = nlohmann::json::array({attachment});
  return root.dump(2);
}

auto format_teams_payload(const WebhookPayload& payload) -> std::string {
  std::string theme_color = "008000";
  if (payload.status == "danger" || payload.status == "error") {
    theme_color = "E01E5A";
  } else if (payload.status == "warning") {
    theme_color = "FFA500";
  }

  nlohmann::json root;
  root["@type"] = "MessageCard";
  root["@context"] = "http://schema.org/extensions";
  root["themeColor"] = theme_color;
  root["summary"] = payload.title;

  nlohmann::json section;
  section["activityTitle"] = payload.title;
  section["activitySubtitle"] = payload.subscription.empty() ? "Default Subscription" : payload.subscription;
  section["text"] = payload.message;
  section["facts"] = nlohmann::json::array({
      {{"name", "Status"}, {"value", payload.status}},
  });
  if (!payload.details.empty()) {
    section["facts"].push_back({{"name", "Details"}, {"value", payload.details}});
  }

  root["sections"] = nlohmann::json::array({section});
  return root.dump(2);
}

auto format_generic_payload(const WebhookPayload& payload) -> std::string {
  nlohmann::json root;
  root["title"] = payload.title;
  root["status"] = payload.status;
  root["subscription"] = payload.subscription;
  root["message"] = payload.message;
  root["details"] = payload.details;
  return root.dump(2);
}

auto make_compliance_webhook_payload(const TagComplianceSummary& summary,
                                     const CliOptions& options) -> WebhookPayload {
  std::string status = "info";
  if (options.min_compliance_percent > 0.0 && summary.compliance_percentage < options.min_compliance_percent) {
    status = "danger";
  } else if (summary.compliance_percentage < 85.0) {
    status = "warning";
  }

  std::ostringstream msg;
  msg << std::fixed << std::setprecision(1);
  msg << "Tag compliance is at " << summary.compliance_percentage << "% (" << summary.compliant_resources
      << "/" << summary.total_resources << " resources compliant). Unallocated spend: "
      << std::fixed << std::setprecision(2) << summary.unallocated_spend << " " << summary.currency;

  std::ostringstream details;
  details << "Missing tags: ";
  bool first = true;
  for (const auto& [tag, count] : summary.missing_tag_counts) {
    if (!first) details << ", ";
    details << tag << " (" << count << " missing)";
    first = false;
  }

  std::string sub_label = "All subscriptions";
  if (!options.subscriptions.empty()) {
    sub_label = options.subscriptions.front();
    if (options.subscriptions.size() > 1) {
      sub_label += " (+" + std::to_string(options.subscriptions.size() - 1) + " more)";
    }
  }

  return WebhookPayload{
      .title = "Azure Tag Compliance Alert",
      .status = std::move(status),
      .subscription = std::move(sub_label),
      .message = msg.str(),
      .details = details.str(),
  };
}

auto make_audit_webhook_payload(const FinOpsAuditReport& report,
                                const CliOptions& options) -> WebhookPayload {
  std::string status = "info";
  if (report.overall_score < 50.0 || report.grade == "F") {
    status = "danger";
  } else if (report.overall_score < 80.0 || report.grade == "C" || report.grade == "D") {
    status = "warning";
  }

  std::ostringstream msg;
  msg << std::fixed << std::setprecision(1);
  msg << "FinOps Maturity Score: " << report.overall_score << "% (Grade: " << report.grade
      << " | Stage: " << report.maturity_stage << "). Spend: " << std::fixed << std::setprecision(2)
      << report.total_spend << " " << report.currency << ", Savings: " << report.potential_savings
      << " " << report.currency;

  std::ostringstream details;
  details << "Pillars: ";
  bool first = true;
  for (const auto& p : report.pillars) {
    if (!first) details << " | ";
    details << p.name << ": " << std::fixed << std::setprecision(0) << p.score << "% (" << p.status << ")";
    first = false;
  }

  std::string sub_label = "All subscriptions";
  if (!options.subscriptions.empty()) {
    sub_label = options.subscriptions.front();
    if (options.subscriptions.size() > 1) {
      sub_label += " (+" + std::to_string(options.subscriptions.size() - 1) + " more)";
    }
  }

  return WebhookPayload{
      .title = "FinOps Maturity Governance Audit Alert",
      .status = std::move(status),
      .subscription = std::move(sub_label),
      .message = msg.str(),
      .details = details.str(),
  };
}

auto make_carbon_webhook_payload(const CarbonFootprintAssessment& assessment,
                                 const CliOptions& options) -> WebhookPayload {
  std::string status = "info";
  if (options.fail_if_carbon_exceeds.has_value() &&
      assessment.total_emissions_mt > *options.fail_if_carbon_exceeds) {
    status = "danger";
  } else if (assessment.avoidable_emissions_percentage > 20.0) {
    status = "warning";
  }

  std::ostringstream msg;
  msg << std::fixed << std::setprecision(2);
  msg << "GreenOps Carbon Footprint: " << assessment.total_emissions_mt << " MT CO2e ("
      << assessment.total_emissions_kg << " kg CO2e) in region " << assessment.region
      << ". Energy: " << assessment.total_energy_kwh << " kWh.";

  std::ostringstream details;
  details << std::fixed << std::setprecision(1);
  details << "Avoidable waste carbon: " << assessment.avoidable_emissions_kg << " kg ("
          << assessment.avoidable_emissions_percentage << "%). Equivalent to "
          << std::setprecision(2) << assessment.equivalent_cars_per_year
          << " cars/year or " << assessment.equivalent_tree_seedlings << " tree seedlings.";

  std::string sub_label = "All subscriptions";
  if (!options.subscriptions.empty()) {
    sub_label = options.subscriptions.front();
    if (options.subscriptions.size() > 1) {
      sub_label += " (+" + std::to_string(options.subscriptions.size() - 1) + " more)";
    }
  }

  return WebhookPayload{
      .title = "GreenOps Cloud Carbon Footprint Alert",
      .status = std::move(status),
      .subscription = std::move(sub_label),
      .message = msg.str(),
      .details = details.str(),
  };
}

DefaultWebhookSender::DefaultWebhookSender()
    : runner_(std::make_shared<ShellCommandRunner>()) {}

DefaultWebhookSender::DefaultWebhookSender(std::shared_ptr<ICommandRunner> runner)
    : runner_(std::move(runner)) {}

auto DefaultWebhookSender::send(const std::string& webhook_url,
                                const WebhookPayload& payload) const -> bool {
  if (webhook_url.empty()) {
    return false;
  }

  std::string body;
  if (webhook_url.find("hooks.slack.com") != std::string::npos) {
    body = format_slack_payload(payload);
  } else if (webhook_url.find("office.com") != std::string::npos ||
             webhook_url.find("webhook.office") != std::string::npos) {
    body = format_teams_payload(payload);
  } else {
    body = format_generic_payload(payload);
  }

  if (body.empty()) {
    return false;
  }

  if (!runner_) {
    return true;
  }

  try {
    ProcessCommand cmd{
        .executable = "curl",
        .arguments = {"-s", "-S", "-X", "POST", "-H", "Content-Type: application/json", "-d", body, webhook_url},
    };
    auto result = runner_->run(cmd);
    return result.exit_code == 0;
  } catch (...) {
    return false;
  }
}

} // namespace azdash
