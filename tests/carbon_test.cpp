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
    return {"sub-carbon-123", "GreenOps Subscription", "tenant-456", "sustainability@company.com"};
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
  [[nodiscard]] auto six_month_trends(const azdash::CliOptions& /*options*/) const
      -> std::vector<azdash::MonthCost> override {
    return {};
  }
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

TEST(GreenOpsCarbonAnalyticsTest, ReturnsAccurateRegionalGridIntensity) {
  EXPECT_DOUBLE_EQ(azdash::get_regional_grid_intensity("swedencentral"), 18.0);
  EXPECT_DOUBLE_EQ(azdash::get_regional_grid_intensity("SwedenCentral"), 18.0);
  EXPECT_DOUBLE_EQ(azdash::get_regional_grid_intensity("francecentral"), 55.0);
  EXPECT_DOUBLE_EQ(azdash::get_regional_grid_intensity("westeurope"), 215.0);
  EXPECT_DOUBLE_EQ(azdash::get_regional_grid_intensity("eastus"), 380.0);
  EXPECT_DOUBLE_EQ(azdash::get_regional_grid_intensity("EASTUS"), 380.0);
  EXPECT_DOUBLE_EQ(azdash::get_regional_grid_intensity("unknown-region-name"), 350.0);
}

TEST(GreenOpsCarbonAnalyticsTest, CalculatesEmissionsAndEnergyAccurately) {
  std::vector<azdash::ServiceCost> costs{
      {"Virtual Machines", 1000.0, {}, "USD"}, // 0.35 kWh/$ -> 350 kWh
      {"Storage", 500.0, {}, "USD"},           // 0.08 kWh/$ -> 40 kWh
      {"App Service", 200.0, {}, "USD"},       // 0.22 kWh/$ -> 44 kWh
  };

  const auto assessment = azdash::estimate_carbon_footprint(costs, {}, "swedencentral");

  EXPECT_EQ(assessment.region, "swedencentral");
  EXPECT_NEAR(assessment.total_energy_kwh, 434.0, 0.01);
  EXPECT_GT(assessment.total_emissions_kg, 0.0);
  EXPECT_DOUBLE_EQ(assessment.total_emissions_mt, assessment.total_emissions_kg / 1000.0);
  EXPECT_GT(assessment.scope2_location_based_kg, 0.0);
  EXPECT_GT(assessment.scope3_embodied_kg, 0.0);
  EXPECT_EQ(assessment.services.size(), 3u);

  // Compare East US vs Sweden Central: East US should have higher emissions
  const auto eastus_assessment = azdash::estimate_carbon_footprint(costs, {}, "eastus");
  EXPECT_GT(eastus_assessment.total_emissions_kg, assessment.total_emissions_kg * 10.0);
}

TEST(GreenOpsCarbonAnalyticsTest, CalculatesWasteCarbonAvoidanceAndEquivalencies) {
  std::vector<azdash::ServiceCost> costs{
      {"Virtual Machines", 2000.0, {}, "USD"},
  };
  std::vector<azdash::WasteFinding> waste{
      {"unattached_disk", "/d1", "Microsoft.Compute/disks", "d1", "eastus", "Delete", 200.0, "USD"},
      {"idle_nat", "/n1", "Microsoft.Network/natGateways", "n1", "eastus", "Delete", 32.40, "USD"},
  };

  const auto assessment = azdash::estimate_carbon_footprint(costs, waste, "eastus");

  EXPECT_GT(assessment.avoidable_emissions_kg, 0.0);
  EXPECT_GT(assessment.avoidable_emissions_percentage, 0.0);
  EXPECT_LE(assessment.avoidable_emissions_percentage, 100.0);
  EXPECT_GT(assessment.equivalent_cars_per_year, 0.0);
  EXPECT_GT(assessment.equivalent_tree_seedlings, 0.0);
  EXPECT_FALSE(assessment.sustainability_tips.empty());
}

