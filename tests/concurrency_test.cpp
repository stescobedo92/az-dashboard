#include "az_dashboard/concurrency.hpp"
#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/cli.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class FakeCommandRecordingRunner final : public azdash::ICommandRunner {
public:
  [[nodiscard]] auto run(const azdash::ProcessCommand& command,
                         const azdash::ProcessRunnerOptions& options) const -> azdash::CommandResult override {
    (void)options;
    std::lock_guard lock(mutex_);
    recorded_commands.push_back(command);
    return {0, "[]", ""};
  }

  mutable std::mutex mutex_;
  mutable std::vector<azdash::ProcessCommand> recorded_commands;
};

TEST(ParallelTransformTest, EmptyItemsReturnsEmptyVector) {
  const std::vector<int> input{};
  const auto output = azdash::parallel_transform(std::span(input), [](int val) { return val * 2; });
  EXPECT_TRUE(output.empty());
}

TEST(ParallelTransformTest, SingleItemExecutesSynchronously) {
  const std::vector<int> input{42};
  const auto output = azdash::parallel_transform(std::span(input), [](int val) { return val * 2; });
  ASSERT_EQ(output.size(), 1);
  EXPECT_EQ(output[0], 84);
}

TEST(ParallelTransformTest, MultipleItemsPreservesOrder) {
  std::vector<int> input;
  for (int i = 0; i < 50; ++i) {
    input.push_back(i);
  }

  const auto output = azdash::parallel_transform(std::span(input), [](int val) {
    // Add varying sleep to simulate work and test ordering under concurrency
    if (val % 3 == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return val * val;
  });

  ASSERT_EQ(output.size(), 50);
  for (std::size_t i = 0; i < 50; ++i) {
    EXPECT_EQ(output[i], static_cast<int>(i * i));
  }
}

TEST(ParallelTransformTest, BoundedConcurrencyLimitsActiveWorkers) {
  std::vector<int> input(20, 1);
  constexpr std::size_t max_concurrency = 4;
  std::atomic<std::size_t> active_workers{0};
  std::atomic<std::size_t> max_seen{0};

  azdash::parallel_transform(
      std::span(input),
      [&](int val) {
        (void)val;
        const auto current = active_workers.fetch_add(1, std::memory_order_relaxed) + 1;
        auto prev_max = max_seen.load(std::memory_order_relaxed);
        while (current > prev_max && !max_seen.compare_exchange_weak(prev_max, current)) {
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        active_workers.fetch_sub(1, std::memory_order_relaxed);
        return 0;
      },
      max_concurrency);

  EXPECT_LE(max_seen.load(), max_concurrency);
}

TEST(ParallelTransformTest, RethrowsWorkerException) {
  const std::vector<int> input{1, 2, 3, 4, 5};

  EXPECT_THROW(
      {
        (void)azdash::parallel_transform(std::span(input), [](int val) -> int {
          if (val == 3) {
            throw std::runtime_error("simulated failure in worker");
          }
          return val;
        });
      },
      std::runtime_error);
}

TEST(AzureCliFastQueryTest, OmitsQueryParameterByDefault) {
  auto runner = std::make_shared<FakeCommandRecordingRunner>();
  azdash::AzureCliClient client(runner);

  azdash::CliOptions options;
  options.subscriptions = {"sub-1"};
  options.fast_query = false;

  (void)client.current_month_costs(options);

  ASSERT_EQ(runner->recorded_commands.size(), 1);
  const auto& args = runner->recorded_commands[0].arguments;
  EXPECT_EQ(std::ranges::find(args, "--query"), args.end());
}

TEST(AzureCliFastQueryTest, AddsQueryParameterWhenFastQueryEnabled) {
  auto runner = std::make_shared<FakeCommandRecordingRunner>();
  azdash::AzureCliClient client(runner);

  azdash::CliOptions options;
  options.subscriptions = {"sub-1"};
  options.fast_query = true;

  (void)client.current_month_costs(options);

  ASSERT_EQ(runner->recorded_commands.size(), 1);
  const auto& args = runner->recorded_commands[0].arguments;
  const auto it = std::ranges::find(args, "--query");
  ASSERT_NE(it, args.end());
  ASSERT_NE(it + 1, args.end());
  EXPECT_TRUE((it + 1)->find("consumedService") != std::string::npos);
  EXPECT_TRUE((it + 1)->find("pretaxCost") != std::string::npos);
}

TEST(CliParserFastQueryTest, ParsesFastFlag) {
  const std::vector<std::string> args = {"--fast", "cost"};
  const auto options = azdash::parse_args(args);
  EXPECT_TRUE(options.fast_query);
  EXPECT_EQ(options.command, azdash::CommandKind::Cost);
}

} // namespace
