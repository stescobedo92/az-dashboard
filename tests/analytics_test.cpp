#include "az_dashboard/analytics.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

struct ExternalCostRow {
  std::string_view label;
  int cents;
};

TEST(AnalyticsTest, CompareCostsIncludesCurrentAndPreviousOnlyServices) {
  const std::vector<azdash::ServiceCost> current{{"Virtual Machines", 150.0}, {"Storage", 25.0}};
  const std::vector<azdash::ServiceCost> previous{{"Virtual Machines", 100.0}, {"SQL", 50.0}};

  const auto rows = azdash::compare_costs(current, previous);

  ASSERT_EQ(rows.size(), 3);
  EXPECT_EQ(rows[0].service, "Virtual Machines");
  EXPECT_DOUBLE_EQ(rows[0].delta, 50.0);
  EXPECT_DOUBLE_EQ(rows[0].delta_percent, 50.0);
}

TEST(AnalyticsTest, AggregateByUsesGenericSelectors) {
  const std::vector<azdash::ServiceCost> values{{"A", 1.0}, {"A", 2.5}, {"B", 3.0}};

  const auto totals = azdash::aggregate_by(values,
                                           [](const auto& value) { return value.service; },
                                           [](const auto& value) { return value.cost; });

  EXPECT_DOUBLE_EQ(totals.at("A"), 3.5);
  EXPECT_DOUBLE_EQ(totals.at("B"), 3.0);
}

TEST(AnalyticsTest, CompareCostsByUsesConstrainedSelectorsForExternalRows) {
  const std::vector<ExternalCostRow> current{{"Functions", 2500}, {"Storage", 1000}};
  const std::vector<ExternalCostRow> previous{{"Functions", 2000}, {"Sql", 900}};

  const auto rows = azdash::compare_costs_by(current,
                                             previous,
                                             [](const ExternalCostRow& value) { return value.label; },
                                             [](const ExternalCostRow& value) { return value.cents / 100.0; });

  ASSERT_EQ(rows.size(), 3);
  EXPECT_EQ(rows[0].service, "Functions");
  EXPECT_DOUBLE_EQ(rows[0].previous, 20.0);
  EXPECT_DOUBLE_EQ(rows[0].current, 25.0);
  EXPECT_DOUBLE_EQ(rows[0].delta_percent, 25.0);
}

TEST(AnalyticsTest, FilterSelectedIsCaseInsensitive) {
  const std::vector<azdash::WasteFinding> findings{
      {.check = "Compute", .name = "vm-a"},
      {.check = "Network", .name = "ip-a"},
  };

  const auto filtered = azdash::filter_selected(findings, {"compute"}, [](const auto& value) {
    return value.check;
  });

  ASSERT_EQ(filtered.size(), 1);
  EXPECT_EQ(filtered[0].name, "vm-a");
}

} // namespace
