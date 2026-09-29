#include "az_dashboard/render.hpp"

#include <gtest/gtest.h>
#include <sstream>

namespace {

TEST(RenderTest, CostCsvEscapesQuotesAndCommas) {
  std::ostringstream out;

  azdash::render_costs({{.service = "Storage, \"Hot\"", .previous = 1.0, .current = 2.0, .delta = 1.0, .delta_percent = 100.0}},
                       0.0, azdash::OutputFormat::Csv, out);

  EXPECT_EQ(out.str(), "service,previous,current,delta,delta_percent\n\"Storage, \"\"Hot\"\"\",1.00,2.00,1.00,100.0%\n");
}

TEST(RenderTest, CostCsvNeutralizesFormulaInjection) {
  std::ostringstream out;

  azdash::render_costs({{.service = "=cmd", .current = 1.0}}, 0.0, azdash::OutputFormat::Csv, out);

  EXPECT_EQ(out.str(), "service,previous,current,delta,delta_percent\n'=cmd,0.00,1.00,0.00,0.0%\n");
}

TEST(RenderTest, CostCsvKeepsNumericCellsRaw) {
  std::ostringstream out;

  azdash::render_costs(
      {{.service = "VM", .previous = 5.0, .current = 3.0, .delta = -2.0, .delta_percent = -40.0}},
      0.0, azdash::OutputFormat::Csv, out);

  EXPECT_EQ(out.str(), "service,previous,current,delta,delta_percent\nVM,5.00,3.00,-2.00,-40.0%\n");
}

TEST(RenderTest, TrendJsonPreservesServiceDetails) {
  std::ostringstream out;

  azdash::render_trends({{.month = "2026-05", .total = 12.5, .services = {{"VM", 7.5}, {"Storage", 5.0}}}},
                        azdash::OutputFormat::Json, out);

  EXPECT_EQ(out.str(),
            "[\n"
            "  {\n"
            "    \"currency\": \"USD\",\n"
            "    \"month\": \"2026-05\",\n"
            "    \"services\": [\n"
            "      {\n"
            "        \"cost\": 7.5,\n"
            "        \"currency\": \"USD\",\n"
            "        \"service\": \"VM\"\n"
            "      },\n"
            "      {\n"
            "        \"cost\": 5.0,\n"
            "        \"currency\": \"USD\",\n"
            "        \"service\": \"Storage\"\n"
            "      }\n"
            "    ],\n"
            "    \"total\": 12.5\n"
            "  }\n"
            "]\n");
}

TEST(RenderTest, CostJsonIncludesCurrency) {
  std::ostringstream out;

  azdash::render_costs({{.service = "VM", .previous = 100.0, .current = 150.0, .delta = 50.0, .delta_percent = 50.0, .currency = "EUR"}},
                       200.0, azdash::OutputFormat::Json, out);

  const auto content = out.str();
  EXPECT_NE(content.find("\"currency\": \"EUR\""), std::string::npos);
  EXPECT_NE(content.find("\"service\": \"VM\""), std::string::npos);
}

TEST(RenderTest, TrendCsvNeutralizesFormulaInjection) {
  std::ostringstream out;

  azdash::render_trends({{.month = "+2026-05", .total = 12.0}}, azdash::OutputFormat::Csv, out);

  EXPECT_EQ(out.str(), "month,total\n'+2026-05,12.00\n");
}

TEST(RenderTest, WasteCsvNeutralizesFormulaInjectionAndEscapesQuotes) {
  std::ostringstream out;

  azdash::render_waste({{.check = "@advisor",
                         .resource_id = "\tid",
                         .resource_type = "Microsoft.Compute/disks",
                         .name = "-disk",
                         .location = "westus",
                         .recommendation = "delete, \"if unused\"",
                         .estimated_monthly_savings = 3.0}},
                       azdash::OutputFormat::Csv, out);

  EXPECT_EQ(out.str(),
            "check,resource_type,name,location,estimated_monthly_savings,recommendation,resource_id\n"
            "'@advisor,Microsoft.Compute/disks,'-disk,westus,3.00,\"delete, \"\"if unused\"\"\",'\tid\n");
}

TEST(RenderTest, TableOutputIncludesProgressBars) {
  std::ostringstream out;

  azdash::render_trends({{.month = "2026-04", .total = 5.0}, {.month = "2026-05", .total = 10.0}},
                        azdash::OutputFormat::Table, out);

  EXPECT_NE(out.str().find("Spend Bar"), std::string::npos);
  EXPECT_NE(out.str().find("[#########---------]"), std::string::npos);
  EXPECT_NE(out.str().find("[##################]"), std::string::npos);
}

TEST(RenderTest, AliasCsvEscapesAliasValues) {
  std::ostringstream out;

  azdash::render_subscription_aliases({{.alias = "=prod", .subscription = "sub,id"}}, azdash::OutputFormat::Csv, out);

  EXPECT_EQ(out.str(), "alias,subscription\n'=prod,\"sub,id\"\n");
}

TEST(RenderTest, CostMarkdownRendersTableWithProjection) {
  std::ostringstream out;

  azdash::render_costs(
      {{.service = "Storage", .previous = 10.0, .current = 25.0, .delta = 15.0, .delta_percent = 150.0}},
      30.0, azdash::OutputFormat::Markdown, out);

  EXPECT_EQ(out.str(),
            "| Service | Previous | Current | Delta | Delta % |\n"
            "| --- | --- | --- | --- | --- |\n"
            "| Storage | 10.00 | 25.00 | 15.00 | 150.0% |\n"
            "\n"
            "**Projected end-of-month total:** 30.00\n");
}

TEST(RenderTest, CostMarkdownEscapesPipesAndOmitsMissingProjection) {
  std::ostringstream out;

  azdash::render_costs({{.service = "A|B", .previous = 1.0, .current = 2.0, .delta = 1.0, .delta_percent = 100.0}},
                       0.0, azdash::OutputFormat::Markdown, out);

  EXPECT_EQ(out.str(),
            "| Service | Previous | Current | Delta | Delta % |\n"
            "| --- | --- | --- | --- | --- |\n"
            "| A\\|B | 1.00 | 2.00 | 1.00 | 100.0% |\n");
}

TEST(RenderTest, TrendMarkdownRendersMonthsWithoutBars) {
  std::ostringstream out;

  azdash::render_trends({{.month = "2026-06", .total = 5.0}, {.month = "2026-07", .total = 10.0}},
                        azdash::OutputFormat::Markdown, out);

  EXPECT_EQ(out.str(),
            "| Month | Total |\n"
            "| --- | --- |\n"
            "| 2026-06 | 5.00 |\n"
            "| 2026-07 | 10.00 |\n");
}

TEST(RenderTest, WasteMarkdownFlattensMultilineRecommendations) {
  std::ostringstream out;

  azdash::render_waste({{.check = "compute",
                         .resource_id = "disk-id",
                         .resource_type = "Microsoft.Compute/disks",
                         .name = "disk-1",
                         .location = "westus",
                         .recommendation = "delete\nif unused",
                         .estimated_monthly_savings = 3.0}},
                       azdash::OutputFormat::Markdown, out);

  EXPECT_EQ(out.str(),
            "| Check | Type | Name | Location | Savings | Recommendation |\n"
            "| --- | --- | --- | --- | --- | --- |\n"
            "| compute | Microsoft.Compute/disks | disk-1 | westus | 3.00 | delete if unused |\n");
}

TEST(RenderTest, AliasMarkdownRendersRows) {
  std::ostringstream out;

  azdash::render_subscription_aliases({{.alias = "prod", .subscription = "sub-id"}},
                                      azdash::OutputFormat::Markdown, out);

  EXPECT_EQ(out.str(),
            "| Alias | Subscription |\n"
            "| --- | --- |\n"
            "| prod | sub-id |\n");
}

TEST(RenderTest, HistoryMarkdownRendersSnapshots) {
  std::ostringstream out;

  azdash::render_cost_history({{.timestamp = "2026-07-01T10:00:00Z", .subscription = "sub-1", .total = 10.0}},
                              azdash::OutputFormat::Markdown, out);

  EXPECT_EQ(out.str(),
            "| Timestamp | Subscription | Total |\n"
            "| --- | --- | --- |\n"
            "| 2026-07-01T10:00:00Z | sub-1 | 10.00 |\n");
}

TEST(RenderTest, CostHtmlRendersValidHtmlDocument) {
  std::ostringstream out;

  azdash::render_costs({{.service = "Virtual Machines", .previous = 100.0, .current = 150.0, .delta = 50.0, .delta_percent = 50.0}},
                       200.0, azdash::OutputFormat::Html, out);

  const std::string html = out.str();
  EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
  EXPECT_NE(html.find("<title>azdash Report</title>"), std::string::npos);
  EXPECT_NE(html.find("<th>Service</th>"), std::string::npos);
  EXPECT_NE(html.find("<td>Virtual Machines</td>"), std::string::npos);
  EXPECT_NE(html.find("150.00"), std::string::npos);
}

TEST(RenderTest, TrendHtmlRendersValidHtmlDocument) {
  std::ostringstream out;

  azdash::render_trends({{.month = "2026-06", .total = 250.0}}, azdash::OutputFormat::Html, out);

  const std::string html = out.str();
  EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
  EXPECT_NE(html.find("<th>Month</th>"), std::string::npos);
  EXPECT_NE(html.find("<td>2026-06</td>"), std::string::npos);
  EXPECT_NE(html.find("250.00"), std::string::npos);
}

TEST(RenderTest, AnomalyJsonRendersVerdictAndRootCauses) {
  std::ostringstream out;
  azdash::CostAnomalyAssessment assessment{
      .enough_data = true,
      .anomalous = true,
      .zscore = 3.5,
      .mean = 200.0,
      .stddev = 15.0,
      .evaluated_total = 450.0,
      .currency = "USD",
      .root_causes = {
          {
              .service = "Virtual Machines",
              .current_cost = 350.0,
              .baseline_mean = 100.0,
              .cost_delta = 250.0,
              .percentage_change = 250.0,
              .contribution_percent = 100.0,
              .impact = "Critical",
              .currency = "USD"
          }
      }
  };

  azdash::render_anomaly(assessment, azdash::OutputFormat::Json, out);
  const std::string json = out.str();
  EXPECT_NE(json.find("\"anomalous\": true"), std::string::npos);
  EXPECT_NE(json.find("\"zscore\": 3.5"), std::string::npos);
  EXPECT_NE(json.find("\"service\": \"Virtual Machines\""), std::string::npos);
  EXPECT_NE(json.find("\"impact\": \"Critical\""), std::string::npos);
}

TEST(RenderTest, AnomalyCsvRendersDriverHeadersAndRows) {
  std::ostringstream out;
  azdash::CostAnomalyAssessment assessment{
      .enough_data = true,
      .anomalous = true,
      .zscore = 2.5,
      .mean = 100.0,
      .stddev = 10.0,
      .evaluated_total = 200.0,
      .currency = "USD",
      .root_causes = {
          {
              .service = "Storage",
              .current_cost = 150.0,
              .baseline_mean = 50.0,
              .cost_delta = 100.0,
              .percentage_change = 200.0,
              .contribution_percent = 100.0,
              .impact = "Critical",
              .currency = "USD"
          }
      }
  };

  azdash::render_anomaly(assessment, azdash::OutputFormat::Csv, out);
  const std::string csv = out.str();
  EXPECT_NE(csv.find("service,baseline_mean,current_cost,cost_delta,percentage_change,contribution_percent,impact,currency"), std::string::npos);
  EXPECT_NE(csv.find("Storage,50.00,150.00,100.00,200.0%,100.0%,Critical,USD"), std::string::npos);
}

TEST(RenderTest, AnomalyMarkdownRendersStatusAndRootCausesTable) {
  std::ostringstream out;
  azdash::CostAnomalyAssessment assessment{
      .enough_data = true,
      .anomalous = true,
      .zscore = 2.8,
      .mean = 500.0,
      .stddev = 25.0,
      .evaluated_total = 750.0,
      .currency = "USD",
      .root_causes = {
          {
              .service = "App Services",
              .current_cost = 400.0,
              .baseline_mean = 150.0,
              .cost_delta = 250.0,
              .percentage_change = 166.67,
              .contribution_percent = 100.0,
              .impact = "Critical",
              .currency = "USD"
          }
      }
  };

  azdash::render_anomaly(assessment, azdash::OutputFormat::Markdown, out);
  const std::string md = out.str();
  EXPECT_NE(md.find("# Azure Cost Anomaly Assessment"), std::string::npos);
  EXPECT_NE(md.find("ANOMALOUS (Cost Spike Detected)"), std::string::npos);
  EXPECT_NE(md.find("| Service | Baseline Mean | Current Cost | Delta | Change | Spike Share | Impact |"), std::string::npos);
  EXPECT_NE(md.find("App Services"), std::string::npos);
  EXPECT_NE(md.find("Critical"), std::string::npos);
}

TEST(RenderTest, AnomalyHtmlRendersValidHtmlDocument) {
  std::ostringstream out;
  azdash::CostAnomalyAssessment assessment{
      .enough_data = true,
      .anomalous = false,
      .zscore = 0.5,
      .mean = 300.0,
      .stddev = 20.0,
      .evaluated_total = 310.0,
      .currency = "USD"
  };

  azdash::render_anomaly(assessment, azdash::OutputFormat::Html, out);
  const std::string html = out.str();
  EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
  EXPECT_NE(html.find("<title>Azure Cost Anomaly Report</title>"), std::string::npos);
  EXPECT_NE(html.find("NORMAL"), std::string::npos);
  EXPECT_NE(html.find("Evaluated Total"), std::string::npos);
}

TEST(RenderTest, AnomalyTableRendersStyledBoxAndDrivers) {
  std::ostringstream out;
  azdash::CostAnomalyAssessment assessment{
      .enough_data = true,
      .anomalous = true,
      .zscore = 3.1,
      .mean = 100.0,
      .stddev = 10.0,
      .evaluated_total = 250.0,
      .currency = "USD",
      .root_causes = {
          {
              .service = "Virtual Machines",
              .current_cost = 200.0,
              .baseline_mean = 50.0,
              .cost_delta = 150.0,
              .percentage_change = 300.0,
              .contribution_percent = 100.0,
              .impact = "Critical",
              .currency = "USD"
          }
      }
  };

  azdash::render_anomaly(assessment, azdash::OutputFormat::Table, out);
  const std::string table = out.str();
  EXPECT_NE(table.find("Anomaly detected"), std::string::npos);
  EXPECT_NE(table.find("z-score"), std::string::npos);
  EXPECT_NE(table.find("Virtual Machines"), std::string::npos);
}

} // namespace
