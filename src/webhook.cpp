#include "az_dashboard/webhook.hpp"
#include "az_dashboard/azure_cli.hpp"

#include <nlohmann/json.hpp>

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
