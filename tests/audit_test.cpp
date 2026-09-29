#include "az_dashboard/analytics.hpp"
#include "az_dashboard/cli.hpp"
#include "az_dashboard/cli_parser.hpp"
#include "az_dashboard/models.hpp"
#include "az_dashboard/render.hpp"
#include "az_dashboard/webhook.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace {

class FakeAccountProvider final : public azdash::ICliAccountProvider {
public:
  [[nodiscard]] auto account(const azdash::CliOptions& /*options*/) const -> azdash::AccountInfo override {
    return {"sub-audit-123", "Production Subscription", "tenant-456", "finops@company.com"};
  }
};

class FakeCostProvider final : public azdash::ICliCostProvider {
public:
  explicit FakeCostProvider(std::vector<azdash::ServiceCost> costs) : costs_(std::move(costs)) {}

  [[nodiscard]] auto current_month_costs(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::ServiceCost> override {
    return costs_;
  }
  [[nodiscard]] auto previous_month_costs(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::ServiceCost> override {
    return costs_;
  }

private:
  std::vector<azdash::ServiceCost> costs_;
};

class FakeTrendProvider final : public azdash::ICliTrendProvider {
public:
  explicit FakeTrendProvider(std::vector<azdash::MonthCost> trends) : trends_(std::move(trends)) {}

  [[nodiscard]] auto six_month_trends(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::MonthCost> override {
    return trends_;
  }

private:
  std::vector<azdash::MonthCost> trends_;
};

class FakeWasteProvider final : public azdash::ICliWasteProvider {
public:
  explicit FakeWasteProvider(std::vector<azdash::WasteFinding> findings) : findings_(std::move(findings)) {}

  [[nodiscard]] auto waste_findings(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::WasteFinding> override {
    return findings_;
  }

private:
  std::vector<azdash::WasteFinding> findings_;
};

class FakeBudgetProvider final : public azdash::ICliBudgetProvider {
public:
  explicit FakeBudgetProvider(std::vector<azdash::BudgetInfo> budgets) : budgets_(std::move(budgets)) {}

  [[nodiscard]] auto budgets(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::BudgetInfo> override {
    return budgets_;
  }

private:
  std::vector<azdash::BudgetInfo> budgets_;
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

class FakeComplianceProvider final : public azdash::ICliComplianceProvider {
public:
  explicit FakeComplianceProvider(azdash::TagComplianceSummary summary) : summary_(std::move(summary)) {}

  [[nodiscard]] auto tag_compliance(const azdash::CliOptions& /*options*/) const
      -> azdash::TagComplianceSummary override {
    return summary_;
  }

private:
  azdash::TagComplianceSummary summary_;
};

class FakeReportWriter final : public azdash::ICliReportWriter {
public:
  [[nodiscard]] auto resolve_path(const std::string& requested_path,
                                  const std::string& default_filename) const -> std::filesystem::path override {
    return std::filesystem::path(requested_path) / default_filename;
  }
  void write_cost(const std::filesystem::path&, const azdash::AccountInfo&,
                  const std::vector<azdash::CostComparisonRow>&) const override {}
  void write_trend(const std::filesystem::path&, const azdash::AccountInfo&,
                   const std::vector<azdash::MonthCost>&) const override {}
  void write_waste(const std::filesystem::path&, const azdash::AccountInfo&,
                   const std::vector<azdash::WasteFinding>&) const override {}
};

class FakeAliasStore final : public azdash::ICliSubscriptionAliasStore {
public:
  [[nodiscard]] auto list() const -> std::vector<azdash::SubscriptionAlias> override { return {}; }
  [[nodiscard]] auto resolve(const std::string& selector) const -> std::string override { return selector; }
  void set(const std::string&, const std::string&) const override {}
  [[nodiscard]] auto remove(const std::string&) const -> bool override { return false; }
};

class FakeCostHistoryStore final : public azdash::ICliCostHistoryStore {
public:
  void record(const azdash::CostSnapshot&) const override {}
  [[nodiscard]] auto snapshots() const -> std::vector<azdash::CostSnapshot> override { return {}; }
};

class MockWebhookSender final : public azdash::IWebhookSender {
public:
  mutable bool send_called{false};
  mutable std::string captured_url;
  mutable azdash::WebhookPayload captured_payload;
  bool return_value{true};

  [[nodiscard]] auto send(const std::string& webhook_url,
                          const azdash::WebhookPayload& payload) const -> bool override {
    send_called = true;
    captured_url = webhook_url;
    captured_payload = payload;
    return return_value;
  }
};

} // namespace

TEST(FinOpsAuditAnalyticsTest, CalculatesRunStageForCompliantEnvironment) {
  azdash::AccountInfo account{"sub-1", "Prod", "tenant-1", "user@test.com"};
  std::vector<azdash::ServiceCost> costs{
      {"Virtual Machines", 5000.0, {}, "USD"},
      {"Storage", 1200.0, {}, "USD"},
  };

  azdash::TagComplianceSummary compliance;
  compliance.total_resources = 100;
  compliance.compliant_resources = 100;
  compliance.non_compliant_resources = 0;
  compliance.compliance_percentage = 100.0;
  compliance.total_spend = 6200.0;
  compliance.allocated_spend = 6200.0;
  compliance.unallocated_spend = 0.0;

  std::vector<azdash::WasteFinding> waste{};
  std::vector<azdash::BudgetInfo> budgets{
      {"MonthlyBudget", 10000.0, 6200.0, "Monthly", "2026-01-01", "2026-12-31", "USD"},
  };
  std::vector<azdash::CommitmentRecommendation> commitments{};

  azdash::CostAnomalyAssessment anomaly;
  anomaly.enough_data = true;
  anomaly.anomalous = false;
  anomaly.zscore = 0.4;
  anomaly.mean = 6000.0;

  const auto report = azdash::evaluate_finops_audit(account, costs, compliance, waste, budgets, commitments, anomaly);

  EXPECT_GE(report.overall_score, 90.0);
  EXPECT_EQ(report.maturity_stage, "Run");
  EXPECT_TRUE(report.grade == "A" || report.grade == "A+");
  EXPECT_EQ(report.pillars.size(), 5u);

  for (const auto& p : report.pillars) {
    EXPECT_EQ(p.status, "Healthy");
    EXPECT_GE(p.score, 80.0);
  }
}

TEST(FinOpsAuditAnalyticsTest, CalculatesCrawlStageForUnhealthyEnvironment) {
  azdash::AccountInfo account{"sub-1", "Prod", "tenant-1", "user@test.com"};
  std::vector<azdash::ServiceCost> costs{
      {"Virtual Machines", 8000.0, {}, "USD"},
      {"Storage", 2000.0, {}, "USD"},
  };

  azdash::TagComplianceSummary compliance;
  compliance.total_resources = 50;
  compliance.compliant_resources = 5;
  compliance.non_compliant_resources = 45;
  compliance.compliance_percentage = 10.0;
  compliance.total_spend = 10000.0;
  compliance.allocated_spend = 1000.0;
  compliance.unallocated_spend = 9000.0;

  std::vector<azdash::WasteFinding> waste{
      {"unattached_disk", "/disk1", "Microsoft.Compute/disks", "disk1", "eastus", "Delete", 250.0, "USD"},
      {"idle_nat_gateway", "/nat1", "Microsoft.Network/natGateways", "nat1", "eastus", "Delete", 32.40, "USD"},
      {"unattached_nic", "/nic1", "Microsoft.Network/networkInterfaces", "nic1", "eastus", "Delete", 5.0, "USD"},
      {"empty_asp", "/asp1", "Microsoft.Web/serverfarms", "asp1", "eastus", "Delete", 73.0, "USD"},
  };

  std::vector<azdash::BudgetInfo> budgets{}; // No budgets -> critical
  std::vector<azdash::CommitmentRecommendation> commitments{
      {"rec-1", "ReservedInstance", "Microsoft.Compute/virtualMachines", "Standard_D4s_v5", "eastus", "3 Years",
       1800.0, 3200.0, "USD", "Save on D4s"},
  };

  azdash::CostAnomalyAssessment anomaly;
  anomaly.enough_data = true;
  anomaly.anomalous = true;
  anomaly.zscore = 3.8;
  anomaly.mean = 6000.0;

  const auto report = azdash::evaluate_finops_audit(account, costs, compliance, waste, budgets, commitments, anomaly);

  EXPECT_LT(report.overall_score, 50.0);
  EXPECT_EQ(report.maturity_stage, "Crawl");
  EXPECT_EQ(report.grade, "F");
  EXPECT_GT(report.potential_savings, 2000.0);
  EXPECT_FALSE(report.key_takeaways.empty());
}

TEST(FinOpsAuditAnalyticsTest, CalculatesWalkStageForMixedEnvironment) {
  azdash::AccountInfo account{"sub-1", "Prod", "tenant-1", "user@test.com"};
  std::vector<azdash::ServiceCost> costs{
      {"Virtual Machines", 4000.0, {}, "USD"},
  };

  azdash::TagComplianceSummary compliance;
  compliance.total_resources = 50;
  compliance.compliant_resources = 25;
  compliance.non_compliant_resources = 25;
  compliance.compliance_percentage = 50.0;
  compliance.total_spend = 4000.0;
  compliance.allocated_spend = 2000.0;
  compliance.unallocated_spend = 2000.0;

  std::vector<azdash::WasteFinding> waste{
      {"unattached_disk", "/disk1", "Microsoft.Compute/disks", "disk1", "eastus", "Delete", 80.0, "USD"},
      {"idle_nat_gateway", "/nat1", "Microsoft.Network/natGateways", "nat1", "eastus", "Delete", 32.40, "USD"},
  };

  std::vector<azdash::BudgetInfo> budgets{
      {"MonthlyBudget", 3000.0, 4000.0, "Monthly", "2026-01-01", "2026-12-31", "USD"}, // Breached!
  };
  std::vector<azdash::CommitmentRecommendation> commitments{};

  azdash::CostAnomalyAssessment anomaly;
  anomaly.enough_data = false; // Crawl/insufficient data

  const auto report = azdash::evaluate_finops_audit(account, costs, compliance, waste, budgets, commitments, anomaly);

  EXPECT_GE(report.overall_score, 50.0);
  EXPECT_LT(report.overall_score, 80.0);
  EXPECT_EQ(report.maturity_stage, "Walk");
}

TEST(FinOpsAuditRenderTest, RendersJsonAndCsvProperly) {
  azdash::FinOpsAuditReport report;
  report.overall_score = 82.5;
  report.maturity_stage = "Run";
  report.grade = "B";
  report.total_spend = 5000.0;
  report.potential_savings = 350.0;
  report.currency = "USD";
  report.pillars = {
      {"Tag Allocation & Hygiene", 85.0, 0.25, "Healthy", "Summary 1", {"Rec 1"}},
      {"Waste & Idle Efficiency", 80.0, 0.25, "Healthy", "Summary 2", {"Rec 2"}},
  };
  report.key_takeaways = {"Takeaway 1"};

  // JSON format
  {
    std::ostringstream ss;
    azdash::render_audit(report, azdash::OutputFormat::Json, ss);
    const auto json_obj = nlohmann::json::parse(ss.str());
    EXPECT_DOUBLE_EQ(json_obj["overallScore"].get<double>(), 82.5);
    EXPECT_EQ(json_obj["maturityStage"].get<std::string>(), "Run");
    EXPECT_EQ(json_obj["grade"].get<std::string>(), "B");
    EXPECT_EQ(json_obj["pillars"].size(), 2u);
  }

  // CSV format
  {
    std::ostringstream ss;
    azdash::render_audit(report, azdash::OutputFormat::Csv, ss);
    const auto csv_text = ss.str();
    EXPECT_NE(csv_text.find("pillar,score,weight,status,summary"), std::string::npos);
    EXPECT_NE(csv_text.find("Overall Score"), std::string::npos);
    EXPECT_NE(csv_text.find("Tag Allocation & Hygiene"), std::string::npos);
  }

  // Markdown format
  {
    std::ostringstream ss;
    azdash::render_audit(report, azdash::OutputFormat::Markdown, ss);
    const auto md_text = ss.str();
    EXPECT_NE(md_text.find("# FinOps Maturity Scorecard"), std::string::npos);
    EXPECT_NE(md_text.find("Tag Allocation & Hygiene"), std::string::npos);
    EXPECT_NE(md_text.find("Takeaway 1"), std::string::npos);
  }

  // HTML format
  {
    std::ostringstream ss;
    azdash::render_audit(report, azdash::OutputFormat::Html, ss);
    const auto html_text = ss.str();
    EXPECT_NE(html_text.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(html_text.find("FinOps Foundation Maturity Scorecard"), std::string::npos);
  }

  // Table format
  {
    std::ostringstream ss;
    azdash::render_audit(report, azdash::OutputFormat::Table, ss);
    const auto table_text = ss.str();
    EXPECT_NE(table_text.find("FinOps Maturity Scorecard"), std::string::npos);
    EXPECT_NE(table_text.find("Tag Allocation & Hygiene"), std::string::npos);
  }
}

TEST(FinOpsAuditCliTest, ExitsZeroWhenScorePassesThreshold) {
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider({{"Compute", 1000.0, {}, "USD"}});
  FakeTrendProvider trend_provider({});
  FakeWasteProvider waste_provider({});
  FakeBudgetProvider budget_provider({{"Budget", 2000.0, 1000.0, "Monthly", "2026-01-01", "2026-12-31", "USD"}});
  FakeCommitmentProvider commitment_provider({});
  azdash::TagComplianceSummary compliance;
  compliance.total_resources = 10;
  compliance.compliant_resources = 10;
  compliance.compliance_percentage = 100.0;
  compliance.total_spend = 1000.0;
  compliance.allocated_spend = 1000.0;
  FakeComplianceProvider compliance_provider(compliance);
  FakeReportWriter report_writer;
  FakeAliasStore alias_store;
  FakeCostHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, account_provider, cost_provider, trend_provider,
                             waste_provider, report_writer, alias_store, history_store,
                             nullptr, nullptr, nullptr, &budget_provider, &commitment_provider,
                             &compliance_provider};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Audit;
  options.output = azdash::OutputFormat::Json;
  options.min_audit_score = 75.0; // Expected to pass

  const int code = azdash::run(options, runtime);
  EXPECT_EQ(code, 0);
  EXPECT_TRUE(err.str().empty());
}

TEST(FinOpsAuditCliTest, ExitsCodeTwoWhenScoreBelowThreshold) {
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider({{"Compute", 10000.0, {}, "USD"}});
  FakeTrendProvider trend_provider({});
  FakeWasteProvider waste_provider({
      {"unattached_disk", "/disk1", "Microsoft.Compute/disks", "disk1", "eastus", "Delete", 500.0, "USD"},
      {"idle_nat", "/nat1", "Microsoft.Network/natGateways", "nat1", "eastus", "Delete", 32.40, "USD"},
  });
  FakeBudgetProvider budget_provider({}); // No budget
  FakeCommitmentProvider commitment_provider({});
  azdash::TagComplianceSummary compliance;
  compliance.total_resources = 10;
  compliance.compliant_resources = 2;
  compliance.compliance_percentage = 20.0;
  compliance.total_spend = 10000.0;
  compliance.allocated_spend = 2000.0;
  compliance.unallocated_spend = 8000.0;
  FakeComplianceProvider compliance_provider(compliance);
  FakeReportWriter report_writer;
  FakeAliasStore alias_store;
  FakeCostHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, account_provider, cost_provider, trend_provider,
                             waste_provider, report_writer, alias_store, history_store,
                             nullptr, nullptr, nullptr, &budget_provider, &commitment_provider,
                             &compliance_provider};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Audit;
  options.output = azdash::OutputFormat::Json;
  options.min_audit_score = 90.0; // High requirement will fail

  const int code = azdash::run(options, runtime);
  EXPECT_EQ(code, 2);
  EXPECT_NE(err.str().find("is below required minimum threshold"), std::string::npos);
}

TEST(FinOpsAuditCliTest, DispatchesWebhookWhenUrlProvided) {
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider({{"Compute", 1000.0, {}, "USD"}});
  FakeTrendProvider trend_provider({});
  FakeWasteProvider waste_provider({});
  FakeBudgetProvider budget_provider({});
  FakeCommitmentProvider commitment_provider({});
  azdash::TagComplianceSummary compliance;
  FakeComplianceProvider compliance_provider(compliance);
  FakeReportWriter report_writer;
  FakeAliasStore alias_store;
  FakeCostHistoryStore history_store;
  MockWebhookSender webhook_sender;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, account_provider, cost_provider, trend_provider,
                             waste_provider, report_writer, alias_store, history_store,
                             &webhook_sender, nullptr, nullptr, &budget_provider, &commitment_provider,
                             &compliance_provider};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Audit;
  options.webhook_url = "https://hooks.slack.com/services/test";

  const int code = azdash::run(options, runtime);
  EXPECT_EQ(code, 0);
  EXPECT_TRUE(webhook_sender.send_called);
  EXPECT_EQ(webhook_sender.captured_url, "https://hooks.slack.com/services/test");
  EXPECT_EQ(webhook_sender.captured_payload.title, "FinOps Maturity Governance Audit Alert");
}
