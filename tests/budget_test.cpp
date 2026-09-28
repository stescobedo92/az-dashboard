#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/cli.hpp"
#include "az_dashboard/models.hpp"
#include "az_dashboard/render.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <string>
#include <mutex>
#include <vector>

namespace {

class FakeCommandRunner final : public azdash::ICommandRunner {
public:
  explicit FakeCommandRunner(std::vector<azdash::CommandResult> results) : results_(std::move(results)) {}

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

class FakeBudgetProvider final : public azdash::ICliBudgetProvider {
public:
  explicit FakeBudgetProvider(std::vector<azdash::BudgetInfo> budgets) : budgets_(std::move(budgets)) {}

  [[nodiscard]] auto budgets(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::BudgetInfo> override {
    return budgets_;
  }

private:
  std::vector<azdash::BudgetInfo> budgets_;
};

class FakeCostProvider final : public azdash::ICliCostProvider {
public:
  [[nodiscard]] auto current_month_costs(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::ServiceCost> override {
    return {{"Compute", 1200.0, {}, "USD"}};
  }
  [[nodiscard]] auto previous_month_costs(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::ServiceCost> override {
    return {{"Compute", 1000.0, {}, "USD"}};
  }
};

class FakeAccountProvider final : public azdash::ICliAccountProvider {
public:
  [[nodiscard]] auto account(const azdash::CliOptions& /*options*/) const -> azdash::AccountInfo override {
    return {"sub-1", "Test Sub", "tenant-1", "user@test.com"};
  }
};

class FakeTrendProvider final : public azdash::ICliTrendProvider {
public:
  [[nodiscard]] auto six_month_trends(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::MonthCost> override {
    return {azdash::MonthCost{"2026-09", 1200.0, {{"Compute", 1200.0, {}, "USD"}}, "USD"}};
  }
};

class FakeWasteProvider final : public azdash::ICliWasteProvider {
public:
  [[nodiscard]] auto waste_findings(const azdash::CliOptions& /*options*/) const -> std::vector<azdash::WasteFinding> override {
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

TEST(BudgetTest, AzureCliClientParsesBudgets) {
  const std::string budget_json = R"([
    {
      "id": "/subscriptions/sub-1/providers/Microsoft.Consumption/budgets/MonthlyBudget",
      "name": "MonthlyBudget",
      "properties": {
        "amount": 2000.0,
        "currentSpend": {
          "amount": 1500.25,
          "unit": "USD"
        },
        "timeGrain": "Monthly",
        "timePeriod": {
          "startDate": "2026-09-01T00:00:00Z",
          "endDate": "2027-09-01T00:00:00Z"
        }
      }
    },
    {
      "id": "/subscriptions/sub-1/providers/Microsoft.Consumption/budgets/DevSandbox",
      "name": "DevSandbox",
      "properties": {
        "amount": 500.0,
        "currentSpend": {
          "amount": 620.0,
          "unit": "EUR"
        },
        "timeGrain": "Monthly"
      }
    }
  ])";

  auto runner = std::make_shared<FakeCommandRunner>(std::vector<azdash::CommandResult>{{0, budget_json, ""}});
  azdash::AzureCliClient client(runner);

  azdash::CliOptions options;
  options.subscriptions = {"sub-1"};

  auto budgets = client.budgets(options);
  ASSERT_EQ(budgets.size(), 2);

  EXPECT_EQ(budgets[0].name, "MonthlyBudget");
  EXPECT_DOUBLE_EQ(budgets[0].amount, 2000.0);
  EXPECT_DOUBLE_EQ(budgets[0].current_spend, 1500.25);
  EXPECT_EQ(budgets[0].currency, "USD");
  EXPECT_EQ(budgets[0].time_grain, "Monthly");
  EXPECT_EQ(budgets[0].start_date, "2026-09-01T00:00:00Z");
  EXPECT_EQ(budgets[0].end_date, "2027-09-01T00:00:00Z");

  EXPECT_EQ(budgets[1].name, "DevSandbox");
  EXPECT_DOUBLE_EQ(budgets[1].amount, 500.0);
  EXPECT_DOUBLE_EQ(budgets[1].current_spend, 620.0);
  EXPECT_EQ(budgets[1].currency, "EUR");
}

TEST(BudgetTest, AzureCliClientFiltersBudgetByName) {
  const std::string budget_json = R"([
    {
      "id": "1",
      "name": "TeamA-Budget",
      "properties": {"amount": 1000.0, "currentSpend": {"amount": 800.0}}
    },
    {
      "id": "2",
      "name": "TeamB-Budget",
      "properties": {"amount": 500.0, "currentSpend": {"amount": 200.0}}
    }
  ])";

  auto runner = std::make_shared<FakeCommandRunner>(std::vector<azdash::CommandResult>{{0, budget_json, ""}});
  azdash::AzureCliClient client(runner);

  azdash::CliOptions options;
  options.subscriptions = {"sub-1"};
  options.budget_filter = "TeamA";

  auto budgets = client.budgets(options);
  ASSERT_EQ(budgets.size(), 1);
  EXPECT_EQ(budgets[0].name, "TeamA-Budget");
}

TEST(BudgetTest, ManagementGroupRecursionResolvesSubscriptions) {
  const std::string mg_json = R"({
    "id": "/providers/Microsoft.Management/managementGroups/mg-enterprise",
    "name": "mg-enterprise",
    "properties": {
      "children": [
        {
          "id": "/subscriptions/sub-prod-001",
          "name": "sub-prod-001",
          "type": "/subscriptions"
        },
        {
          "id": "/providers/Microsoft.Management/managementGroups/mg-subgroup",
          "name": "mg-subgroup",
          "type": "/providers/Microsoft.Management/managementGroups",
          "children": [
            {
              "id": "/subscriptions/sub-dev-002",
              "name": "sub-dev-002",
              "type": "/subscriptions"
            }
          ]
        }
      ]
    }
  })";

