#include "az_dashboard/webhook.hpp"
#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/cli.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

namespace {

class FakeWebhookCommandRunner final : public azdash::ICommandRunner {
public:
  mutable std::vector<azdash::ProcessCommand> executed_commands;
  int return_code{0};
  bool should_throw{false};

  [[nodiscard]] auto run(const azdash::ProcessCommand& command,
                         const azdash::ProcessRunnerOptions& = {}) const -> azdash::CommandResult override {
    executed_commands.push_back(command);
    if (should_throw) {
      throw std::runtime_error("network timeout");
    }
    return {.exit_code = return_code, .stdout_text = "ok", .stderr_text = ""};
  }
};

class MockWebhookSender final : public azdash::IWebhookSender {
public:
  mutable std::vector<std::pair<std::string, azdash::WebhookPayload>> sent_payloads;
  bool return_value{true};

  [[nodiscard]] auto send(const std::string& webhook_url,
                          const azdash::WebhookPayload& payload) const -> bool override {
    sent_payloads.emplace_back(webhook_url, payload);
    return return_value;
  }
};

class DummyStore final : public azdash::ICliSubscriptionAliasStore,
                         public azdash::ICliCostHistoryStore,
                         public azdash::ICliReportWriter {
public:
  [[nodiscard]] auto list() const -> std::vector<azdash::SubscriptionAlias> override { return {}; }
  [[nodiscard]] auto resolve(const std::string& selector) const -> std::string override { return selector; }
  void set(const std::string&, const std::string&) const override {}
  [[nodiscard]] auto remove(const std::string&) const -> bool override { return true; }

  void record(const azdash::CostSnapshot&) const override {}
  [[nodiscard]] auto snapshots() const -> std::vector<azdash::CostSnapshot> override { return {}; }

  [[nodiscard]] auto resolve_path(const std::string& path, const std::string&) const -> std::filesystem::path override {
    return path;
  }
  void write_cost(const std::filesystem::path&, const azdash::AccountInfo&, const std::vector<azdash::CostComparisonRow>&) const override {}
  void write_trend(const std::filesystem::path&, const azdash::AccountInfo&, const std::vector<azdash::MonthCost>&) const override {}
  void write_waste(const std::filesystem::path&, const azdash::AccountInfo&, const std::vector<azdash::WasteFinding>&) const override {}
};

class MockAzureClient final : public azdash::IAzureClient {
public:
  mutable int account_calls{0};
  mutable int current_month_calls{0};
  mutable int previous_month_calls{0};
  mutable int trend_calls{0};
  mutable int waste_calls{0};

  [[nodiscard]] auto account(const azdash::CliOptions&) const -> azdash::AccountInfo override {
    ++account_calls;
    return {.subscription_id = "sub-123", .subscription_name = "MockSub", .tenant_id = "tenant-1", .user_name = "test@example.com"};
  }

  [[nodiscard]] auto current_month_costs(const azdash::CliOptions&) const -> std::vector<azdash::ServiceCost> override {
    ++current_month_calls;
    return {
        {.service = "Virtual Machines", .cost = 250.0, .currency = "USD"},
        {.service = "Storage Accounts", .cost = 50.0, .currency = "USD"},
    };
  }

  [[nodiscard]] auto previous_month_costs(const azdash::CliOptions&) const -> std::vector<azdash::ServiceCost> override {
    ++previous_month_calls;
    return {
        {.service = "Virtual Machines", .cost = 200.0, .currency = "USD"},
        {.service = "Storage Accounts", .cost = 45.0, .currency = "USD"},
    };
  }

