#pragma once

#include "az_dashboard/models.hpp"

#include <memory>
#include <string>

namespace azdash {

class ICommandRunner;

/**
 * @brief Notification alert payload for webhook dispatch.
 */
struct WebhookPayload {
  std::string title;
  std::string status; // "info", "warning", "danger"
  std::string subscription;
  std::string message;
  std::string details;
};

/**
 * @brief Interface for dispatching webhook alerts to HTTP endpoints.
 */
class IWebhookSender {
public:
  virtual ~IWebhookSender() = default;
  [[nodiscard]] virtual auto send(const std::string& webhook_url,
                                  const WebhookPayload& payload) const -> bool = 0;
};

/**
 * @brief Formats a WebhookPayload into a Slack incoming webhook JSON object.
 */
[[nodiscard]] auto format_slack_payload(const WebhookPayload& payload) -> std::string;

/**
 * @brief Formats a WebhookPayload into a Microsoft Teams connector card JSON object.
 */
[[nodiscard]] auto format_teams_payload(const WebhookPayload& payload) -> std::string;

/**
 * @brief Formats a generic JSON webhook payload.
 */
[[nodiscard]] auto format_generic_payload(const WebhookPayload& payload) -> std::string;

/**
 * @brief Formats a WebhookPayload for tag compliance alerts.
 */
[[nodiscard]] auto make_compliance_webhook_payload(const TagComplianceSummary& summary,
                                                   const CliOptions& options) -> WebhookPayload;

/**
 * @brief Default CLI webhook sender implementing IWebhookSender.
 */
class DefaultWebhookSender final : public IWebhookSender {
public:
  DefaultWebhookSender();
  explicit DefaultWebhookSender(std::shared_ptr<ICommandRunner> runner);

  [[nodiscard]] auto send(const std::string& webhook_url,
                          const WebhookPayload& payload) const -> bool override;

private:
  std::shared_ptr<ICommandRunner> runner_;
};

} // namespace azdash
