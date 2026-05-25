#include "az_dashboard/azure_cli.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeRunner final : public azdash::ICommandRunner {
public:
  explicit FakeRunner(std::vector<azdash::CommandResult> results) : results_(std::move(results)) {}

  [[nodiscard]] auto run(const azdash::ProcessCommand& command, const azdash::ProcessRunnerOptions& options) const
      -> azdash::CommandResult override {
    commands.push_back(command);
    options_seen.push_back(options);
    if (next_ >= results_.size()) {
      return {1, "", "unexpected command"};
    }
    return results_[next_++];
  }

  mutable std::vector<azdash::ProcessCommand> commands;
  mutable std::vector<azdash::ProcessRunnerOptions> options_seen;

private:
  std::vector<azdash::CommandResult> results_;
  mutable std::size_t next_{0};
};

auto make_client(std::shared_ptr<FakeRunner> runner) -> azdash::AzureCliClient {
  return azdash::AzureCliClient(std::move(runner));
}

TEST(AzureCliTest, RejectsNullRunner) {
  EXPECT_THROW(azdash::AzureCliClient(nullptr), std::invalid_argument);
}

TEST(AzureCliTest, AccountParsesJson) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{
      {0, R"({"id":"sub-1","name":"Production","tenantId":"tenant-1","user":{"name":"user@example.com"}})", ""},
  });

  const auto account = make_client(runner).account({});

  EXPECT_EQ(account.subscription_id, "sub-1");
  EXPECT_EQ(account.subscription_name, "Production");
  EXPECT_EQ(account.tenant_id, "tenant-1");
  EXPECT_EQ(account.user_name, "user@example.com");
}

TEST(AzureCliTest, AccountCommandIncludesSubscriptionAndTenant) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{{0, "{}", ""}});
  azdash::CliOptions options;
  options.subscription = "sub-1";
  options.tenant = "tenant-1";

  (void)make_client(runner).account(options);

  ASSERT_EQ(runner->commands.size(), 1);
  EXPECT_EQ(runner->commands[0].executable, "az");
  const std::vector<std::string> expected_arguments{
      "account", "show", "--subscription", "sub-1", "--tenant", "tenant-1", "-o", "json"};
  EXPECT_EQ(runner->commands[0].arguments, expected_arguments);
  ASSERT_EQ(runner->options_seen.size(), 1);
  EXPECT_EQ(runner->options_seen[0].timeout, std::chrono::seconds{30});
}

TEST(AzureCliTest, CostCommandIncludesSubscriptionAndTenant) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{{0, "[]", ""}});
  azdash::CliOptions options;
  options.subscription = "sub-1";
  options.tenant = "tenant-1";

  (void)make_client(runner).current_month_costs(options);

  ASSERT_EQ(runner->commands.size(), 1);
  EXPECT_EQ(runner->commands[0].executable, "az");
  ASSERT_GE(runner->commands[0].arguments.size(), 10);
  EXPECT_EQ(runner->commands[0].arguments[0], "consumption");
  EXPECT_EQ(runner->commands[0].arguments[1], "usage");
  EXPECT_EQ(runner->commands[0].arguments[2], "list");
  EXPECT_NE(std::ranges::find(runner->commands[0].arguments, "--subscription"), runner->commands[0].arguments.end());
  EXPECT_NE(std::ranges::find(runner->commands[0].arguments, "sub-1"), runner->commands[0].arguments.end());
  EXPECT_NE(std::ranges::find(runner->commands[0].arguments, "--tenant"), runner->commands[0].arguments.end());
  EXPECT_NE(std::ranges::find(runner->commands[0].arguments, "tenant-1"), runner->commands[0].arguments.end());
}

TEST(AzureCliTest, UnsafeLookingSubscriptionIsPassedAsTypedArgument) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{{0, "{}", ""}});
  azdash::CliOptions options;
  options.subscription = "sub;rm";

  EXPECT_NO_THROW((void)make_client(runner).account(options));
  ASSERT_EQ(runner->commands.size(), 1);
  const std::vector<std::string> expected_arguments{"account", "show", "--subscription", "sub;rm", "-o", "json"};
  EXPECT_EQ(runner->commands[0].arguments, expected_arguments);
}

TEST(AzureCliTest, UnsafeLookingTenantIsPassedAsTypedArgument) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{{0, "{}", ""}});
  azdash::CliOptions options;
  options.tenant = "tenant$(whoami)";

  EXPECT_NO_THROW((void)make_client(runner).account(options));
  ASSERT_EQ(runner->commands.size(), 1);
  const std::vector<std::string> expected_arguments{"account", "show", "--tenant", "tenant$(whoami)", "-o", "json"};
  EXPECT_EQ(runner->commands[0].arguments, expected_arguments);
}

TEST(AzureCliTest, AccountThrowsOnNonzeroExit) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{{2, "", "boom"}});

  EXPECT_THROW((void)make_client(runner).account({}), std::runtime_error);
}

