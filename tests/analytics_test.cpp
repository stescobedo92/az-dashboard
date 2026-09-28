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

TEST(AnalyticsTest, MeanAndSampleStddevComputeBasicStatistics) {
  const std::vector<double> values{100.0, 110.0, 90.0, 105.0, 95.0};

  EXPECT_DOUBLE_EQ(azdash::mean(values), 100.0);
  EXPECT_NEAR(azdash::sample_stddev(values), 7.9057, 1e-3);
  EXPECT_DOUBLE_EQ(azdash::mean({}), 0.0);
  EXPECT_DOUBLE_EQ(azdash::sample_stddev({42.0}), 0.0);
}

TEST(AnalyticsTest, AssessCostAnomalyFlagsHighZScore) {
  const auto assessment = azdash::assess_cost_anomaly({100.0, 110.0, 90.0, 105.0, 95.0}, 150.0);

  EXPECT_TRUE(assessment.enough_data);
  EXPECT_TRUE(assessment.anomalous);
  EXPECT_NEAR(assessment.zscore, 6.32, 1e-2);
  EXPECT_DOUBLE_EQ(assessment.mean, 100.0);
  EXPECT_DOUBLE_EQ(assessment.evaluated_total, 150.0);
}

TEST(AnalyticsTest, AssessCostAnomalyAcceptsTypicalSpend) {
  const auto assessment = azdash::assess_cost_anomaly({100.0, 110.0, 90.0, 105.0, 95.0}, 108.0);

  EXPECT_TRUE(assessment.enough_data);
  EXPECT_FALSE(assessment.anomalous);
}

TEST(AnalyticsTest, AssessCostAnomalyFlagsSpendCollapse) {
  const auto assessment = azdash::assess_cost_anomaly({100.0, 110.0, 90.0, 105.0, 95.0}, 40.0);

  EXPECT_TRUE(assessment.anomalous);
  EXPECT_LT(assessment.zscore, 0.0);
}

TEST(AnalyticsTest, AssessCostAnomalyFallsBackTo20PercentRuleOnZeroVariance) {
  EXPECT_TRUE(azdash::assess_cost_anomaly({100.0, 100.0, 100.0}, 125.0).anomalous);
  EXPECT_FALSE(azdash::assess_cost_anomaly({100.0, 100.0, 100.0}, 115.0).anomalous);
}

TEST(AnalyticsTest, AssessCostAnomalyRequiresAtLeastTwoPastTotals) {
  const auto assessment = azdash::assess_cost_anomaly({100.0}, 500.0);

  EXPECT_FALSE(assessment.enough_data);
  EXPECT_FALSE(assessment.anomalous);
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

TEST(AnalyticsTest, CompareCostsPropagatesCurrencyFromInputs) {
  const std::vector<azdash::ServiceCost> current{
      {.service = "Virtual Machines", .cost = 150.0, .tags = {}, .currency = "EUR"}};
  const std::vector<azdash::ServiceCost> previous{
      {.service = "Virtual Machines", .cost = 100.0, .tags = {}, .currency = "EUR"}};

  const auto rows = azdash::compare_costs(current, previous);

  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].currency, "EUR");
}

TEST(AnalyticsTest, ComputeProjectionLinearScalesCorrectly) {
  const double proj_linear = azdash::compute_projection(100.0);
  EXPECT_GE(proj_linear, 100.0);

  const double proj_explicit = azdash::compute_projection(100.0, azdash::ProjectionMode::Linear, {});
  EXPECT_DOUBLE_EQ(proj_linear, proj_explicit);
}

TEST(AnalyticsTest, ComputeProjectionWeightedStabilizesWithHistoricalBaseline) {
  const std::vector<double> history{500.0, 520.0, 510.0};
  const double proj_weighted = azdash::compute_projection(100.0, azdash::ProjectionMode::Weighted, history);
  EXPECT_GT(proj_weighted, 0.0);

  // When history is empty, weighted mode falls back to linear
  const double fallback = azdash::compute_projection(100.0, azdash::ProjectionMode::Weighted, {});
  const double linear = azdash::compute_projection(100.0, azdash::ProjectionMode::Linear, {});
  EXPECT_DOUBLE_EQ(fallback, linear);
}

} // namespace
