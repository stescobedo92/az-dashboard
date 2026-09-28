#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/cli.hpp"
#include "az_dashboard/models.hpp"
#include "az_dashboard/render.hpp"
#include "az_dashboard/webhook.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

class FakeRunner final : public azdash::ICommandRunner {
public:
  explicit FakeRunner(std::vector<azdash::CommandResult> results) : results_(std::move(results)) {}

  [[nodiscard]] auto run(const azdash::ProcessCommand& command, const azdash::ProcessRunnerOptions& options) const
      -> azdash::CommandResult override {
    std::lock_guard lock(mutex_);
    commands.push_back(command);
    options_seen.push_back(options);
    if (next_ >= results_.size()) {
      return {1, "", "unexpected command"};
    }
    return results_[next_++];
  }

  mutable std::mutex mutex_;
  mutable std::vector<azdash::ProcessCommand> commands;
  mutable std::vector<azdash::ProcessRunnerOptions> options_seen;

private:
  std::vector<azdash::CommandResult> results_;
  mutable std::size_t next_{0};
};

class FakeCommitmentProvider final : public azdash::ICliCommitmentProvider {
public:
  explicit FakeCommitmentProvider(std::vector<azdash::CommitmentRecommendation> recs) : recs_(std::move(recs)) {}

  [[nodiscard]] auto commitment_recommendations(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::CommitmentRecommendation> override {
    return recs_;
  }

private:
  std::vector<azdash::CommitmentRecommendation> recs_;
};

class FakeAccountProvider final : public azdash::ICliAccountProvider {
public:
  [[nodiscard]] auto account(const azdash::CliOptions& /*options*/) const -> azdash::AccountInfo override {
    return {"sub-1", "Test Sub", "tenant-1", "user@test.com"};
  }
};

class FakeCostProvider final : public azdash::ICliCostProvider {
public:
  [[nodiscard]] auto current_month_costs(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::ServiceCost> override {
    return {};
  }
  [[nodiscard]] auto previous_month_costs(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::ServiceCost> override {
    return {};
  }
};

class FakeTrendProvider final : public azdash::ICliTrendProvider {
public:
  [[nodiscard]] auto six_month_trends(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::MonthCost> override {
    return {};
  }
};

class FakeWasteProvider final : public azdash::ICliWasteProvider {
public:
  [[nodiscard]] auto waste_findings(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::WasteFinding> override {
    return {};
  }
};

class FakeBudgetProvider final : public azdash::ICliBudgetProvider {
public:
  [[nodiscard]] auto budgets(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::BudgetInfo> override {
    return {};
  }
};

class FakeReportWriter final : public azdash::ICliReportWriter {
public:
  [[nodiscard]] auto resolve_path(const std::string& requested, const std::string& def) const -> std::filesystem::path override {
    return requested.empty() ? std::filesystem::path(def) : std::filesystem::path(requested);
  }
  void write_cost(const std::filesystem::path&, const azdash::AccountInfo&, const std::vector<azdash::CostComparisonRow>&) const override {}
  void write_trend(const std::filesystem::path&, const azdash::AccountInfo&, const std::vector<azdash::MonthCost>&) const override {}
  void write_waste(const std::filesystem::path&, const azdash::AccountInfo&, const std::vector<azdash::WasteFinding>&) const override {}
};

class FakeAliasStore final : public azdash::ICliSubscriptionAliasStore {
public:
  [[nodiscard]] auto list() const -> std::vector<azdash::SubscriptionAlias> override { return {}; }
  [[nodiscard]] auto resolve(const std::string& s) const -> std::string override { return s; }
  void set(const std::string&, const std::string&) const override {}
  [[nodiscard]] auto remove(const std::string&) const -> bool override { return false; }
};

class FakeHistoryStore final : public azdash::ICliCostHistoryStore {
public:
  void record(const azdash::CostSnapshot&) const override {}
  [[nodiscard]] auto snapshots() const -> std::vector<azdash::CostSnapshot> override { return {}; }
};

class MockWebhookSender final : public azdash::IWebhookSender {
public:
  mutable std::vector<std::pair<std::string, azdash::WebhookPayload>> sent;
  [[nodiscard]] auto send(const std::string& url, const azdash::WebhookPayload& payload) const -> bool override {
    sent.emplace_back(url, payload);
    return true;
  }
};

TEST(CommitmentsTest, AzureCliClientParsesReservationsAndAdvisorSavingsPlans) {
  const std::string consumption_recs_json = R"([
    {
      "id": "/subscriptions/sub-1/providers/Microsoft.Consumption/reservationRecommendations/rec-1",
      "properties": {
        "skuName": "Standard_D4s_v5",
        "term": "P1Y",
        "netSavings": 120.50,
        "totalCostWithReservedInstances": 180.00,
        "currency": "USD",
        "region": "eastus"
      }
    }
  ])";

  const std::string advisor_recs_json = R"([
    {
      "id": "/subscriptions/sub-1/providers/Microsoft.Advisor/recommendations/adv-1",
      "properties": {
        "recommendationTypeId": "SavingsPlan",
        "shortDescription": {"solution": "Buy 3 Year Compute Savings Plan"},
        "annualSavingsAmount": 2400.0,
        "impactedValue": "Compute",
        "impactedField": "Global",
        "currency": "USD"
      }
    }
  ])";

  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{
      {0, consumption_recs_json, ""},
      {0, advisor_recs_json, ""},
  });
  azdash::AzureCliClient client(runner);