TEST(AzureCliTest, NonzeroExitRedactsSensitiveArgumentsAndIncludesStderr) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{
      {2, "stdout mentions sub-secret", "stderr mentions tenant-secret and sub-secret"},
  });
  azdash::CliOptions options;
  options.subscription = "sub-secret";
  options.tenant = "tenant-secret";

  try {
    (void)make_client(runner).account(options);
    FAIL() << "expected runtime_error";
  } catch (const std::runtime_error& error) {
    const auto message = std::string{error.what()};
    EXPECT_EQ(message.find("sub-secret"), std::string::npos);
    EXPECT_EQ(message.find("tenant-secret"), std::string::npos);
    EXPECT_NE(message.find("--subscription <redacted>"), std::string::npos);
    EXPECT_NE(message.find("--tenant <redacted>"), std::string::npos);
    EXPECT_NE(message.find("stderr:"), std::string::npos);
    EXPECT_NE(message.find("<redacted>"), std::string::npos);
  }
}

TEST(AzureCliTest, AccountThrowsOnInvalidJson) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{{0, "not json", ""}});

  EXPECT_THROW((void)make_client(runner).account({}), std::runtime_error);
}

TEST(AzureCliTest, CurrentMonthCostsAggregatesDuplicateServices) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{
      {0, R"([{"properties":{"consumedService":"Virtual Machines","pretaxCost":2.5}},{"properties":{"consumedService":"Virtual Machines","pretaxCost":"3.5"}},{"properties":{"meterCategory":"Storage","pretaxCost":1.0}}])", ""},
  });

  const auto costs = make_client(runner).current_month_costs({});

  ASSERT_EQ(costs.size(), 2);
  const auto virtual_machines = std::ranges::find(costs, "Virtual Machines", &azdash::ServiceCost::service);
  const auto storage = std::ranges::find(costs, "Storage", &azdash::ServiceCost::service);
  ASSERT_NE(virtual_machines, costs.end());
  ASSERT_NE(storage, costs.end());
  EXPECT_DOUBLE_EQ(virtual_machines->cost, 6.0);
  EXPECT_DOUBLE_EQ(storage->cost, 1.0);
}

TEST(AzureCliTest, WasteFindingsDetectsAdvisorAndResourceHeuristics) {
  auto runner = std::make_shared<FakeRunner>(std::vector<azdash::CommandResult>{
      {0, R"([{"properties":{"impactedField":"Microsoft.Compute/virtualMachines","impactedValue":"vm-rightsize","resourceMetadata":"/subscriptions/sub/resourceGroups/rg/providers/Microsoft.Compute/virtualMachines/vm-rightsize","shortDescription":"Resize VM","annualSavingsAmount":120.0}}])", ""},
      {0, R"([{"type":"Microsoft.Compute/disks","name":"disk-1","id":"disk-id","location":"westus","managedBy":null},{"type":"Microsoft.Network/publicIPAddresses","name":"pip-1","id":"pip-id","location":"westus","properties":{"ipConfiguration":null}}])", ""},
      {0, R"([{"id":"vm-id","name":"vm-1","location":"westus","powerState":"VM deallocated"}])", ""},
  });

  const auto findings = make_client(runner).waste_findings({});

  ASSERT_EQ(findings.size(), 4);
  EXPECT_EQ(findings[0].check, "advisor");
  EXPECT_DOUBLE_EQ(findings[0].estimated_monthly_savings, 10.0);
  EXPECT_EQ(findings[1].name, "disk-1");
  EXPECT_EQ(findings[2].name, "pip-1");
  EXPECT_EQ(findings[3].name, "vm-1");
}

#ifndef _WIN32
TEST(ShellCommandRunnerTest, CapturesStdoutAndStderrSeparately) {
  const azdash::ShellCommandRunner runner;

  const auto result = runner.run({"/bin/sh", {"-c", "printf out; printf err >&2"}});

  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.stdout_text, "out");
  EXPECT_EQ(result.stderr_text, "err");
  EXPECT_FALSE(result.timed_out);
}

TEST(ShellCommandRunnerTest, TimesOutLongRunningCommand) {
  const azdash::ShellCommandRunner runner;

  const auto result =
      runner.run({"/bin/sh", {"-c", "sleep 1"}}, azdash::ProcessRunnerOptions{std::chrono::milliseconds{50}});

  EXPECT_TRUE(result.timed_out);
  EXPECT_NE(result.exit_code, 0);
}

TEST(ShellCommandRunnerTest, TimesOutDescendantHoldingPipeOpen) {
  const azdash::ShellCommandRunner runner;

  const auto start = std::chrono::steady_clock::now();
  const auto result = runner.run({"/bin/sh", {"-c", "sleep 2 & exit 0"}},
                                 azdash::ProcessRunnerOptions{std::chrono::milliseconds{50}});
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(result.timed_out);
  EXPECT_LT(elapsed, std::chrono::seconds{1});
}
#endif

} // namespace
