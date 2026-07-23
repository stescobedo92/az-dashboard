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
            "    \"month\": \"2026-05\",\n"
            "    \"services\": [\n"
            "      {\n"
            "        \"cost\": 7.5,\n"
            "        \"service\": \"VM\"\n"
            "      },\n"
            "      {\n"
            "        \"cost\": 5.0,\n"
            "        \"service\": \"Storage\"\n"
            "      }\n"
            "    ],\n"
            "    \"total\": 12.5\n"
            "  }\n"
            "]\n");
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

} // namespace