  [[nodiscard]] auto six_month_trends(const azdash::CliOptions&) const -> std::vector<azdash::MonthCost> override {
    ++trend_calls;
    return {
        {.month = "2026-04", .total = 100.0, .services = {}, .currency = "USD"},
        {.month = "2026-05", .total = 105.0, .services = {}, .currency = "USD"},
        {.month = "2026-06", .total = 110.0, .services = {}, .currency = "USD"},
        {.month = "2026-07", .total = 108.0, .services = {}, .currency = "USD"},
        {.month = "2026-08", .total = 112.0, .services = {}, .currency = "USD"},
        {.month = "2026-09", .total = 300.0, .services = {}, .currency = "USD"}, // Anomaly
    };
  }

  [[nodiscard]] auto waste_findings(const azdash::CliOptions&) const -> std::vector<azdash::WasteFinding> override {
    ++waste_calls;
    return {
        {.check = "unattached-disk",
         .resource_id = "/subscriptions/sub-123/resourceGroups/rg1/providers/Microsoft.Compute/disks/unattached-disk",
         .resource_type = "Microsoft.Compute/disks",
         .name = "unattached-disk",
         .location = "eastus",
         .recommendation = "Delete unattached disk",
         .estimated_monthly_savings = 32.50,
         .currency = "USD"},
    };
  }

  [[nodiscard]] auto budgets(const azdash::CliOptions&) const -> std::vector<azdash::BudgetInfo> override {
    return {
        {.name = "DefaultBudget", .amount = 1000.0, .current_spend = 300.0, .time_grain = "Monthly", .currency = "USD"}
    };
  }

  [[nodiscard]] auto commitment_recommendations(const azdash::CliOptions&) const
      -> std::vector<azdash::CommitmentRecommendation> override {
    return {
        {.id = "rec-1",
         .type = "ReservedInstance",
         .resource_type = "Microsoft.Compute/virtualMachines",
         .sku = "Standard_D4s_v5",
         .region = "eastus",
         .term = "1 Year",
         .estimated_monthly_savings = 85.0,
         .estimated_monthly_cost = 140.0,
         .currency = "USD",
         .details = "1 Year reservation recommendation"}
    };
  }
};

} // namespace

TEST(WebhookFormattingTest, FormatsSlackPayloadWithWarningAndDangerColors) {
  azdash::WebhookPayload payload{
      .title = "High Spend Alert",
      .status = "danger",
      .subscription = "Production",
      .message = "Monthly spend exceeded threshold",
      .details = "Current: $500.00, Threshold: $400.00",
  };

  const auto json_str = azdash::format_slack_payload(payload);
  const auto parsed = nlohmann::json::parse(json_str);

  EXPECT_TRUE(parsed.contains("attachments"));
  ASSERT_EQ(parsed["attachments"].size(), 1u);
  EXPECT_EQ(parsed["attachments"][0]["color"], "#e01e5a"); // Red for danger

  payload.status = "warning";
  const auto warn_str = azdash::format_slack_payload(payload);
  const auto warn_parsed = nlohmann::json::parse(warn_str);
  EXPECT_EQ(warn_parsed["attachments"][0]["color"], "#ecb22e"); // Yellow for warning

  payload.status = "info";
  const auto info_str = azdash::format_slack_payload(payload);
  const auto info_parsed = nlohmann::json::parse(info_str);
  EXPECT_EQ(info_parsed["attachments"][0]["color"], "#2eb886"); // Green for info
}

TEST(WebhookFormattingTest, FormatsTeamsPayloadCorrectly) {
  azdash::WebhookPayload payload{
      .title = "Anomaly Detected",
      .status = "warning",
      .subscription = "sub-xyz",
      .message = "Cost anomaly detected for current period",
      .details = "Z-Score: 2.85",
  };

  const auto json_str = azdash::format_teams_payload(payload);
  const auto parsed = nlohmann::json::parse(json_str);

  EXPECT_EQ(parsed["@type"], "MessageCard");
  EXPECT_EQ(parsed["themeColor"], "FFA500");
  EXPECT_EQ(parsed["summary"], "Anomaly Detected");
  ASSERT_EQ(parsed["sections"].size(), 1u);
  EXPECT_EQ(parsed["sections"][0]["activitySubtitle"], "sub-xyz");
}