TEST(GreenOpsCarbonRenderTest, RendersAllOutputFormats) {
  azdash::CarbonFootprintAssessment assessment;
  assessment.region = "eastus";
  assessment.total_energy_kwh = 1200.0;
  assessment.total_emissions_kg = 524.4;
  assessment.total_emissions_mt = 0.5244;
  assessment.scope2_location_based_kg = 419.52;
  assessment.scope2_market_based_kg = 62.93;
  assessment.scope3_embodied_kg = 104.88;
  assessment.avoidable_emissions_kg = 85.0;
  assessment.avoidable_emissions_percentage = 16.2;
  assessment.equivalent_cars_per_year = 0.114;
  assessment.equivalent_tree_seedlings = 8.74;
  assessment.services = {
      {"Virtual Machines", 1000.0, 350.0, 150.0, 37.5, 187.5, 0.0},
  };
  assessment.sustainability_tips = {"Tip 1", "Tip 2"};

  // JSON
  {
    std::ostringstream ss;
    azdash::render_carbon(assessment, azdash::OutputFormat::Json, ss);
    const auto obj = nlohmann::json::parse(ss.str());
    EXPECT_EQ(obj["region"].get<std::string>(), "eastus");
    EXPECT_NEAR(obj["totalEmissionsKg"].get<double>(), 524.4, 0.1);
    EXPECT_EQ(obj["services"].size(), 1u);
  }

  // CSV
  {
    std::ostringstream ss;
    azdash::render_carbon(assessment, azdash::OutputFormat::Csv, ss);
    const auto csv = ss.str();
    EXPECT_NE(csv.find("service,cost,energy_kwh,scope2_kg,scope3_kg,total_emissions_kg"), std::string::npos);
    EXPECT_NE(csv.find("Virtual Machines"), std::string::npos);
    EXPECT_NE(csv.find("Total (eastus)"), std::string::npos);
  }

  // Markdown
  {
    std::ostringstream ss;
    azdash::render_carbon(assessment, azdash::OutputFormat::Markdown, ss);
    const auto md = ss.str();
    EXPECT_NE(md.find("# Cloud Sustainability & Carbon Footprint Report"), std::string::npos);
    EXPECT_NE(md.find("Virtual Machines"), std::string::npos);
    EXPECT_NE(md.find("Tip 1"), std::string::npos);
  }

  // HTML
  {
    std::ostringstream ss;
    azdash::render_carbon(assessment, azdash::OutputFormat::Html, ss);
    const auto html = ss.str();
    EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(html.find("GreenOps Cloud Carbon Footprint"), std::string::npos);
  }

  // Table
  {
    std::ostringstream ss;
    azdash::render_carbon(assessment, azdash::OutputFormat::Table, ss);
    const auto table = ss.str();
    EXPECT_NE(table.find("GreenOps Carbon Footprint"), std::string::npos);
    EXPECT_NE(table.find("Virtual Machines"), std::string::npos);
  }
}

TEST(GreenOpsCarbonCliTest, ExitsZeroWhenCarbonWithinLimit) {
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider({{"Virtual Machines", 500.0, {}, "USD"}});
  FakeTrendProvider trend_provider;
  FakeWasteProvider waste_provider({});
  FakeReportWriter report_writer;
  FakeAliasStore alias_store;
  FakeCostHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, account_provider, cost_provider, trend_provider,
                             waste_provider, report_writer, alias_store, history_store};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Carbon;
  options.output = azdash::OutputFormat::Json;
  options.fail_if_carbon_exceeds = 5.0; // 5 Metric Tons limit (spend produces << 5 MT)

  const int code = azdash::run(options, runtime);
  EXPECT_EQ(code, 0);
  EXPECT_TRUE(err.str().empty());
}

TEST(GreenOpsCarbonCliTest, ExitsCodeTwoWhenCarbonExceedsLimit) {
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider({{"Virtual Machines", 50000.0, {}, "USD"}});
  FakeTrendProvider trend_provider;
  FakeWasteProvider waste_provider({});
  FakeReportWriter report_writer;
  FakeAliasStore alias_store;
  FakeCostHistoryStore history_store;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, account_provider, cost_provider, trend_provider,
                             waste_provider, report_writer, alias_store, history_store};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Carbon;
  options.output = azdash::OutputFormat::Json;
  options.fail_if_carbon_exceeds = 0.001; // Strict limit: 0.001 MT (1 kg)

  const int code = azdash::run(options, runtime);
  EXPECT_EQ(code, 2);
  EXPECT_NE(err.str().find("exceeds limit of"), std::string::npos);
}

TEST(GreenOpsCarbonCliTest, DispatchesWebhookNotification) {
  FakeAccountProvider account_provider;
  FakeCostProvider cost_provider({{"Compute", 1000.0, {}, "USD"}});
  FakeTrendProvider trend_provider;
  FakeWasteProvider waste_provider({});
  FakeReportWriter report_writer;
  FakeAliasStore alias_store;
  FakeCostHistoryStore history_store;
  MockWebhookSender webhook_sender;

  std::ostringstream out;
  std::ostringstream err;
  azdash::CliRuntime runtime{out, err, account_provider, cost_provider, trend_provider,
                             waste_provider, report_writer, alias_store, history_store,
                             &webhook_sender};

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Carbon;
  options.webhook_url = "https://hooks.slack.com/services/test-carbon";

  const int code = azdash::run(options, runtime);
  EXPECT_EQ(code, 0);
  EXPECT_TRUE(webhook_sender.send_called);
  EXPECT_EQ(webhook_sender.captured_url, "https://hooks.slack.com/services/test-carbon");
  EXPECT_EQ(webhook_sender.captured_payload.title, "GreenOps Cloud Carbon Footprint Alert");
}
