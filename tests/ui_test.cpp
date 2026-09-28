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

} // namespace