  azdash::CliOptions options;
  options.subscriptions = {"sub-1"};

  auto recs = client.commitment_recommendations(options);
  ASSERT_EQ(recs.size(), 2);

  EXPECT_EQ(recs[0].type, "ReservedInstance");
  EXPECT_EQ(recs[0].sku, "Standard_D4s_v5");
  EXPECT_EQ(recs[0].region, "eastus");
  EXPECT_EQ(recs[0].term, "1 Year");
  EXPECT_DOUBLE_EQ(recs[0].estimated_monthly_savings, 120.50);
  EXPECT_DOUBLE_EQ(recs[0].estimated_monthly_cost, 180.00);

  EXPECT_EQ(recs[1].type, "SavingsPlan");
  EXPECT_EQ(recs[1].sku, "Compute");
  EXPECT_EQ(recs[1].region, "Global");
  EXPECT_EQ(recs[1].term, "3 Years");
  EXPECT_DOUBLE_EQ(recs[1].estimated_monthly_savings, 200.0); // 2400 / 12
}

TEST(CommitmentsTest, FilterByTermAndMinSavings) {
  const std::string consumption_recs_json = R"([
    {
      "properties": {
        "skuName": "Standard_B2s",
        "term": "P1Y",
        "netSavings": 15.00,
        "currency": "USD"
      }
    },
    {
      "properties": {
        "skuName": "Standard_E8s_v5",
        "term": "P3Y",
        "netSavings": 350.00,
        "currency": "USD"
      }
    }
  ])";

  auto runner1 = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{
      {0, consumption_recs_json, ""},
      {0, "[]", ""},
  });
  azdash::AzureCliClient client1(runner1);

  azdash::CliOptions opt_term;
  opt_term.subscriptions = {"sub-1"};
  opt_term.commitment_term = "3yr";

  auto recs_3yr = client1.commitment_recommendations(opt_term);
  ASSERT_EQ(recs_3yr.size(), 1);
  EXPECT_EQ(recs_3yr[0].sku, "Standard_E8s_v5");
  EXPECT_EQ(recs_3yr[0].term, "3 Years");

  auto runner2 = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{
      {0, consumption_recs_json, ""},
      {0, "[]", ""},
  });
  azdash::AzureCliClient client2(runner2);

  azdash::CliOptions opt_savings;
  opt_savings.subscriptions = {"sub-1"};
  opt_savings.min_savings = 50.0;

  auto recs_savings = client2.commitment_recommendations(opt_savings);
  ASSERT_EQ(recs_savings.size(), 1);
  EXPECT_EQ(recs_savings[0].sku, "Standard_E8s_v5");
}