  const std::string sub1_budgets = R"([{"name": "Sub1Budget", "properties": {"amount": 1000.0, "currentSpend": 900.0}}])";
  const std::string sub2_budgets = R"([{"name": "Sub2Budget", "properties": {"amount": 2000.0, "currentSpend": 1100.0}}])";

  auto runner = std::make_shared<FakeCommandRunner>(std::vector<azdash::CommandResult>{
      {0, mg_json, ""},
      {0, sub1_budgets, ""},
      {0, sub2_budgets, ""},
  });
  azdash::AzureCliClient client(runner);

  azdash::CliOptions options;
  options.management_group = "mg-enterprise";

  auto budgets = client.budgets(options);
  EXPECT_EQ(budgets.size(), 2);
  ASSERT_GE(runner->commands.size(), 3);
  EXPECT_EQ(runner->commands[0].arguments[1], "management-group");
  EXPECT_EQ(runner->commands[0].arguments[4], "mg-enterprise");
}

TEST(BudgetTest, CliArgumentParserHandlesBudgetAndMgFlags) {
  const std::vector<std::string> args = {"--mg", "mg-root", "budget", "QuarterlyBudget", "-o", "json"};
  auto options = azdash::parse_args(args);

  EXPECT_EQ(options.command, azdash::CommandKind::Budget);
  EXPECT_EQ(options.management_group, "mg-root");
  ASSERT_FALSE(options.selectors.empty());
  EXPECT_EQ(options.selectors[0], "QuarterlyBudget");
  EXPECT_EQ(options.output, azdash::OutputFormat::Json);
}

TEST(BudgetTest, CliArgumentParserHandlesGlobalBudgetFilterFlag) {
  const std::vector<std::string> args = {"--management-group", "corp-mg", "--budget", "CoreProd", "cost"};
  auto options = azdash::parse_args(args);

  EXPECT_EQ(options.command, azdash::CommandKind::Cost);
  EXPECT_EQ(options.management_group, "corp-mg");
  EXPECT_EQ(options.budget_filter, "CoreProd");
}

TEST(BudgetTest, BudgetRenderingTableAndJson) {
  std::vector<azdash::BudgetInfo> budgets = {
      {"CoreOps", 1000.0, 450.0, "Monthly", "2026-09-01", "2027-09-01", "USD"},
      {"DataLake", 500.0, 750.0, "Monthly", "2026-09-01", "2027-09-01", "USD"},
  };

  std::ostringstream table_out;
  azdash::render_budgets(budgets, azdash::OutputFormat::Table, table_out);
  std::string table_str = table_out.str();
  EXPECT_NE(table_str.find("CoreOps"), std::string::npos);
  EXPECT_NE(table_str.find("DataLake"), std::string::npos);
  EXPECT_NE(table_str.find("OK"), std::string::npos);
  EXPECT_NE(table_str.find("EXCEEDED"), std::string::npos);

  std::ostringstream json_out;
  azdash::render_budgets(budgets, azdash::OutputFormat::Json, json_out);
  std::string json_str = json_out.str();
  EXPECT_NE(json_str.find("\"name\": \"CoreOps\""), std::string::npos);
  EXPECT_NE(json_str.find("\"usage_percent\": 150.0"), std::string::npos);

  std::ostringstream csv_out;
  azdash::render_budgets(budgets, azdash::OutputFormat::Csv, csv_out);
  std::string csv_str = csv_out.str();
  EXPECT_NE(csv_str.find("CoreOps"), std::string::npos);
}

TEST(BudgetTest, CliWorkflowExecuteBudget) {
  std::vector<azdash::BudgetInfo> mock_budgets = {
      {"ProdBudget", 5000.0, 4200.0, "Monthly", "2026-09-01", "2027-09-01", "USD"},
  };
  FakeBudgetProvider budget_prov(mock_budgets);
  FakeAccountProvider acc_prov;
  FakeCostProvider cost_prov;
  FakeTrendProvider trend_prov;
  FakeWasteProvider waste_prov;
  FakeReportWriter rep_writer;
  FakeAliasStore alias_store;
  FakeHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, acc_prov, cost_prov, trend_prov, waste_prov,
                             rep_writer, alias_store, history_store, nullptr, nullptr, nullptr, &budget_prov};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Budget;
  options.output = azdash::OutputFormat::Table;