TEST(WebhookFormattingTest, FormatsGenericPayloadCorrectly) {
  azdash::WebhookPayload payload{
      .title = "FinOps Report",
      .status = "info",
      .subscription = "all",
      .message = "Summary generated",
      .details = "3 waste items found",
  };

  const auto json_str = azdash::format_generic_payload(payload);
  const auto parsed = nlohmann::json::parse(json_str);

  EXPECT_EQ(parsed["title"], "FinOps Report");
  EXPECT_EQ(parsed["status"], "info");
  EXPECT_EQ(parsed["subscription"], "all");
  EXPECT_EQ(parsed["message"], "Summary generated");
  EXPECT_EQ(parsed["details"], "3 waste items found");
}

TEST(DefaultWebhookSenderTest, RejectsEmptyUrl) {
  azdash::DefaultWebhookSender sender;
  azdash::WebhookPayload payload{.title = "Test"};
  EXPECT_FALSE(sender.send("", payload));
}

TEST(DefaultWebhookSenderTest, ExecutesCurlWithProperArgumentsForSlack) {
  auto runner = std::make_shared<FakeWebhookCommandRunner>();
  azdash::DefaultWebhookSender sender(runner);

  azdash::WebhookPayload payload{
      .title = "Test Slack",
      .status = "info",
      .subscription = "dev-sub",
      .message = "Routine check",
      .details = "",
  };

  const std::string slack_url = "https://hooks.slack.com/services/T00/B00/X00";
  const bool result = sender.send(slack_url, payload);

  EXPECT_TRUE(result);
  ASSERT_EQ(runner->executed_commands.size(), 1u);
  const auto& cmd = runner->executed_commands.front();
  EXPECT_EQ(cmd.executable, "curl");
  // Arguments should include POST, Content-Type: application/json, -d body, and URL
  EXPECT_EQ(cmd.arguments.back(), slack_url);
  EXPECT_NE(std::find(cmd.arguments.begin(), cmd.arguments.end(), "-X"), cmd.arguments.end());
}

TEST(DefaultWebhookSenderTest, HandlesRunnerFailureGracefully) {
  auto runner = std::make_shared<FakeWebhookCommandRunner>();
  runner->return_code = 1; // Curl error
  azdash::DefaultWebhookSender sender(runner);

  azdash::WebhookPayload payload{.title = "Test Error"};
  EXPECT_FALSE(sender.send("https://webhook.office.com/webhookb2/teams", payload));

  runner->should_throw = true;
  EXPECT_FALSE(sender.send("https://webhook.office.com/webhookb2/teams", payload));
}

TEST(CliWebhookIntegrationTest, DispatchesCostAlertOnBudgetExceeded) {
  auto azure_client = std::make_shared<MockAzureClient>();
  auto adapter = azdash::AzureClientAdapter(azure_client);
  DummyStore dummy_store;
  MockWebhookSender webhook_sender;
  std::ostringstream out;
  std::ostringstream err;

  azdash::CliRuntime runtime{
      .out = out,
      .err = err,
      .account_provider = adapter,
      .cost_provider = adapter,
      .trend_provider = adapter,
      .waste_provider = adapter,
      .report_writer = dummy_store,
      .alias_store = dummy_store,
      .history_store = dummy_store,
      .webhook_sender = &webhook_sender,
  };

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Cost;
  options.webhook_url = "https://example.com/webhook";
  options.fail_if_exceeds_cost = 200.0; // Total is 300.0 ($250 + $50)

  int exit_code = azdash::run(options, runtime);
  EXPECT_EQ(exit_code, 2); // Budget exceeded exit code

  ASSERT_EQ(webhook_sender.sent_payloads.size(), 1u);
  const auto& [url, payload] = webhook_sender.sent_payloads.front();
  EXPECT_EQ(url, "https://example.com/webhook");
  EXPECT_EQ(payload.status, "danger");
  EXPECT_NE(payload.message.find("Budget EXCEEDED"), std::string::npos);
}

