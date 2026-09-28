#include "az_dashboard/cache.hpp"
#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/cli.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace {

class FakeCountingRunner final : public azdash::ICommandRunner {
public:
  explicit FakeCountingRunner(std::string default_json) : default_json_(std::move(default_json)) {}

  [[nodiscard]] auto run(const azdash::ProcessCommand& command,
                         const azdash::ProcessRunnerOptions& options) const -> azdash::CommandResult override {
    (void)command;
    (void)options;
    ++call_count;
    return {0, default_json_, ""};
  }

  std::string default_json_;
  mutable std::size_t call_count{0};
};

TEST(LocalTrendCacheStoreTest, ReturnsNulloptWhenMissingOrEmpty) {
  const auto temp_path = std::filesystem::temp_directory_path() / "azdash-test-cache-missing" / "cache.json";
  std::filesystem::remove_all(temp_path.parent_path());

  const azdash::LocalTrendCacheStore store(temp_path);
  EXPECT_EQ(store.get("sub-1", "2026-05", "service"), std::nullopt);
}

TEST(LocalTrendCacheStoreTest, PutsAndGetsDataWithCurrency) {
  const auto temp_path = std::filesystem::temp_directory_path() / "azdash-test-cache-crud" / "cache.json";
  std::filesystem::remove_all(temp_path.parent_path());

  const azdash::LocalTrendCacheStore store(temp_path);
  const std::vector<azdash::ServiceCost> services = {
      {"Virtual Machines", 125.50, {}, "EUR"},
      {"Storage Accounts", 45.20, {}, "EUR"},
  };

  store.put("sub-prod", "2026-04", "service", services);

  const auto cached = store.get("sub-prod", "2026-04", "service");
  ASSERT_TRUE(cached.has_value());
  ASSERT_EQ(cached->size(), 2);
  EXPECT_EQ((*cached)[0].service, "Virtual Machines");
  EXPECT_DOUBLE_EQ((*cached)[0].cost, 125.50);
  EXPECT_EQ((*cached)[0].currency, "EUR");
  EXPECT_EQ((*cached)[1].service, "Storage Accounts");
  EXPECT_DOUBLE_EQ((*cached)[1].cost, 45.20);
  EXPECT_EQ((*cached)[1].currency, "EUR");

  // Querying different key returns nullopt
  EXPECT_EQ(store.get("sub-other", "2026-04", "service"), std::nullopt);
  EXPECT_EQ(store.get("sub-prod", "2026-05", "service"), std::nullopt);
  EXPECT_EQ(store.get("sub-prod", "2026-04", "rg"), std::nullopt);
}

TEST(LocalTrendCacheStoreTest, OverwritesDataForSameKey) {
  const auto temp_path = std::filesystem::temp_directory_path() / "azdash-test-cache-overwrite" / "cache.json";
  std::filesystem::remove_all(temp_path.parent_path());

  const azdash::LocalTrendCacheStore store(temp_path);
  store.put("sub-1", "2026-01", "service", {{"Service A", 10.0, {}, "USD"}});
  store.put("sub-1", "2026-01", "service", {{"Service B", 25.0, {}, "EUR"}});

  const auto cached = store.get("sub-1", "2026-01", "service");
  ASSERT_TRUE(cached.has_value());
  ASSERT_EQ(cached->size(), 1);
  EXPECT_EQ((*cached)[0].service, "Service B");
  EXPECT_DOUBLE_EQ((*cached)[0].cost, 25.0);
  EXPECT_EQ((*cached)[0].currency, "EUR");
}

TEST(LocalTrendCacheStoreTest, GracefullyHandlesCorruptFile) {
  const auto temp_path = std::filesystem::temp_directory_path() / "azdash-test-cache-corrupt" / "cache.json";
  std::filesystem::remove_all(temp_path.parent_path());
  std::filesystem::create_directories(temp_path.parent_path());
  {
    std::ofstream out(temp_path);
    out << "{ corrupt json content ...";
  }

  const azdash::LocalTrendCacheStore store(temp_path);
  EXPECT_EQ(store.get("sub-1", "2026-01", "service"), std::nullopt);
}

TEST(AzureCliCacheTest, CachesClosedMonthsAndSkipsRunnerOnSubsequentCalls) {
  const auto temp_path = std::filesystem::temp_directory_path() / "azdash-test-cli-cache" / "cache.json";
  std::filesystem::remove_all(temp_path.parent_path());

  const std::string usage_json = R"([
    {"instanceName": "vm1", "consumedService": "Microsoft.Compute", "pretaxCost": "100.00", "billingCurrency": "USD"}
  ])";

  auto runner = std::make_shared<FakeCountingRunner>(usage_json);
  auto cache = std::make_shared<azdash::LocalTrendCacheStore>(temp_path);
  azdash::AzureCliClient client(runner, cache);

  azdash::CliOptions options;
  options.subscriptions = {"sub-1"};

  // First run: cache is empty, so all 6 months query the runner
  const auto trends1 = client.six_month_trends(options);
  EXPECT_EQ(runner->call_count, 6);
  EXPECT_EQ(trends1.size(), 6);

  // Second run: 5 closed months should hit cache, only offset 0 (current month) queries runner
  runner->call_count = 0;
  const auto trends2 = client.six_month_trends(options);
  EXPECT_EQ(runner->call_count, 1);
  EXPECT_EQ(trends2.size(), 6);
  EXPECT_DOUBLE_EQ(trends2.front().total, 100.0);
}

TEST(AzureCliCacheTest, NoCacheFlagBypassesCache) {
  const auto temp_path = std::filesystem::temp_directory_path() / "azdash-test-cli-nocache" / "cache.json";
  std::filesystem::remove_all(temp_path.parent_path());

  const std::string usage_json = R"([
    {"instanceName": "vm1", "consumedService": "Microsoft.Compute", "pretaxCost": "50.00", "billingCurrency": "USD"}
  ])";

  auto runner = std::make_shared<FakeCountingRunner>(usage_json);
  auto cache = std::make_shared<azdash::LocalTrendCacheStore>(temp_path);
  azdash::AzureCliClient client(runner, cache);

  azdash::CliOptions options;
  options.subscriptions = {"sub-1"};

  // First run populates the cache
  (void)client.six_month_trends(options);
  EXPECT_EQ(runner->call_count, 6);

  // Second run with no_cache = true should query all 6 months again
  runner->call_count = 0;
  options.no_cache = true;
  (void)client.six_month_trends(options);
  EXPECT_EQ(runner->call_count, 6);
}

TEST(CliParserTest, ParsesNoCacheFlag) {
  const std::vector<std::string> args = {"--no-cache", "trend"};
  const auto options = azdash::parse_args(args);
  EXPECT_TRUE(options.no_cache);
  EXPECT_EQ(options.command, azdash::CommandKind::Trend);
}

} // namespace
