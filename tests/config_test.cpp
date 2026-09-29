#include "az_dashboard/config.hpp"
#include "az_dashboard/cli_parser.hpp"
#include "az_dashboard/models.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace {

TEST(ConfigTest, ParsesEmptyAndInvalidJsonGracefully) {
  const auto empty = azdash::parse_app_config("");
  EXPECT_FALSE(empty.default_subscription.has_value());
  EXPECT_FALSE(empty.webhook_url.has_value());
  EXPECT_TRUE(empty.required_tags.empty());

  const auto invalid = azdash::parse_app_config("{invalid json]");
  EXPECT_FALSE(invalid.default_subscription.has_value());

  const auto non_object = azdash::parse_app_config("[\"hello\"]");
  EXPECT_FALSE(non_object.default_subscription.has_value());
}

TEST(ConfigTest, ParsesFullConfigJsonSuccessfully) {
  const std::string json = R"({
    "defaultSubscription": "prod-sub",
    "defaultCurrency": "EUR",
    "webhookUrl": "https://hooks.slack.com/services/T00/B00/X00",
    "managementGroup": "mg-enterprise",
    "failIfExceeds": 1500.50,
    "minCompliance": 92.5,
    "requiredTags": ["Environment", "Owner", "CostCenter", "Application"],
    "output": "html",
    "fast": true,
    "rest": true,
    "noCache": true,
    "projection": "weighted"
  })";

  const auto config = azdash::parse_app_config(json);

  ASSERT_TRUE(config.default_subscription.has_value());
  EXPECT_EQ(*config.default_subscription, "prod-sub");

  ASSERT_TRUE(config.default_currency.has_value());
  EXPECT_EQ(*config.default_currency, "EUR");

  ASSERT_TRUE(config.webhook_url.has_value());
  EXPECT_EQ(*config.webhook_url, "https://hooks.slack.com/services/T00/B00/X00");

  ASSERT_TRUE(config.management_group.has_value());
  EXPECT_EQ(*config.management_group, "mg-enterprise");

  ASSERT_TRUE(config.fail_if_exceeds_cost.has_value());
  EXPECT_DOUBLE_EQ(*config.fail_if_exceeds_cost, 1500.50);

  ASSERT_TRUE(config.min_compliance_percent.has_value());
  EXPECT_DOUBLE_EQ(*config.min_compliance_percent, 92.5);

  ASSERT_EQ(config.required_tags.size(), 4u);
  EXPECT_EQ(config.required_tags[0], "Environment");
  EXPECT_EQ(config.required_tags[3], "Application");

  ASSERT_TRUE(config.output_format.has_value());
  EXPECT_EQ(*config.output_format, azdash::OutputFormat::Html);

  ASSERT_TRUE(config.fast_query.has_value());
  EXPECT_TRUE(*config.fast_query);

  ASSERT_TRUE(config.use_rest.has_value());
  EXPECT_TRUE(*config.use_rest);

  ASSERT_TRUE(config.no_cache.has_value());
  EXPECT_TRUE(*config.no_cache);

  ASSERT_TRUE(config.projection_mode.has_value());
  EXPECT_EQ(*config.projection_mode, azdash::ProjectionMode::Weighted);
}

TEST(ConfigTest, ApplyDefaultsFillsUnsetValuesWithoutOverridingExplicitFlags) {
  azdash::AppConfig config;
  config.default_subscription = "sub-config";
  config.webhook_url = "https://hooks.slack.com/config";
  config.management_group = "mg-config";
  config.fail_if_exceeds_cost = 2000.0;
  config.min_compliance_percent = 85.0;
  config.required_tags = {"Environment", "Owner"};
  config.output_format = azdash::OutputFormat::Json;
  config.fast_query = true;
  config.use_rest = true;
  config.no_cache = true;
  config.projection_mode = azdash::ProjectionMode::Weighted;

  // Case 1: All unset -> takes all defaults from config
  {
    azdash::CliOptions options;
    azdash::apply_config_defaults(options, config);

    ASSERT_EQ(options.subscriptions.size(), 1u);
    EXPECT_EQ(options.subscriptions.front(), "sub-config");
    EXPECT_EQ(options.webhook_url, "https://hooks.slack.com/config");
    EXPECT_EQ(options.management_group, "mg-config");
    ASSERT_TRUE(options.fail_if_exceeds_cost.has_value());
    EXPECT_DOUBLE_EQ(*options.fail_if_exceeds_cost, 2000.0);
    EXPECT_DOUBLE_EQ(options.min_compliance_percent, 85.0);
    ASSERT_EQ(options.required_tags.size(), 2u);
    EXPECT_EQ(options.output, azdash::OutputFormat::Json);
    EXPECT_TRUE(options.fast_query);
    EXPECT_TRUE(options.use_rest);
    EXPECT_TRUE(options.no_cache);
    EXPECT_EQ(options.projection_mode, azdash::ProjectionMode::Weighted);
  }

  // Case 2: Explicit CLI flags set -> config defaults do NOT override them
  {
    azdash::CliOptions options;
    options.subscriptions.push_back("sub-cli");
    options.webhook_url = "https://hooks.slack.com/cli";
    options.management_group = "mg-cli";
    options.fail_if_exceeds_cost = 500.0;
    options.min_compliance_percent = 99.0;
    options.required_tags = {"CostCenter"};
    options.output = azdash::OutputFormat::Csv;
    options.fast_query = false; // explicitly false
    options.use_rest = false;
    options.no_cache = false;
    options.projection_mode = azdash::ProjectionMode::Linear;

    azdash::apply_config_defaults(options, config);

    EXPECT_EQ(options.subscriptions.front(), "sub-cli");
    EXPECT_EQ(options.webhook_url, "https://hooks.slack.com/cli");
    EXPECT_EQ(options.management_group, "mg-cli");
    EXPECT_DOUBLE_EQ(*options.fail_if_exceeds_cost, 500.0);
    EXPECT_DOUBLE_EQ(options.min_compliance_percent, 99.0);
    ASSERT_EQ(options.required_tags.size(), 1u);
    EXPECT_EQ(options.required_tags.front(), "CostCenter");
    EXPECT_EQ(options.output, azdash::OutputFormat::Csv);
  }
}

TEST(ConfigTest, LoadsConfigFromDiskFile) {
  const auto temp_config_path = std::filesystem::temp_directory_path() / "test_azdash_config.json";
  {
    std::ofstream out(temp_config_path);
    out << R"({ "subscription": "disk-sub-123", "minCompliance": 88.0 })";
  }

  const auto config = azdash::load_app_config(temp_config_path.string());
  ASSERT_TRUE(config.default_subscription.has_value());
  EXPECT_EQ(*config.default_subscription, "disk-sub-123");
  ASSERT_TRUE(config.min_compliance_percent.has_value());
  EXPECT_DOUBLE_EQ(*config.min_compliance_percent, 88.0);

  std::error_code ec;
  std::filesystem::remove(temp_config_path, ec);
}

TEST(ConfigTest, CliParserParsesConfigFlag) {
  const std::vector<std::string> args = {"cost", "--config", "/path/to/my_config.json"};
  const auto options = azdash::parse_args(args);
  EXPECT_EQ(options.config_path, "/path/to/my_config.json");
}

} // namespace