TEST(CliWebhookIntegrationTest, DispatchesAnomalyAlert) {
  auto azure_client = std::make_shared<MockAzureClient>();
  auto adapter = azdash::AzureClientAdapter(azure_client);
  DummyStore dummy_store;
  MockWebhookSender webhook_sender;
  std::ostringstream out;
  std::ostringstream err;

  azdash::CliRuntime runtime{
      .out = out,
      .err = err,
      .account_provider = adapter,
      .cost_provider = adapter,
      .trend_provider = adapter,
      .waste_provider = adapter,
      .report_writer = dummy_store,
      .alias_store = dummy_store,
      .history_store = dummy_store,
      .webhook_sender = &webhook_sender,
  };

  azdash::CliOptions options;
  options.command = azdash::CommandKind::CostAnomaly;
  options.webhook_url = "https://example.com/anomaly-webhook";

  int exit_code = azdash::run(options, runtime);
  EXPECT_EQ(exit_code, 0);

  ASSERT_EQ(webhook_sender.sent_payloads.size(), 1u);
  const auto& [url, payload] = webhook_sender.sent_payloads.front();
  EXPECT_EQ(url, "https://example.com/anomaly-webhook");
  EXPECT_EQ(payload.status, "warning"); // Spike to 300 vs baseline ~110
  EXPECT_NE(payload.details.find("Z-Score"), std::string::npos);
}

TEST(CliWebhookIntegrationTest, DispatchesWasteAlert) {
  auto azure_client = std::make_shared<MockAzureClient>();
  auto adapter = azdash::AzureClientAdapter(azure_client);
  DummyStore dummy_store;
  MockWebhookSender webhook_sender;
  std::ostringstream out;
  std::ostringstream err;

  azdash::CliRuntime runtime{
      .out = out,
      .err = err,
      .account_provider = adapter,
      .cost_provider = adapter,
      .trend_provider = adapter,
      .waste_provider = adapter,
      .report_writer = dummy_store,
      .alias_store = dummy_store,
      .history_store = dummy_store,
      .webhook_sender = &webhook_sender,
  };

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Waste;
  options.webhook_url = "https://example.com/waste-webhook";
  options.dry_run = true;

  int exit_code = azdash::run(options, runtime);
  EXPECT_EQ(exit_code, 0);

  ASSERT_EQ(webhook_sender.sent_payloads.size(), 1u);
  const auto& [url, payload] = webhook_sender.sent_payloads.front();
  EXPECT_EQ(url, "https://example.com/waste-webhook");
  EXPECT_EQ(payload.status, "warning");
  EXPECT_NE(payload.message.find("Detected 1 waste items"), std::string::npos);
  EXPECT_NE(payload.details.find("Dry-run"), std::string::npos);
}

TEST(AzureClientAdapterTest, CorrectlyDelegatesToIAzureClient) {
  auto mock_client = std::make_shared<MockAzureClient>();
  azdash::AzureClientAdapter adapter(mock_client);
  azdash::CliOptions options;

  auto account = adapter.account(options);
  EXPECT_EQ(account.subscription_id, "sub-123");
  EXPECT_EQ(mock_client->account_calls, 1);

  auto current = adapter.current_month_costs(options);
  EXPECT_EQ(current.size(), 2u);
  EXPECT_EQ(mock_client->current_month_calls, 1);

  auto prev = adapter.previous_month_costs(options);
  EXPECT_EQ(prev.size(), 2u);
  EXPECT_EQ(mock_client->previous_month_calls, 1);

  auto trends = adapter.six_month_trends(options);
  EXPECT_EQ(trends.size(), 6u);
  EXPECT_EQ(mock_client->trend_calls, 1);

  auto waste = adapter.waste_findings(options);
  EXPECT_EQ(waste.size(), 1u);
  EXPECT_EQ(mock_client->waste_calls, 1);
}
