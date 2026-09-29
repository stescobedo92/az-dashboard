#include "az_dashboard/cli_parser.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace azdash {
namespace {

auto parse(std::initializer_list<std::string> args) -> CliOptions {
  std::vector<std::string> vec(args);
  return parse_args(vec);
}

TEST(CliParserTest, ParsesHelpTokensAndEmptyArgs) {
  EXPECT_EQ(parse({}).command, CommandKind::Help);
  EXPECT_EQ(parse({"help"}).command, CommandKind::Help);
  EXPECT_EQ(parse({"-h"}).command, CommandKind::Help);
  EXPECT_EQ(parse({"--help"}).command, CommandKind::Help);
  EXPECT_EQ(parse({"--subscription", "sub1", "help"}).command, CommandKind::Help);
  EXPECT_EQ(parse({"report", "help"}).command, CommandKind::Help);
}

TEST(CliParserTest, ParsesBasicCommands) {
  EXPECT_EQ(parse({"cost"}).command, CommandKind::Cost);
  EXPECT_EQ(parse({"anomaly"}).command, CommandKind::CostAnomaly);
  EXPECT_EQ(parse({"ui"}).command, CommandKind::UI);
  EXPECT_EQ(parse({"history"}).command, CommandKind::History);
  EXPECT_EQ(parse({"link-account"}).command, CommandKind::LinkAccount);
  EXPECT_EQ(parse({"link"}).command, CommandKind::LinkAccount);
  EXPECT_EQ(parse({"version"}).command, CommandKind::Version);
  EXPECT_EQ(parse({"update"}).command, CommandKind::Update);
}

TEST(CliParserTest, ParsesSelectorCommands) {
  const auto trend = parse({"trend", "Compute", "Storage"});
  EXPECT_EQ(trend.command, CommandKind::Trend);
  ASSERT_EQ(trend.selectors.size(), 2u);
  EXPECT_EQ(trend.selectors[0], "Compute");
  EXPECT_EQ(trend.selectors[1], "Storage");

  const auto waste = parse({"waste", "disks", "ips"});
  EXPECT_EQ(waste.command, CommandKind::Waste);
  ASSERT_EQ(waste.selectors.size(), 2u);
  EXPECT_EQ(waste.selectors[0], "disks");

  const auto budget = parse({"budget", "DevBudget"});
  EXPECT_EQ(budget.command, CommandKind::Budget);
  ASSERT_EQ(budget.selectors.size(), 1u);
  EXPECT_EQ(budget.selectors[0], "DevBudget");

  const auto commitments = parse({"commitments", "Standard_D2s_v5"});
  EXPECT_EQ(commitments.command, CommandKind::Commitments);
  ASSERT_EQ(commitments.selectors.size(), 1u);
  EXPECT_EQ(commitments.selectors[0], "Standard_D2s_v5");

  EXPECT_EQ(parse({"ri"}).command, CommandKind::Commitments);
  EXPECT_EQ(parse({"reservations"}).command, CommandKind::Commitments);
}

TEST(CliParserTest, ParsesReportCommands) {
  EXPECT_EQ(parse({"report", "cost"}).command, CommandKind::ReportCost);

  const auto r_trend = parse({"report", "trend", "Compute"});
  EXPECT_EQ(r_trend.command, CommandKind::ReportTrend);
  ASSERT_EQ(r_trend.selectors.size(), 1u);
  EXPECT_EQ(r_trend.selectors[0], "Compute");

  const auto r_waste = parse({"report", "waste", "disks"});
  EXPECT_EQ(r_waste.command, CommandKind::ReportWaste);
  ASSERT_EQ(r_waste.selectors.size(), 1u);
  EXPECT_EQ(r_waste.selectors[0], "disks");

  EXPECT_THROW(parse({"report"}), std::invalid_argument);
  EXPECT_THROW(parse({"report", "unknown"}), std::invalid_argument);
}

TEST(CliParserTest, ParsesAliasSubCommands) {
  const auto list1 = parse({"alias-sub", "list"});
  EXPECT_EQ(list1.command, CommandKind::AliasSub);
  EXPECT_EQ(list1.alias_action, AliasSubAction::List);

  const auto list2 = parse({"alias-sub"});
  EXPECT_EQ(list2.command, CommandKind::AliasSub);
  EXPECT_EQ(list2.alias_action, AliasSubAction::List);

  const auto set_cmd = parse({"alias-sub", "set", "prod", "sub-1234"});
  EXPECT_EQ(set_cmd.command, CommandKind::AliasSub);
  EXPECT_EQ(set_cmd.alias_action, AliasSubAction::Set);
  EXPECT_EQ(set_cmd.alias_name, "prod");
  EXPECT_EQ(set_cmd.alias_subscription, "sub-1234");

  const auto remove_cmd = parse({"alias-sub", "remove", "prod"});
  EXPECT_EQ(remove_cmd.command, CommandKind::AliasSub);
  EXPECT_EQ(remove_cmd.alias_action, AliasSubAction::Remove);
  EXPECT_EQ(remove_cmd.alias_name, "prod");

  const auto rm_cmd = parse({"alias-sub", "rm", "prod"});
  EXPECT_EQ(rm_cmd.command, CommandKind::AliasSub);
  EXPECT_EQ(rm_cmd.alias_action, AliasSubAction::Remove);
  EXPECT_EQ(rm_cmd.alias_name, "prod");

  const auto implicit_set = parse({"alias-sub", "dev", "sub-5678"});
  EXPECT_EQ(implicit_set.command, CommandKind::AliasSub);
  EXPECT_EQ(implicit_set.alias_action, AliasSubAction::Set);
  EXPECT_EQ(implicit_set.alias_name, "dev");
  EXPECT_EQ(implicit_set.alias_subscription, "sub-5678");

  EXPECT_THROW(parse({"alias-sub", "set", "prod"}), std::invalid_argument);
  EXPECT_THROW(parse({"alias-sub", "remove"}), std::invalid_argument);
}

TEST(CliParserTest, ParsesAllGlobalFlagsCorrectly) {
  const auto opts = parse({
      "--subscription", "sub-1",
      "--subscription", "sub-2",
      "--all-subscriptions",
      "--group-by", "resource-group",
      "--group-by-tag", "Env",
      "--filter-tag", "Env=Prod",
      "--generate-remediation", "cleanup.sh",
      "--tenant", "tenant-99",
      "-o", "json",
      "--path", "report.pdf",
      "--function-memory-threshold", "85",
      "--secrets-idle-days", "180",
      "--fail-if-exceeds", "2500.75",
      "--no-cache",
      "--fast",
      "--dry-run",
      "--webhook", "https://hooks.slack.com/services/test",
      "-i",
      "--projection", "weighted",
      "--mg", "mg-enterprise",
      "--budget", "BudgetQ3",
      "--term", "3yr",
      "--min-savings", "150.5",
      "--rest",
      "cost"
  });

  ASSERT_EQ(opts.subscriptions.size(), 2u);
  EXPECT_EQ(opts.subscriptions[0], "sub-1");
  EXPECT_EQ(opts.subscriptions[1], "sub-2");
  EXPECT_TRUE(opts.all_subscriptions);
  EXPECT_EQ(opts.group_by, GroupBy::ResourceGroup);
  ASSERT_EQ(opts.group_by_tags.size(), 1u);
  EXPECT_EQ(opts.group_by_tags[0], "Env");
  ASSERT_EQ(opts.filter_tags.size(), 1u);
  EXPECT_EQ(opts.filter_tags[0], "Env=Prod");
  EXPECT_EQ(opts.remediation_path, "cleanup.sh");
  EXPECT_EQ(opts.tenant, "tenant-99");
  EXPECT_EQ(opts.output, OutputFormat::Json);
  EXPECT_EQ(opts.report_path, "report.pdf");
  EXPECT_EQ(opts.function_memory_threshold_percent, 85);
  ASSERT_TRUE(opts.fail_if_exceeds_cost.has_value());
  EXPECT_DOUBLE_EQ(opts.fail_if_exceeds_cost.value(), 2500.75);
  EXPECT_TRUE(opts.no_cache);
  EXPECT_TRUE(opts.fast_query);
  EXPECT_TRUE(opts.dry_run);
  EXPECT_EQ(opts.webhook_url, "https://hooks.slack.com/services/test");
  EXPECT_TRUE(opts.interactive);
  EXPECT_EQ(opts.projection_mode, ProjectionMode::Weighted);
  EXPECT_EQ(opts.management_group, "mg-enterprise");
  EXPECT_EQ(opts.budget_filter, "BudgetQ3");
  EXPECT_EQ(opts.commitment_term, "3yr");
  EXPECT_DOUBLE_EQ(opts.min_savings, 150.5);
  EXPECT_TRUE(opts.use_rest);
}

TEST(CliParserTest, ValidatesFormatsAndEdgeCases) {
  EXPECT_EQ(parse({"-o", "csv", "cost"}).output, OutputFormat::Csv);
  EXPECT_EQ(parse({"-o", "markdown", "cost"}).output, OutputFormat::Markdown);
  EXPECT_EQ(parse({"-o", "md", "cost"}).output, OutputFormat::Markdown);
  EXPECT_EQ(parse({"-o", "table", "cost"}).output, OutputFormat::Table);
  EXPECT_THROW(parse({"-o", "xml", "cost"}), std::invalid_argument);

  EXPECT_EQ(parse({"--group-by", "rg", "cost"}).group_by, GroupBy::ResourceGroup);
  EXPECT_EQ(parse({"--group-by", "service", "cost"}).group_by, GroupBy::Service);
  EXPECT_THROW(parse({"--group-by", "invalid", "cost"}), std::invalid_argument);

  EXPECT_EQ(parse({"--projection", "linear", "cost"}).projection_mode, ProjectionMode::Linear);
  EXPECT_THROW(parse({"--projection", "unknown", "cost"}), std::invalid_argument);

  EXPECT_THROW(parse({"--unknown-flag", "cost"}), std::invalid_argument);
  EXPECT_THROW(parse({"--subscription"}), std::invalid_argument);
  EXPECT_THROW(parse({"--subscription", "-o"}), std::invalid_argument);
  EXPECT_THROW(parse({"--fail-if-exceeds", "abc", "cost"}), std::invalid_argument);
  EXPECT_THROW(parse({"--function-memory-threshold", "150", "cost"}), std::invalid_argument);
  EXPECT_THROW(parse({"--function-memory-threshold", "-5", "cost"}), std::invalid_argument);
  EXPECT_THROW(parse({"cost", "extra_argument"}), std::invalid_argument);
}

TEST(CliParserTest, HelpTextAndToStringCoverage) {
  const auto help = help_text();
  EXPECT_FALSE(help.empty());
  EXPECT_NE(help.find("azdash - Azure cost, trend, and waste diagnostics"), std::string::npos);
  EXPECT_NE(help.find("--rest"), std::string::npos);
  EXPECT_NE(help.find("--budget"), std::string::npos);
  EXPECT_NE(help.find("commitments"), std::string::npos);
  EXPECT_NE(help.find("--term"), std::string::npos);

  EXPECT_EQ(to_string(OutputFormat::Json), "json");
  EXPECT_EQ(to_string(OutputFormat::Csv), "csv");
  EXPECT_EQ(to_string(OutputFormat::Markdown), "markdown");
  EXPECT_EQ(to_string(OutputFormat::Table), "table");
}

TEST(CliParserTest, ParsesAnomalyAndRemediationFlags) {
  const auto opts = parse({
      "--anomaly-threshold", "2.75",
      "--fail-on-anomaly",
      "--remediation-format", "terraform",
      "--projection", "holt-winters",
      "anomaly"
  });

  EXPECT_EQ(opts.command, CommandKind::CostAnomaly);
  EXPECT_DOUBLE_EQ(opts.anomaly_threshold, 2.75);
  EXPECT_TRUE(opts.fail_on_anomaly);
  EXPECT_EQ(opts.remediation_format, "terraform");
  EXPECT_EQ(opts.projection_mode, ProjectionMode::HoltWinters);

  EXPECT_EQ(parse({"--projection", "exponential", "cost"}).projection_mode, ProjectionMode::HoltWinters);
  EXPECT_EQ(parse({"--remediation-format", "bicep", "waste"}).remediation_format, "bicep");

  EXPECT_THROW(parse({"--anomaly-threshold", "-1.0", "cost"}), std::invalid_argument);
  EXPECT_THROW(parse({"--anomaly-threshold", "0", "cost"}), std::invalid_argument);
  EXPECT_THROW(parse({"--anomaly-threshold", "invalid", "cost"}), std::invalid_argument);
  EXPECT_THROW(parse({"--remediation-format", "yaml", "cost"}), std::invalid_argument);
}

TEST(CliParserTest, ParsesAuditAndCarbonCommandsAndFlags) {
  const auto audit_opts = parse({"--min-score", "85.5", "audit"});
  EXPECT_EQ(audit_opts.command, CommandKind::Audit);
  EXPECT_DOUBLE_EQ(audit_opts.min_audit_score, 85.5);

  const auto scorecard_opts = parse({"scorecard"});
  EXPECT_EQ(scorecard_opts.command, CommandKind::Audit);

  const auto carbon_opts = parse({
      "--fail-if-carbon-exceeds", "2.5",
      "--default-region", "swedencentral",
      "carbon"
  });
  EXPECT_EQ(carbon_opts.command, CommandKind::Carbon);
  ASSERT_TRUE(carbon_opts.fail_if_carbon_exceeds.has_value());
  EXPECT_DOUBLE_EQ(*carbon_opts.fail_if_carbon_exceeds, 2.5);
  EXPECT_EQ(carbon_opts.default_region, "swedencentral");

  const auto greenops_opts = parse({"--region", "francecentral", "greenops"});
  EXPECT_EQ(greenops_opts.command, CommandKind::Carbon);
  EXPECT_EQ(greenops_opts.default_region, "francecentral");

  const auto sust_opts = parse({"sustainability"});
  EXPECT_EQ(sust_opts.command, CommandKind::Carbon);

  EXPECT_THROW(parse({"--min-score", "-5.0", "audit"}), std::invalid_argument);
  EXPECT_THROW(parse({"--min-score", "105.0", "audit"}), std::invalid_argument);
  EXPECT_THROW(parse({"--fail-if-carbon-exceeds", "-1.0", "carbon"}), std::invalid_argument);
  EXPECT_THROW(parse({"--fail-if-carbon-exceeds", "0", "carbon"}), std::invalid_argument);
}

} // namespace
} // namespace azdash
