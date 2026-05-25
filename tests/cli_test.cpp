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
                             public azdash::ICliReportWriter {
public:
  [[nodiscard]] auto runtime() const -> azdash::CliRuntime {
    return azdash::CliRuntime{out, err, *this, *this, *this, *this, *this};
  }

  [[nodiscard]] auto account(const azdash::CliOptions&) const -> azdash::AccountInfo override {
    ++account_calls;
    return {.subscription_id = "sub-id", .subscription_name = "Subscription", .tenant_id = "tenant", .user_name = "user"};
  }

  [[nodiscard]] auto current_month_costs(const azdash::CliOptions&) const -> std::vector<azdash::ServiceCost> override {
    ++current_cost_calls;
    return current_costs;
  }

  [[nodiscard]] auto previous_month_costs(const azdash::CliOptions&) const -> std::vector<azdash::ServiceCost> override {
    ++previous_cost_calls;
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
  mutable std::filesystem::path last_report_path;
  mutable std::size_t last_cost_report_row_count{0};
  mutable std::size_t last_trend_report_row_count{0};
  mutable std::size_t last_waste_report_row_count{0};
};

TEST(CliTest, ParsesCostCommandWithGlobalFlags) {
  const auto options = parse({"--subscription", "sub-1", "--output", "json", "cost"});

  EXPECT_EQ(options.command, azdash::CommandKind::Cost);
  EXPECT_EQ(options.output, azdash::OutputFormat::Json);
  EXPECT_EQ(options.subscription, "sub-1");
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
  auto options = azdash::CliOptions{.command = azdash::CommandKind::Cost, .output = azdash::OutputFormat::Csv};
  auto fake = FakeCliRuntime();
  fake.current_costs = {azdash::ServiceCost{.service = "Storage", .cost = 25.0}};
  fake.previous_costs = {azdash::ServiceCost{.service = "Storage", .cost = 10.0}};

  EXPECT_EQ(azdash::run(options, fake.runtime()), 0);

  EXPECT_EQ(fake.account_calls, 0);
  EXPECT_EQ(fake.current_cost_calls, 1);
  EXPECT_EQ(fake.previous_cost_calls, 1);
  EXPECT_NE(fake.out.str().find("service,previous,current,delta,delta_percent"), std::string::npos);
  EXPECT_NE(fake.out.str().find("Storage,10.00,25.00,15.00,150.0%"), std::string::npos);
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
  EXPECT_NE(fake.out.str().find("Report written to trend.pdf"), std::string::npos);
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