TEST(CommitmentsTest, ArgumentParserParsesCommitmentsCommandsAndFlags) {
  const std::vector<std::string> args = {"commitments", "--term", "3yr", "--min-savings", "150.50", "-o", "json"};
  auto options = azdash::parse_args(args);

  EXPECT_EQ(options.command, azdash::CommandKind::Commitments);
  EXPECT_EQ(options.commitment_term, "3yr");
  EXPECT_DOUBLE_EQ(options.min_savings, 150.50);
  EXPECT_EQ(options.output, azdash::OutputFormat::Json);

  const std::vector<std::string> args_alias1 = {"ri"};
  EXPECT_EQ(azdash::parse_args(args_alias1).command, azdash::CommandKind::Commitments);

  const std::vector<std::string> args_alias2 = {"reservations"};
  EXPECT_EQ(azdash::parse_args(args_alias2).command, azdash::CommandKind::Commitments);
}

TEST(CommitmentsTest, RenderingTableJsonCsvMarkdown) {
  std::vector<azdash::CommitmentRecommendation> recs = {
      {.id = "1",
       .type = "ReservedInstance",
       .resource_type = "Microsoft.Compute/virtualMachines",
       .sku = "Standard_D4s_v5",
       .region = "eastus",
       .term = "1 Year",
       .estimated_monthly_savings = 80.0,
       .estimated_monthly_cost = 120.0,
       .currency = "USD",
       .details = "1yr reservation recommendation"},
  };

  std::ostringstream table_out;
  azdash::render_commitments(recs, azdash::OutputFormat::Table, table_out);
  std::string table_str = table_out.str();
  EXPECT_NE(table_str.find("Standard_D4s_v5"), std::string::npos);
  EXPECT_NE(table_str.find("1 Year"), std::string::npos);
  EXPECT_NE(table_str.find("Total Potential Monthly Savings"), std::string::npos);

  std::ostringstream json_out;
  azdash::render_commitments(recs, azdash::OutputFormat::Json, json_out);
  std::string json_str = json_out.str();
  EXPECT_NE(json_str.find("\"sku\": \"Standard_D4s_v5\""), std::string::npos);
  EXPECT_NE(json_str.find("\"estimated_monthly_savings\": 80.0"), std::string::npos);

  std::ostringstream csv_out;
  azdash::render_commitments(recs, azdash::OutputFormat::Csv, csv_out);
  std::string csv_str = csv_out.str();
  EXPECT_NE(csv_str.find("Standard_D4s_v5"), std::string::npos);

  std::ostringstream md_out;
  azdash::render_commitments(recs, azdash::OutputFormat::Markdown, md_out);
  std::string md_str = md_out.str();
  EXPECT_NE(md_str.find("Standard_D4s_v5"), std::string::npos);
  EXPECT_NE(md_str.find("**Total potential monthly savings:**"), std::string::npos);
}

TEST(CommitmentsTest, CliWorkflowExecuteCommitmentsAndWebhook) {
  std::vector<azdash::CommitmentRecommendation> recs = {
      {.id = "1",
       .type = "ReservedInstance",
       .resource_type = "Microsoft.Compute/virtualMachines",
       .sku = "Standard_D8s_v5",
       .region = "westeurope",
       .term = "3 Years",
       .estimated_monthly_savings = 250.0,
       .estimated_monthly_cost = 350.0,
       .currency = "USD",
       .details = "3yr commitment recommendation"},
  };

  FakeCommitmentProvider commit_prov(recs);
  FakeAccountProvider acc_prov;
  FakeCostProvider cost_prov;
  FakeTrendProvider trend_prov;
  FakeWasteProvider waste_prov;
  FakeBudgetProvider budget_prov;
  FakeReportWriter rep_writer;
  FakeAliasStore alias_store;
  FakeHistoryStore history_store;
  MockWebhookSender webhook_sender;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, acc_prov, cost_prov, trend_prov, waste_prov,
                             rep_writer, alias_store, history_store, &webhook_sender, nullptr, nullptr,
                             &budget_prov, &commit_prov};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Commitments;
  options.webhook_url = "https://hooks.slack.com/services/T00/B00/X00";

  int rc = azdash::run(options, runtime);
  EXPECT_EQ(rc, 0);
  EXPECT_NE(out.str().find("Standard_D8s_v5"), std::string::npos);
  ASSERT_EQ(webhook_sender.sent.size(), 1);
  EXPECT_EQ(webhook_sender.sent[0].second.title, "Azure Commitment Discounts Alert");
  EXPECT_NE(webhook_sender.sent[0].second.message.find("250.00 USD"), std::string::npos);
}

} // namespace
