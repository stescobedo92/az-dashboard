#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/cli.hpp"
#include "az_dashboard/cli_parser.hpp"
#include "az_dashboard/models.hpp"
#include "az_dashboard/render.hpp"
#include "az_dashboard/webhook.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace {

class FakeComplianceProvider final : public azdash::ICliComplianceProvider {
public:
  explicit FakeComplianceProvider(azdash::TagComplianceSummary summary)
      : summary_(std::move(summary)) {}

  [[nodiscard]] auto tag_compliance(const azdash::CliOptions& /*options*/) const
      -> azdash::TagComplianceSummary override {
    return summary_;
  }

private:
  azdash::TagComplianceSummary summary_;
};

class FakeAccountProvider final : public azdash::ICliAccountProvider {
public:
  [[nodiscard]] auto account(const azdash::CliOptions& /*options*/) const -> azdash::AccountInfo override {
    return {"sub-1", "Test Sub", "tenant-1", "user@test.com"};
  }
};

class FakeCostProvider final : public azdash::ICliCostProvider {
public:
  [[nodiscard]] auto current_month_costs(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::ServiceCost> override {
    return {};
  }
  [[nodiscard]] auto previous_month_costs(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::ServiceCost> override {
    return {};
  }
};

class FakeTrendProvider final : public azdash::ICliTrendProvider {
public:
  [[nodiscard]] auto six_month_trends(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::MonthCost> override {
    return {};
  }
};

class FakeWasteProvider final : public azdash::ICliWasteProvider {
public:
  [[nodiscard]] auto waste_findings(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::WasteFinding> override {
    return {};
  }
};

class FakeReportWriter final : public azdash::ICliReportWriter {
public:
  [[nodiscard]] auto resolve_path(const std::string& requested_path,
                                  const std::string& default_filename) const -> std::filesystem::path override {
    return std::filesystem::path(requested_path.empty() ? default_filename : requested_path);
  }
  void write_cost(const std::filesystem::path&, const azdash::AccountInfo&,
                  const std::vector<azdash::CostComparisonRow>&) const override {}
  void write_trend(const std::filesystem::path&, const azdash::AccountInfo&,
                   const std::vector<azdash::MonthCost>&) const override {}
  void write_waste(const std::filesystem::path&, const azdash::AccountInfo&,
                   const std::vector<azdash::WasteFinding>&) const override {}
};

class FakeSubscriptionAliasStore final : public azdash::ICliSubscriptionAliasStore {
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

class FakeWebhookSender final : public azdash::IWebhookSender {
public:
  [[nodiscard]] auto send(const std::string& url, const azdash::WebhookPayload& payload) const -> bool override {
    urls.push_back(url);
    payloads.push_back(payload);
    return true;
  }

  mutable std::vector<std::string> urls;
  mutable std::vector<azdash::WebhookPayload> payloads;
};

// -----------------------------------------------------------------------------
// Tag Compliance Evaluation Tests
// -----------------------------------------------------------------------------

TEST(ComplianceTest, EvaluatesEmptyPayload) {
  const nlohmann::json empty = nlohmann::json::array();
  const auto summary = azdash::detail::evaluate_tag_compliance(empty, {}, "USD");

  EXPECT_EQ(summary.total_resources, 0u);
  EXPECT_EQ(summary.compliant_resources, 0u);
  EXPECT_EQ(summary.non_compliant_resources, 0u);
  EXPECT_DOUBLE_EQ(summary.compliance_percentage, 100.0);
  EXPECT_DOUBLE_EQ(summary.total_spend, 0.0);
  EXPECT_DOUBLE_EQ(summary.unallocated_spend, 0.0);
}

TEST(ComplianceTest, EvaluatesCompliantResourceWithDefaultTagsCaseInsensitive) {
  const nlohmann::json payload = nlohmann::json::array({
      {
          {"name", "vm-prod-01"},
          {"properties",
           {
               {"instanceName", "vm-prod-01"},
               {"resourceGroup", "rg-core"},
               {"consumedService", "Microsoft.Compute"},
               {"pretaxCost", "120.50"},
               {"billingCurrency", "EUR"},
           }},
          {"tags",
           {
               {"environment", "production"},
               {"OWNER", "alice@example.com"},
               {"CostCenter", "CC-101"},
           }},
      },
  });

  const auto summary = azdash::detail::evaluate_tag_compliance(payload, {}, "USD");

  EXPECT_EQ(summary.total_resources, 1u);
  EXPECT_EQ(summary.compliant_resources, 1u);
  EXPECT_EQ(summary.non_compliant_resources, 0u);
  EXPECT_DOUBLE_EQ(summary.compliance_percentage, 100.0);
  EXPECT_DOUBLE_EQ(summary.allocated_spend, 120.50);
  EXPECT_DOUBLE_EQ(summary.unallocated_spend, 0.0);
  EXPECT_EQ(summary.currency, "EUR");
  EXPECT_TRUE(summary.non_compliant_items.empty());
}

TEST(ComplianceTest, EvaluatesNonCompliantResourceAndTracksMissingTagsAndCost) {
  const nlohmann::json payload = nlohmann::json::array({
      {
          {"name", "storage-unallocated"},
          {"properties",
           {
               {"instanceName", "storage-unallocated"},
               {"resourceGroup", "rg-data"},
               {"consumedService", "Microsoft.Storage"},
               {"pretaxCost", "75.00"},
               {"billingCurrency", "USD"},
           }},
          {"tags",
           {
               {"Environment", "Dev"},
           }},
      },
      {
          {"name", "app-service-fully-compliant"},
          {"properties",
           {
               {"instanceName", "app-service-fully-compliant"},
               {"resourceGroup", "rg-web"},
               {"consumedService", "Microsoft.Web"},
               {"pretaxCost", "50.00"},
               {"billingCurrency", "USD"},
           }},
          {"tags",
           {
               {"Environment", "Production"},
               {"Owner", "bob@example.com"},
               {"CostCenter", "FIN-404"},
           }},
      },
  });

  const auto summary = azdash::detail::evaluate_tag_compliance(payload, {}, "USD");

  EXPECT_EQ(summary.total_resources, 2u);
  EXPECT_EQ(summary.compliant_resources, 1u);
  EXPECT_EQ(summary.non_compliant_resources, 1u);
  EXPECT_DOUBLE_EQ(summary.compliance_percentage, 50.0);
  EXPECT_DOUBLE_EQ(summary.allocated_spend, 50.00);
  EXPECT_DOUBLE_EQ(summary.unallocated_spend, 75.00);
  EXPECT_DOUBLE_EQ(summary.total_spend, 125.00);

  ASSERT_EQ(summary.non_compliant_items.size(), 1u);
  const auto& item = summary.non_compliant_items.front();
  EXPECT_EQ(item.resource_name, "storage-unallocated");
  EXPECT_EQ(item.resource_group, "rg-data");
  EXPECT_DOUBLE_EQ(item.cost, 75.00);

  // Missing tags should be Owner and CostCenter
  EXPECT_EQ(item.missing_tags.size(), 2u);
  EXPECT_EQ(summary.missing_tag_counts.at("Owner"), 1u);
  EXPECT_EQ(summary.missing_tag_counts.at("CostCenter"), 1u);
  EXPECT_DOUBLE_EQ(summary.missing_tag_costs.at("Owner"), 75.00);
  EXPECT_DOUBLE_EQ(summary.missing_tag_costs.at("CostCenter"), 75.00);
}

TEST(ComplianceTest, CustomRequiredTagsFilter) {
  const nlohmann::json payload = nlohmann::json::array({
      {
          {"name", "k8s-cluster"},
          {"properties",
           {
               {"instanceName", "k8s-cluster"},
               {"resourceGroup", "rg-aks"},
               {"consumedService", "Microsoft.ContainerService"},
               {"pretaxCost", "300.00"},
           }},
          {"tags",
           {
               {"Application", "BillingPlatform"},
           }},
      },
  });

  std::vector<std::string> custom_tags = {"Application", "ServiceTier"};
  const auto summary = azdash::detail::evaluate_tag_compliance(payload, custom_tags, "USD");

  EXPECT_EQ(summary.total_resources, 1u);
  EXPECT_EQ(summary.compliant_resources, 0u);
  EXPECT_EQ(summary.non_compliant_resources, 1u);
  EXPECT_DOUBLE_EQ(summary.compliance_percentage, 0.0);
  EXPECT_EQ(summary.missing_tag_counts.at("ServiceTier"), 1u);
  EXPECT_FALSE(summary.missing_tag_counts.contains("Application"));
}

// -----------------------------------------------------------------------------
// Rendering Tests
// -----------------------------------------------------------------------------

TEST(ComplianceTest, RenderJsonContainsExpectedStructure) {
  azdash::TagComplianceSummary summary;
  summary.total_resources = 10;
  summary.compliant_resources = 8;
  summary.non_compliant_resources = 2;
  summary.compliance_percentage = 80.0;
  summary.total_spend = 1000.0;
  summary.allocated_spend = 800.0;
  summary.unallocated_spend = 200.0;
  summary.currency = "USD";
  summary.missing_tag_counts["Owner"] = 2;
  summary.missing_tag_costs["Owner"] = 200.0;
  summary.non_compliant_items.push_back({
      .resource_name = "vm-orphan",
      .resource_group = "rg-test",
      .resource_type = "Microsoft.Compute/virtualMachines",
      .cost = 200.0,
      .currency = "USD",
      .missing_tags = {"Owner"},
  });

  std::ostringstream out;
  azdash::render_compliance(summary, azdash::OutputFormat::Json, out);

  const auto parsed = nlohmann::json::parse(out.str());
  EXPECT_EQ(parsed["totalResources"], 10);
  EXPECT_EQ(parsed["compliantResources"], 8);
  EXPECT_EQ(parsed["compliancePercentage"], 80.0);
  EXPECT_EQ(parsed["missingTagCounts"]["Owner"], 2);
  EXPECT_EQ(parsed["nonCompliantItems"].size(), 1u);
  EXPECT_EQ(parsed["nonCompliantItems"][0]["resourceName"], "vm-orphan");
}

TEST(ComplianceTest, RenderCsvProducesCorrectHeadersAndRows) {
  azdash::TagComplianceSummary summary;
  summary.non_compliant_items.push_back({
      .resource_name = "disk-orphan",
      .resource_group = "rg-disks",
      .resource_type = "Microsoft.Compute/disks",
      .cost = 15.50,
      .currency = "USD",
      .missing_tags = {"Environment", "Owner"},
  });

  std::ostringstream out;
  azdash::render_compliance(summary, azdash::OutputFormat::Csv, out);

  const std::string text = out.str();
  EXPECT_NE(text.find("resource_name,resource_group,resource_type,cost,currency,missing_tags"), std::string::npos);
  EXPECT_NE(text.find("disk-orphan"), std::string::npos);
  EXPECT_NE(text.find("15.50"), std::string::npos);
  EXPECT_NE(text.find("Environment;Owner"), std::string::npos);
}

TEST(ComplianceTest, RenderMarkdownHasExecutiveSummary) {
  azdash::TagComplianceSummary summary;
  summary.total_resources = 5;
  summary.compliant_resources = 4;
  summary.non_compliant_resources = 1;
  summary.compliance_percentage = 80.0;
  summary.total_spend = 500.0;
  summary.allocated_spend = 400.0;
  summary.unallocated_spend = 100.0;
  summary.missing_tag_counts["CostCenter"] = 1;
  summary.missing_tag_costs["CostCenter"] = 100.0;

  std::ostringstream out;
  azdash::render_compliance(summary, azdash::OutputFormat::Markdown, out);

  const std::string text = out.str();
  EXPECT_NE(text.find("# Azure Tag Compliance & Cost Allocation Report"), std::string::npos);
  EXPECT_NE(text.find("**Overall Compliance:** 80.0%"), std::string::npos);
  EXPECT_NE(text.find("### Missing Tags Breakdown"), std::string::npos);
  EXPECT_NE(text.find("CostCenter"), std::string::npos);
}

TEST(ComplianceTest, RenderHtmlProducesValidDocument) {
  azdash::TagComplianceSummary summary;
  summary.total_resources = 20;
  summary.compliant_resources = 18;
  summary.non_compliant_resources = 2;
  summary.compliance_percentage = 90.0;
  summary.total_spend = 1500.0;
  summary.allocated_spend = 1350.0;
  summary.unallocated_spend = 150.0;
  summary.currency = "USD";
  summary.missing_tag_counts["Environment"] = 2;
  summary.missing_tag_costs["Environment"] = 150.0;
  summary.non_compliant_items.push_back({
      .resource_name = "lb-external",
      .resource_group = "rg-net",
      .resource_type = "Microsoft.Network/loadBalancers",
      .cost = 150.0,
      .currency = "USD",
      .missing_tags = {"Environment"},
  });

  std::ostringstream out;
  azdash::render_compliance(summary, azdash::OutputFormat::Html, out);

  const std::string html = out.str();
  EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
  EXPECT_NE(html.find("<title>Azure Tag Compliance &amp; Cost Allocation Report</title>"), std::string::npos);
  EXPECT_NE(html.find("90.0%"), std::string::npos);
  EXPECT_NE(html.find("lb-external"), std::string::npos);
  EXPECT_NE(html.find("tag-badge"), std::string::npos);
}

// -----------------------------------------------------------------------------
// CLI Argument Parsing Tests
// -----------------------------------------------------------------------------

TEST(ComplianceTest, ParsesComplianceCommandAndFlags) {
  const std::vector<std::string> args = {
      "compliance",
      "--required-tags", "Environment,Owner,CostCenter",
      "--min-compliance", "85.5",
      "-o", "html",
  };

  const auto options = azdash::parse_args(args);

  EXPECT_EQ(options.command, azdash::CommandKind::Compliance);
  ASSERT_EQ(options.required_tags.size(), 3u);
  EXPECT_EQ(options.required_tags[0], "Environment");
  EXPECT_EQ(options.required_tags[1], "Owner");
  EXPECT_EQ(options.required_tags[2], "CostCenter");
  EXPECT_DOUBLE_EQ(options.min_compliance_percent, 85.5);
  EXPECT_EQ(options.output, azdash::OutputFormat::Html);
}

TEST(ComplianceTest, ParsesTagsAlias) {
  const std::vector<std::string> args = {"tags"};
  const auto options = azdash::parse_args(args);
  EXPECT_EQ(options.command, azdash::CommandKind::Compliance);
}

// -----------------------------------------------------------------------------
// CLI Execution & CI/CD Threshold Gate Tests
// -----------------------------------------------------------------------------

TEST(ComplianceTest, FailsWithExitCode2WhenBelowMinComplianceThreshold) {
  azdash::TagComplianceSummary summary;
  summary.total_resources = 10;
  summary.compliant_resources = 6;
  summary.non_compliant_resources = 4;
  summary.compliance_percentage = 60.0;

  FakeComplianceProvider compliance_provider{summary};
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider;
  FakeTrendProvider trend_provider;
  FakeWasteProvider waste_provider;
  FakeReportWriter report_writer;
  FakeSubscriptionAliasStore alias_store;
  FakeCostHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{
      out, err, account_provider, cost_provider, trend_provider, waste_provider,
      report_writer, alias_store, history_store, nullptr, nullptr, nullptr,
      nullptr, nullptr, &compliance_provider,
  };

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Compliance;
  options.min_compliance_percent = 80.0; // 60% < 80% -> must fail

  const int code = azdash::run(options, runtime);

  EXPECT_EQ(code, 2);
  EXPECT_NE(err.str().find("below required minimum threshold"), std::string::npos);
}

TEST(ComplianceTest, SucceedsWhenAboveMinComplianceThreshold) {
  azdash::TagComplianceSummary summary;
  summary.total_resources = 10;
  summary.compliant_resources = 9;
  summary.non_compliant_resources = 1;
  summary.compliance_percentage = 90.0;

  FakeComplianceProvider compliance_provider{summary};
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider;
  FakeTrendProvider trend_provider;
  FakeWasteProvider waste_provider;
  FakeReportWriter report_writer;
  FakeSubscriptionAliasStore alias_store;
  FakeCostHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{
      out, err, account_provider, cost_provider, trend_provider, waste_provider,
      report_writer, alias_store, history_store, nullptr, nullptr, nullptr,
      nullptr, nullptr, &compliance_provider,
  };

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Compliance;
  options.min_compliance_percent = 85.0; // 90% >= 85% -> succeeds

  const int code = azdash::run(options, runtime);

  EXPECT_EQ(code, 0);
  EXPECT_TRUE(err.str().empty());
}

TEST(ComplianceTest, WebhookDispatchesComplianceAlert) {
  azdash::TagComplianceSummary summary;
  summary.total_resources = 10;
  summary.compliant_resources = 5;
  summary.non_compliant_resources = 5;
  summary.compliance_percentage = 50.0;
  summary.unallocated_spend = 500.0;
  summary.missing_tag_counts["Environment"] = 5;

  FakeComplianceProvider compliance_provider{summary};
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider;
  FakeTrendProvider trend_provider;
  FakeWasteProvider waste_provider;
  FakeReportWriter report_writer;
  FakeSubscriptionAliasStore alias_store;
  FakeCostHistoryStore history_store;
  FakeWebhookSender webhook_sender;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{
      out, err, account_provider, cost_provider, trend_provider, waste_provider,
      report_writer, alias_store, history_store, &webhook_sender, nullptr, nullptr,
      nullptr, nullptr, &compliance_provider,
  };

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Compliance;
  options.webhook_url = "https://hooks.slack.com/services/T00/B00/X00";
  options.min_compliance_percent = 80.0;

  const int code = azdash::run(options, runtime);

  EXPECT_EQ(code, 2); // Still breaches threshold
  ASSERT_EQ(webhook_sender.urls.size(), 1u);
  EXPECT_EQ(webhook_sender.urls.front(), options.webhook_url);
  EXPECT_EQ(webhook_sender.payloads.front().status, "danger");
  EXPECT_NE(webhook_sender.payloads.front().message.find("50.0%"), std::string::npos);
}

TEST(ComplianceTest, GeneratesRemediationScriptForNonCompliantResources) {
  azdash::TagComplianceSummary summary;
  summary.total_resources = 1;
  summary.compliant_resources = 0;
  summary.non_compliant_resources = 1;
  summary.compliance_percentage = 0.0;
  summary.non_compliant_items.push_back({
      .resource_name = "vm-untagged",
      .resource_group = "rg-dev",
      .resource_type = "Microsoft.Compute/virtualMachines",
      .cost = 120.0,
      .currency = "USD",
      .missing_tags = {"Environment", "Owner"},
      .tags = {{"Project", "Phoenix"}},
  });

  FakeComplianceProvider compliance_provider{summary};
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider;
  FakeTrendProvider trend_provider;
  FakeWasteProvider waste_provider;
  FakeReportWriter report_writer;
  FakeSubscriptionAliasStore alias_store;
  FakeCostHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{
      out, err, account_provider, cost_provider, trend_provider, waste_provider,
      report_writer, alias_store, history_store, nullptr, nullptr, nullptr,
      nullptr, nullptr, &compliance_provider,
  };

  const auto script_path = std::filesystem::temp_directory_path() / "test_tag_remediation.sh";
  if (std::filesystem::exists(script_path)) {
    std::filesystem::remove(script_path);
  }

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Compliance;
  options.remediation_path = script_path.string();

  const int code = azdash::run(options, runtime);
  EXPECT_EQ(code, 0);

  std::string content;
  {
    std::ifstream in(script_path);
    content.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  EXPECT_NE(content.find("az resource tag --name \"vm-untagged\""), std::string::npos);
  EXPECT_NE(content.find("\"Environment=unassigned\""), std::string::npos);
  EXPECT_NE(content.find("\"Owner=unassigned\""), std::string::npos);
  EXPECT_NE(content.find("\"Project=Phoenix\""), std::string::npos);

  std::error_code ec;
  std::filesystem::remove(script_path, ec);
}

TEST(ComplianceTest, DryRunSkipsRemediationScriptCreation) {
  azdash::TagComplianceSummary summary;
  summary.total_resources = 1;
  summary.non_compliant_resources = 1;
  summary.non_compliant_items.push_back({
      .resource_name = "vm-untagged",
      .missing_tags = {"Environment"},
  });

  FakeComplianceProvider compliance_provider{summary};
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider;
  FakeTrendProvider trend_provider;
  FakeWasteProvider waste_provider;
  FakeReportWriter report_writer;
  FakeSubscriptionAliasStore alias_store;
  FakeCostHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{
      out, err, account_provider, cost_provider, trend_provider, waste_provider,
      report_writer, alias_store, history_store, nullptr, nullptr, nullptr,
      nullptr, nullptr, &compliance_provider,
  };

  const auto script_path = std::filesystem::temp_directory_path() / "test_tag_remediation_dryrun.sh";
  if (std::filesystem::exists(script_path)) {
    std::filesystem::remove(script_path);
  }

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Compliance;
  options.remediation_path = script_path.string();
  options.dry_run = true;

  const int code = azdash::run(options, runtime);
  EXPECT_EQ(code, 0);
  EXPECT_FALSE(std::filesystem::exists(script_path));
  EXPECT_NE(out.str().find("Dry Run Complete"), std::string::npos);
}

} // namespace