  int rc = azdash::run(options, runtime);
  EXPECT_EQ(rc, 0);
  EXPECT_NE(out.str().find("ProdBudget"), std::string::npos);
  EXPECT_NE(out.str().find("OK"), std::string::npos);
}

TEST(BudgetTest, CliWorkflowExecuteBudgetFailIfExceeds) {
  std::vector<azdash::BudgetInfo> mock_budgets = {
      {"ProdBudget", 5000.0, 6000.0, "Monthly", "2026-09-01", "2027-09-01", "USD"},
  };
  FakeBudgetProvider budget_prov(mock_budgets);
  FakeAccountProvider acc_prov;
  FakeCostProvider cost_prov;
  FakeTrendProvider trend_prov;
  FakeWasteProvider waste_prov;
  FakeReportWriter rep_writer;
  FakeAliasStore alias_store;
  FakeHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, acc_prov, cost_prov, trend_prov, waste_prov,
                             rep_writer, alias_store, history_store, nullptr, nullptr, nullptr, &budget_prov};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Budget;
  options.fail_if_exceeds_cost = 5500.0;

  int rc = azdash::run(options, runtime);
  EXPECT_EQ(rc, 2);
}

TEST(BudgetTest, CliCostBudgetWarningExceeded) {
  std::vector<azdash::BudgetInfo> mock_budgets = {
      {"CapBudget", 500.0, 400.0, "Monthly", "2026-09-01", "2027-09-01", "USD"},
  };
  FakeBudgetProvider budget_prov(mock_budgets);
  FakeAccountProvider acc_prov;
  FakeCostProvider cost_prov;
  FakeTrendProvider trend_prov;
  FakeWasteProvider waste_prov;
  FakeReportWriter rep_writer;
  FakeAliasStore alias_store;
  FakeHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, acc_prov, cost_prov, trend_prov, waste_prov,
                             rep_writer, alias_store, history_store, nullptr, nullptr, nullptr, &budget_prov};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Cost;
  options.budget_filter = "CapBudget";

  int rc = azdash::run(options, runtime);
  EXPECT_EQ(rc, 0);
  EXPECT_NE(out.str().find("Azure Budget Warning"), std::string::npos);
  EXPECT_NE(out.str().find("CapBudget"), std::string::npos);
}

} // namespace
