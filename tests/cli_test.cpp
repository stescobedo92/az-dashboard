#include "az_dashboard/cli.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

auto parse(std::initializer_list<std::string> values) -> azdash::CliOptions {
  const std::vector<std::string> args(values);
  return azdash::parse_args(std::span<const std::string>(args.data(), args.size()));
}

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

} // namespace
