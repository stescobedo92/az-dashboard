#include "az_dashboard/ui.hpp"
#include "az_dashboard/cli.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

auto render_to_string(const ftxui::Element& element, int width = 120, int height = 30) -> std::string {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, element);
  return screen.ToString();
}

TEST(TuiTrendElementTest, RendersEmptyTrendMessage) {
  const auto element = azdash::render_tui_trend_element({}, "USD");
  const auto output = render_to_string(element);
  EXPECT_TRUE(output.find("No trend data available") != std::string::npos);
}

TEST(TuiTrendElementTest, RendersMonthsAndProjection) {
  const std::vector<azdash::MonthCost> trends = {
      {"2026-05", 100.0, {}, "USD"},
      {"2026-06", 150.0, {}, "EUR"},
  };

  const auto element = azdash::render_tui_trend_element(trends, "USD");
  const auto output = render_to_string(element);

  EXPECT_TRUE(output.find("2026-05") != std::string::npos);
  EXPECT_TRUE(output.find("100.00 USD") != std::string::npos);
  EXPECT_TRUE(output.find("2026-06") != std::string::npos);
  EXPECT_TRUE(output.find("150.00 EUR") != std::string::npos);
  EXPECT_TRUE(output.find("Projected Month End") != std::string::npos);
}

TEST(TuiWasteElementTest, RendersEmptyWasteMessage) {
  const auto element = azdash::render_tui_waste_element({}, 0);
  const auto output = render_to_string(element);
  EXPECT_TRUE(output.find("No waste detected") != std::string::npos);
}

TEST(TuiWasteElementTest, RendersWasteListAndSelectedDetails) {
  const std::vector<azdash::WasteFinding> findings = {
      {"compute", "/subscriptions/s/disks/d1", "Microsoft.Compute/disks", "prem-disk", "eastus", "Delete unattached disk", 25.50, "USD"},
      {"network", "/subscriptions/s/pips/p1", "Microsoft.Network/publicIPAddresses", "idle-pip", "westus", "Delete idle public IP", 4.00, "USD"},
  };

  // Selected index = 0 -> detail box shows disk recommendation
  const auto element = azdash::render_tui_waste_element(findings, 0);
  const auto output = render_to_string(element);

  EXPECT_TRUE(output.find("prem-disk") != std::string::npos);
  EXPECT_TRUE(output.find("idle-pip") != std::string::npos);
  EXPECT_TRUE(output.find("25.50 USD") != std::string::npos);
  EXPECT_TRUE(output.find("Finding Details") != std::string::npos);
  EXPECT_TRUE(output.find("Delete unattached disk") != std::string::npos);
}

TEST(TuiCostElementTest, RendersEmptyCostMessage) {
  const auto element = azdash::render_tui_cost_element({}, 0);
  const auto output = render_to_string(element);
  EXPECT_TRUE(output.find("No cost data available") != std::string::npos);
}

TEST(TuiCostElementTest, RendersServiceCostsAndTotal) {
  const std::vector<azdash::ServiceCost> costs = {
      {"Virtual Machines", 250.0, {}, "USD"},
      {"Azure Cosmos DB", 75.0, {}, "USD"},
  };

  const auto element = azdash::render_tui_cost_element(costs, 0);
  const auto output = render_to_string(element);

  EXPECT_TRUE(output.find("Virtual Machines") != std::string::npos);
  EXPECT_TRUE(output.find("250.00 USD") != std::string::npos);
  EXPECT_TRUE(output.find("Azure Cosmos DB") != std::string::npos);
  EXPECT_TRUE(output.find("75.00 USD") != std::string::npos);
  EXPECT_TRUE(output.find("Current Month Total") != std::string::npos);
  EXPECT_TRUE(output.find("325.00 USD") != std::string::npos);
}

TEST(CliParserUiTest, ParsesUiCommand) {
  const std::vector<std::string> args = {"ui"};
  const auto options = azdash::parse_args(args);
  EXPECT_EQ(options.command, azdash::CommandKind::UI);
}

TEST(TuiBudgetsCommitmentsElementTest, RendersBudgetsAndCommitments) {
  const std::vector<azdash::BudgetInfo> budgets = {
      {.name = "Dev-Budget", .amount = 500.0, .current_spend = 250.0, .time_grain = "Monthly", .currency = "USD"},
      {.name = "Prod-Budget", .amount = 1000.0, .current_spend = 1200.0, .time_grain = "Monthly", .currency = "USD"},
  };
  const std::vector<azdash::CommitmentRecommendation> commitments = {
      {.id = "c1", .type = "ReservedInstance", .sku = "Standard_D8s_v5", .region = "eastus", .term = "3 Years",
       .estimated_monthly_savings = 150.0, .estimated_monthly_cost = 200.0, .currency = "USD", .details = "3yr RI"},
  };

  const auto element = azdash::render_tui_budgets_commitments_element(budgets, commitments, 0);
  const auto output = render_to_string(element);

  EXPECT_TRUE(output.find("Dev-Budget") != std::string::npos);
  EXPECT_TRUE(output.find("500.00 USD") != std::string::npos);
  EXPECT_TRUE(output.find("Prod-Budget") != std::string::npos);
  EXPECT_TRUE(output.find("EXCEEDED") != std::string::npos);
  EXPECT_TRUE(output.find("ReservedInstance") != std::string::npos);
  EXPECT_TRUE(output.find("Standard_D8s_v5") != std::string::npos);
  EXPECT_TRUE(output.find("150.00 USD") != std::string::npos);
}

TEST(TuiComplianceElementTest, RendersComplianceKPICardsAndMissingTags) {
  azdash::TagComplianceSummary compliance;
  compliance.total_resources = 10;
  compliance.compliant_resources = 8;
  compliance.non_compliant_resources = 2;
  compliance.compliance_percentage = 80.0;
  compliance.total_spend = 1000.0;
  compliance.allocated_spend = 800.0;
  compliance.unallocated_spend = 200.0;
  compliance.currency = "USD";
  compliance.missing_tag_counts["Environment"] = 2;
  compliance.missing_tag_costs["Environment"] = 200.0;
  compliance.non_compliant_items.push_back({
      .resource_name = "vm-untagged",
      .resource_group = "rg-core",
      .resource_type = "Microsoft.Compute/virtualMachines",
      .cost = 200.0,
      .currency = "USD",
      .missing_tags = {"Environment"},
  });

  const auto element = azdash::render_tui_compliance_element(compliance, 0);
  const auto output = render_to_string(element);

  EXPECT_TRUE(output.find("80.0%") != std::string::npos);
  EXPECT_TRUE(output.find("200.00 USD") != std::string::npos);
  EXPECT_TRUE(output.find("800.00 USD") != std::string::npos);
  EXPECT_TRUE(output.find("Environment") != std::string::npos);
  EXPECT_TRUE(output.find("vm-untagged") != std::string::npos);
}

} // namespace
