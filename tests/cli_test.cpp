#include "az_dashboard/cli.hpp"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace azdash::detail {

[[nodiscard]] auto civil_date_for_month(std::chrono::year_month_day anchor,
                                        int month_offset,
                                        bool month_start) -> std::string;

} // namespace azdash::detail

namespace {

auto parse(std::initializer_list<std::string> values) -> azdash::CliOptions {
  const std::vector<std::string> args(values);
  return azdash::parse_args(std::span<const std::string>(args.data(), args.size()));
}

void expect_invalid_argument_message(std::initializer_list<std::string> values, const std::string& expected) {
  try {
    (void)parse(values);
    FAIL() << "expected std::invalid_argument";
  } catch (const std::invalid_argument& error) {
    EXPECT_EQ(error.what(), expected);
  }
}

class FakeCliRuntime final : public azdash::ICliAccountProvider,
                             public azdash::ICliCostProvider,
                             public azdash::ICliTrendProvider,
                             public azdash::ICliWasteProvider,
                             public azdash::ICliReportWriter,
                             public azdash::ICliSubscriptionAliasStore,
                             public azdash::ICliCostHistoryStore {
public:
  [[nodiscard]] auto runtime() const -> azdash::CliRuntime {
    return azdash::CliRuntime{out, err, *this, *this, *this, *this, *this, *this, *this};
  }

  [[nodiscard]] auto account(const azdash::CliOptions&) const -> azdash::AccountInfo override {
    ++account_calls;
    return {.subscription_id = "sub-id", .subscription_name = "Subscription", .tenant_id = "tenant", .user_name = "user"};
  }

  [[nodiscard]] auto current_month_costs(const azdash::CliOptions& options) const -> std::vector<azdash::ServiceCost> override {
    ++current_cost_calls;
    last_subscription = options.subscriptions.empty() ? std::string{} : options.subscriptions.front();
    return current_costs;
  }

  [[nodiscard]] auto previous_month_costs(const azdash::CliOptions& options) const -> std::vector<azdash::ServiceCost> override {
    ++previous_cost_calls;
    last_subscription = options.subscriptions.empty() ? std::string{} : options.subscriptions.front();
    return previous_costs;
  }

  [[nodiscard]] auto six_month_trends(const azdash::CliOptions&) const -> std::vector<azdash::MonthCost> override {
    ++trend_calls;
    return trends;
  }

  [[nodiscard]] auto waste_findings(const azdash::CliOptions&) const -> std::vector<azdash::WasteFinding> override {
    ++waste_calls;
    return waste;
  }

  [[nodiscard]] auto resolve_path(const std::string& requested_path,
                                  const std::string& default_filename) const -> std::filesystem::path override {
    ++resolve_path_calls;
    return requested_path.empty() ? std::filesystem::path(default_filename) : std::filesystem::path(requested_path);
  }

  void write_cost(const std::filesystem::path& path,
                  const azdash::AccountInfo&,
                  const std::vector<azdash::CostComparisonRow>& rows) const override {
    ++write_cost_calls;
    last_report_path = path;
    last_cost_report_row_count = rows.size();
  }

  void write_trend(const std::filesystem::path& path,
                   const azdash::AccountInfo&,
                   const std::vector<azdash::MonthCost>& rows) const override {
    ++write_trend_calls;
    last_report_path = path;
    last_trend_report_row_count = rows.size();
  }

  void write_waste(const std::filesystem::path& path,
                   const azdash::AccountInfo&,
                   const std::vector<azdash::WasteFinding>& rows) const override {
    ++write_waste_calls;
    last_report_path = path;
    last_waste_report_row_count = rows.size();
  }

  [[nodiscard]] auto list() const -> std::vector<azdash::SubscriptionAlias> override {
    ++alias_list_calls;
    return aliases;
  }

  [[nodiscard]] auto resolve(const std::string& selector) const -> std::string override {
    ++alias_resolve_calls;
    for (const auto& alias : aliases) {
      if (alias.alias == selector) {
        return alias.subscription;
      }
    }
    return selector;
  }

  void set(const std::string& alias, const std::string& subscription) const override {
    ++alias_set_calls;
    last_alias_name = alias;
    last_alias_subscription = subscription;
  }

  [[nodiscard]] auto remove(const std::string& alias) const -> bool override {
    ++alias_remove_calls;
    last_alias_name = alias;
    return remove_result;
  }

  void record(const azdash::CostSnapshot& snapshot) const override {
    ++record_calls;
    if (record_throws) {
      throw std::runtime_error("history store unavailable");
    }
    last_snapshot = snapshot;
  }

  [[nodiscard]] auto snapshots() const -> std::vector<azdash::CostSnapshot> override {
    ++snapshot_list_calls;
    return history;
  }

  mutable std::ostringstream out;
  mutable std::ostringstream err;
  std::vector<azdash::ServiceCost> current_costs;
  std::vector<azdash::ServiceCost> previous_costs;
  std::vector<azdash::MonthCost> trends;
  std::vector<azdash::WasteFinding> waste;
  mutable int account_calls{0};
  mutable int current_cost_calls{0};
  mutable int previous_cost_calls{0};
  mutable int trend_calls{0};
  mutable int waste_calls{0};
  mutable int resolve_path_calls{0};
  mutable int write_cost_calls{0};
  mutable int write_trend_calls{0};
  mutable int write_waste_calls{0};
  mutable int alias_list_calls{0};
  mutable int alias_resolve_calls{0};
  mutable int alias_set_calls{0};
  mutable int alias_remove_calls{0};
  bool remove_result{true};
  bool record_throws{false};
  std::vector<azdash::SubscriptionAlias> aliases;
  std::vector<azdash::CostSnapshot> history;
  mutable int record_calls{0};
  mutable int snapshot_list_calls{0};
  mutable azdash::CostSnapshot last_snapshot;
  mutable std::filesystem::path last_report_path;
  mutable std::string last_subscription;
  mutable std::string last_alias_name;
  mutable std::string last_alias_subscription;
  mutable std::size_t last_cost_report_row_count{0};
  mutable std::size_t last_trend_report_row_count{0};
  mutable std::size_t last_waste_report_row_count{0};
};

TEST(CliTest, ParsesCostCommandWithGlobalFlags) {
  const auto options = parse({"--subscription", "sub-1", "--output", "json", "cost"});

  EXPECT_EQ(options.command, azdash::CommandKind::Cost);
  EXPECT_EQ(options.output, azdash::OutputFormat::Json);
  ASSERT_EQ(options.subscriptions.size(), 1);
  EXPECT_EQ(options.subscriptions[0], "sub-1");
}

TEST(CliTest, ParsesAliasSubSetAndRemove) {
  const auto set_options = parse({"alias-sub", "set", "prod", "sub-id"});
  const auto remove_options = parse({"alias-sub", "remove", "prod"});

  EXPECT_EQ(set_options.command, azdash::CommandKind::AliasSub);
  EXPECT_EQ(set_options.alias_action, azdash::AliasSubAction::Set);
  EXPECT_EQ(set_options.alias_name, "prod");
  EXPECT_EQ(set_options.alias_subscription, "sub-id");
  EXPECT_EQ(remove_options.command, azdash::CommandKind::AliasSub);
  EXPECT_EQ(remove_options.alias_action, azdash::AliasSubAction::Remove);
  EXPECT_EQ(remove_options.alias_name, "prod");
}

TEST(CliTest, ParsesAliasSubShortcutAndList) {
  const auto shortcut_options = parse({"alias-sub", "prod", "sub-id"});
  const auto list_options = parse({"alias-sub"});

  EXPECT_EQ(shortcut_options.alias_action, azdash::AliasSubAction::Set);
  EXPECT_EQ(shortcut_options.alias_name, "prod");
  EXPECT_EQ(shortcut_options.alias_subscription, "sub-id");
  EXPECT_EQ(list_options.command, azdash::CommandKind::AliasSub);
  EXPECT_EQ(list_options.alias_action, azdash::AliasSubAction::List);
}

TEST(CliTest, ParsesReportWasteSelectorsAndPath) {
  const auto options = parse({"report", "waste", "compute", "network", "--path", "reports"});

  EXPECT_EQ(options.command, azdash::CommandKind::ReportWaste);
  ASSERT_EQ(options.selectors.size(), 2);
  EXPECT_EQ(options.selectors[0], "compute");
  EXPECT_EQ(options.report_path, "reports");
}

TEST(CliTest, RejectsUnknownOutputFormat) {
  EXPECT_THROW(parse({"--output", "xml", "cost"}), std::invalid_argument);
}

TEST(CliTest, ParsesHelpFlagsBeforeGlobalFlagProcessing) {
  EXPECT_EQ(parse({"--help"}).command, azdash::CommandKind::Help);
  EXPECT_EQ(parse({"-h"}).command, azdash::CommandKind::Help);
}

TEST(CliTest, ParsesHelpFlagsAfterGlobalFlagsAndCommands) {
  EXPECT_EQ(parse({"--subscription", "sub-1", "--help"}).command, azdash::CommandKind::Help);
  EXPECT_EQ(parse({"cost", "--help"}).command, azdash::CommandKind::Help);
  EXPECT_EQ(parse({"trend", "Storage", "--help"}).command, azdash::CommandKind::Help);
  EXPECT_EQ(parse({"report", "--help"}).command, azdash::CommandKind::Help);
}

TEST(CliTest, RejectsUnexpectedPositionalArgumentForCost) {
  expect_invalid_argument_message({"cost", "typo"}, "cost does not accept argument: typo");
}

TEST(CliTest, RejectsUnexpectedPositionalArgumentForReportCost) {
  expect_invalid_argument_message({"report", "cost", "typo"}, "report cost does not accept argument: typo");
}

TEST(CliTest, ParsesShortOutputFlag) {
  const auto options = parse({"-o", "csv", "cost"});

  EXPECT_EQ(options.output, azdash::OutputFormat::Csv);
  EXPECT_EQ(options.command, azdash::CommandKind::Cost);
}

TEST(CliTest, ParsesMarkdownOutputFormat) {
  EXPECT_EQ(parse({"--output", "markdown", "cost"}).output, azdash::OutputFormat::Markdown);
  EXPECT_EQ(parse({"-o", "md", "trend"}).output, azdash::OutputFormat::Markdown);
}

TEST(CliTest, ParsesHistoryCommand) {
  EXPECT_EQ(parse({"history"}).command, azdash::CommandKind::History);
  EXPECT_EQ(parse({"history", "--output", "json"}).output, azdash::OutputFormat::Json);
}

TEST(CliTest, ParsesGroupByDimension) {
  EXPECT_EQ(parse({"cost"}).group_by, azdash::GroupBy::Service);
  EXPECT_EQ(parse({"--group-by", "service", "cost"}).group_by, azdash::GroupBy::Service);
  EXPECT_EQ(parse({"--group-by", "resource-group", "cost"}).group_by, azdash::GroupBy::ResourceGroup);
  EXPECT_EQ(parse({"--group-by", "rg", "cost"}).group_by, azdash::GroupBy::ResourceGroup);
}

TEST(CliTest, RejectsUnknownGroupByDimension) {
  expect_invalid_argument_message({"--group-by", "region", "cost"},
                                  "unsupported group-by dimension: region");
}

TEST(CliTest, RejectsMissingFlagValueAtEnd) {
  EXPECT_THROW(parse({"--subscription"}), std::invalid_argument);
}

TEST(CliTest, RejectsMissingFlagValueBeforeNextFlag) {
  EXPECT_THROW(parse({"--subscription", "--output", "json", "cost"}), std::invalid_argument);
}

TEST(CliTest, RejectsUnknownFlag) {
  EXPECT_THROW(parse({"--unknown", "cost"}), std::invalid_argument);
}

TEST(CliTest, RejectsFunctionMemoryThresholdOutsidePercentRange) {
  EXPECT_THROW(parse({"--function-memory-threshold", "101", "waste"}), std::invalid_argument);
  EXPECT_THROW(parse({"--function-memory-threshold", "-1", "waste"}), std::invalid_argument);
}

TEST(CliTest, RejectsSecretsIdleDaysOutsideAllowedRange) {
  EXPECT_THROW(parse({"--secrets-idle-days", "3651", "waste"}), std::invalid_argument);
  EXPECT_THROW(parse({"--secrets-idle-days", "-1", "waste"}), std::invalid_argument);
}

TEST(CliTest, RejectsNonNumericThresholdWithCleanMessage) {
  expect_invalid_argument_message({"--function-memory-threshold", "abc", "waste"},
                                  "--function-memory-threshold must be an integer");
}

TEST(CliTest, DispatchesHelpWithoutAzureDependencies) {
  const auto options = azdash::CliOptions{.command = azdash::CommandKind::Help};
  const auto fake = FakeCliRuntime();

  EXPECT_EQ(azdash::run(options, fake.runtime()), 0);

  EXPECT_NE(fake.out.str().find("Usage:"), std::string::npos);
  EXPECT_EQ(fake.err.str(), "");
  EXPECT_EQ(fake.account_calls, 0);
  EXPECT_EQ(fake.current_cost_calls, 0);
  EXPECT_EQ(fake.previous_cost_calls, 0);
}

TEST(CliTest, DispatchesCostThroughInjectedProviders) {
  auto options = azdash::CliOptions{
      .command = azdash::CommandKind::Cost, .output = azdash::OutputFormat::Csv, .subscriptions = {"prod"}};
  auto fake = FakeCliRuntime();
  fake.aliases = {azdash::SubscriptionAlias{.alias = "prod", .subscription = "sub-id"}};
  fake.current_costs = {azdash::ServiceCost{.service = "Storage", .cost = 25.0}};
  fake.previous_costs = {azdash::ServiceCost{.service = "Storage", .cost = 10.0}};

  EXPECT_EQ(azdash::run(options, fake.runtime()), 0);

  EXPECT_EQ(fake.account_calls, 0);
  EXPECT_EQ(fake.current_cost_calls, 1);
  EXPECT_EQ(fake.previous_cost_calls, 1);
  EXPECT_EQ(fake.alias_resolve_calls, 1);
  EXPECT_EQ(fake.last_subscription, "sub-id");
  EXPECT_NE(fake.out.str().find("service,previous,current,delta,delta_percent"), std::string::npos);
  EXPECT_NE(fake.out.str().find("Storage,10.00,25.00,15.00,150.0%"), std::string::npos);
}

TEST(CliTest, AnomalyFlagsSpikeAgainstTrendBaseline) {
  auto options = azdash::CliOptions{.command = azdash::CommandKind::CostAnomaly};
  auto fake = FakeCliRuntime();
  fake.trends = {{.month = "2026-02", .total = 100.0}, {.month = "2026-03", .total = 110.0},
                 {.month = "2026-04", .total = 90.0},  {.month = "2026-05", .total = 105.0},
                 {.month = "2026-06", .total = 95.0},  {.month = "2026-07", .total = 900.0}};

  EXPECT_EQ(azdash::run(options, fake.runtime()), 0);

  EXPECT_EQ(fake.trend_calls, 1);
  EXPECT_NE(fake.out.str().find("Anomaly detected"), std::string::npos);
  EXPECT_NE(fake.out.str().find("z-score"), std::string::npos);
}

TEST(CliTest, AnomalyFailsWithoutEnoughTrendData) {
  auto options = azdash::CliOptions{.command = azdash::CommandKind::CostAnomaly};
  auto fake = FakeCliRuntime();
  fake.trends = {{.month = "2026-06", .total = 100.0}, {.month = "2026-07", .total = 100.0}};

  EXPECT_EQ(azdash::run(options, fake.runtime()), 1);

  EXPECT_NE(fake.err.str().find("Not enough data"), std::string::npos);
}

TEST(CliTest, CostRecordsSnapshotInHistory) {
  auto options = azdash::CliOptions{
      .command = azdash::CommandKind::Cost, .output = azdash::OutputFormat::Csv, .subscriptions = {"prod"}};
  auto fake = FakeCliRuntime();
  fake.aliases = {azdash::SubscriptionAlias{.alias = "prod", .subscription = "sub-id"}};
  fake.current_costs = {azdash::ServiceCost{.service = "Storage", .cost = 25.0}};
  fake.previous_costs = {azdash::ServiceCost{.service = "Storage", .cost = 10.0}};

  EXPECT_EQ(azdash::run(options, fake.runtime()), 0);

  EXPECT_EQ(fake.record_calls, 1);
  EXPECT_EQ(fake.last_snapshot.subscription, "sub-id");
  EXPECT_DOUBLE_EQ(fake.last_snapshot.total, 25.0);
  ASSERT_EQ(fake.last_snapshot.services.size(), 1);
  EXPECT_EQ(fake.last_snapshot.services[0].service, "Storage");
  EXPECT_FALSE(fake.last_snapshot.timestamp.empty());
}

TEST(CliTest, CostSucceedsWhenHistoryRecordingFails) {
  auto options = azdash::CliOptions{.command = azdash::CommandKind::Cost, .output = azdash::OutputFormat::Csv};
  auto fake = FakeCliRuntime();
  fake.record_throws = true;
  fake.current_costs = {azdash::ServiceCost{.service = "Storage", .cost = 25.0}};

  EXPECT_EQ(azdash::run(options, fake.runtime()), 0);

  EXPECT_EQ(fake.record_calls, 1);
  EXPECT_NE(fake.err.str().find("could not record cost history"), std::string::npos);
}

TEST(CliTest, HistoryCommandRendersStoredSnapshots) {
  auto options = azdash::CliOptions{.command = azdash::CommandKind::History};
  options.output = azdash::OutputFormat::Csv;
  auto fake = FakeCliRuntime();
  fake.history = {{.timestamp = "2026-07-01T10:00:00Z", .subscription = "sub-1", .total = 10.0}};

  EXPECT_EQ(azdash::run(options, fake.runtime()), 0);

  EXPECT_EQ(fake.snapshot_list_calls, 1);
  EXPECT_NE(fake.out.str().find("timestamp,subscription,total"), std::string::npos);
  EXPECT_NE(fake.out.str().find("2026-07-01T10:00:00Z,sub-1,10.00"), std::string::npos);
}

TEST(CliTest, DispatchesAliasSubSetAndListWithoutAzureCalls) {
  auto fake = FakeCliRuntime();
  auto set_options = azdash::CliOptions{.command = azdash::CommandKind::AliasSub,
                                        .alias_action = azdash::AliasSubAction::Set,
                                        .alias_name = "prod",
                                        .alias_subscription = "sub-id"};

  EXPECT_EQ(azdash::run(set_options, fake.runtime()), 0);
  EXPECT_EQ(fake.alias_set_calls, 1);
  EXPECT_EQ(fake.last_alias_name, "prod");
  EXPECT_EQ(fake.last_alias_subscription, "sub-id");
  EXPECT_EQ(fake.account_calls, 0);

  auto list_options = azdash::CliOptions{.command = azdash::CommandKind::AliasSub};
  list_options.output = azdash::OutputFormat::Csv;
  fake.aliases = {azdash::SubscriptionAlias{.alias = "prod", .subscription = "sub-id"}};
  EXPECT_EQ(azdash::run(list_options, fake.runtime()), 0);
  EXPECT_NE(fake.out.str().find("alias,subscription"), std::string::npos);
}

TEST(CliTest, DispatchesReportThroughInjectedReportWriter) {
  auto options = azdash::CliOptions{.command = azdash::CommandKind::ReportTrend, .report_path = "trend.pdf"};
  auto fake = FakeCliRuntime();
  fake.trends = {azdash::MonthCost{.month = "2026-05", .total = 42.0}};

  EXPECT_EQ(azdash::run(options, fake.runtime()), 0);

  EXPECT_EQ(fake.account_calls, 1);
  EXPECT_EQ(fake.trend_calls, 1);
  EXPECT_EQ(fake.resolve_path_calls, 1);
  EXPECT_EQ(fake.write_trend_calls, 1);
  EXPECT_EQ(fake.last_report_path, std::filesystem::path("trend.pdf"));
  EXPECT_EQ(fake.last_trend_report_row_count, 1);
  EXPECT_NE(fake.out.str().find("Trend report written to trend.pdf"), std::string::npos);
}

TEST(AzureCliDateTest, ClampsComparableDayToPreviousMonthEnd) {
  const auto leap_anchor = std::chrono::year{2024} / std::chrono::March / std::chrono::day{31};
  const auto non_leap_anchor = std::chrono::year{2023} / std::chrono::March / std::chrono::day{31};

  EXPECT_EQ(azdash::detail::civil_date_for_month(leap_anchor, -1, false), "2024-02-29");
  EXPECT_EQ(azdash::detail::civil_date_for_month(non_leap_anchor, -1, false), "2023-02-28");
}

TEST(AzureCliDateTest, UsesFirstDayForMonthStart) {
  const auto anchor = std::chrono::year{2024} / std::chrono::March / std::chrono::day{31};

  EXPECT_EQ(azdash::detail::civil_date_for_month(anchor, -1, true), "2024-02-01");
}

} // namespace
