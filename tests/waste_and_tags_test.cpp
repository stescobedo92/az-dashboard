#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/cli.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

class FakeScriptRunner final : public azdash::ICommandRunner {
public:
  FakeScriptRunner() = default;
  explicit FakeScriptRunner(std::vector<azdash::CommandResult> results) : results_(std::move(results)) {}

  [[nodiscard]] auto run(const azdash::ProcessCommand& command,
                         const azdash::ProcessRunnerOptions& options) const -> azdash::CommandResult override {
    (void)options;
    executed_commands.push_back(command);
    if (results_.empty()) {
      return {0, "[]", ""};
    }
    if (next_ >= results_.size()) {
      return {1, "[]", "unexpected command"};
    }
    return results_[next_++];
  }

  std::vector<azdash::CommandResult> results_;
  mutable std::size_t next_{0};
  mutable std::vector<azdash::ProcessCommand> executed_commands;
};

TEST(MultiTagFilterTest, FiltersByExactAndOrValues) {
  const std::string usage_json = R"([
    {"properties":{"consumedService":"VM-Prod","pretaxCost":10.0,"tags":{"env":"prod","team":"core"}}},
    {"properties":{"consumedService":"VM-Dev","pretaxCost":5.0,"tags":{"env":"dev","team":"core"}}},
    {"properties":{"consumedService":"VM-Stage","pretaxCost":7.0,"tags":{"env":"stage","team":"billing"}}},
    {"properties":{"consumedService":"VM-Untagged","pretaxCost":3.0,"tags":{}}}
  ])";

  auto runner = std::make_shared<FakeScriptRunner>(std::vector<azdash::CommandResult>{{0, usage_json, ""}});
  azdash::AzureCliClient client(runner);

  // Match env=prod,stage
  azdash::CliOptions options;
  options.subscriptions = {"sub-1"};
  options.filter_tags = {"env=prod,stage"};

  const auto costs = client.current_month_costs(options);
  ASSERT_EQ(costs.size(), 2);
  EXPECT_DOUBLE_EQ(costs[0].cost + costs[1].cost, 17.0);
}

TEST(MultiTagFilterTest, FiltersByNotEqualAndTagExistence) {
  const std::string usage_json = R"([
    {"properties":{"consumedService":"VM-Prod","pretaxCost":10.0,"tags":{"env":"prod","owner":"alice"}}},
    {"properties":{"consumedService":"VM-Dev","pretaxCost":5.0,"tags":{"env":"dev"}}},
    {"properties":{"consumedService":"VM-NoEnv","pretaxCost":2.0,"tags":{"owner":"bob"}}}
  ])";

  // Test: key!=val (env!=prod)
  {
    auto runner = std::make_shared<FakeScriptRunner>(std::vector<azdash::CommandResult>{{0, usage_json, ""}});
    azdash::AzureCliClient client(runner);

    azdash::CliOptions options;
    options.subscriptions = {"sub-1"};
    options.filter_tags = {"env!=prod"};

    const auto costs = client.current_month_costs(options);
    ASSERT_EQ(costs.size(), 2); // VM-Dev (env=dev) and VM-NoEnv (env not present)
  }

  // Test: tag exists ("owner")
  {
    auto runner = std::make_shared<FakeScriptRunner>(std::vector<azdash::CommandResult>{{0, usage_json, ""}});
    azdash::AzureCliClient client(runner);

    azdash::CliOptions options;
    options.subscriptions = {"sub-1"};
    options.filter_tags = {"owner"};

    const auto costs = client.current_month_costs(options);
    ASSERT_EQ(costs.size(), 2); // VM-Prod and VM-NoEnv have owner tag
  }

  // Test: tag absent ("!owner") - FinOps untagged compliance check
  {
    auto runner = std::make_shared<FakeScriptRunner>(std::vector<azdash::CommandResult>{{0, usage_json, ""}});
    azdash::AzureCliClient client(runner);

    azdash::CliOptions options;
    options.subscriptions = {"sub-1"};
    options.filter_tags = {"!owner"};

    const auto costs = client.current_month_costs(options);
    ASSERT_EQ(costs.size(), 1);
    EXPECT_EQ(costs[0].service, "VM-Dev");
  }
}

TEST(WasteHeuristicsTest, DetectsNewWasteResourceTypes) {
  const std::string resources_json = R"([
    {
      "id": "/subscriptions/sub/resourceGroups/rg/providers/Microsoft.Compute/disks/prem-disk",
      "name": "prem-disk",
      "type": "Microsoft.Compute/disks",
      "location": "eastus",
      "managedBy": null,
      "sku": {"name": "Premium_LRS", "tier": "Premium"}
    },
    {
      "id": "/subscriptions/sub/resourceGroups/rg/providers/Microsoft.Network/networkSecurityGroups/nsg-unused",
      "name": "nsg-unused",
      "type": "Microsoft.Network/networkSecurityGroups",
      "location": "eastus",
      "properties": {"subnets": [], "networkInterfaces": []}
    },
    {
      "id": "/subscriptions/sub/resourceGroups/rg/providers/Microsoft.Network/routeTables/rt-unused",
      "name": "rt-unused",
      "type": "Microsoft.Network/routeTables",
      "location": "eastus",
      "properties": {"subnets": []}
    },
    {
      "id": "/subscriptions/sub/resourceGroups/rg/providers/Microsoft.Network/natGateways/nat-unused",
      "name": "nat-unused",
      "type": "Microsoft.Network/natGateways",
      "location": "eastus",
      "properties": {"subnets": []}
    },
    {
      "id": "/subscriptions/sub/resourceGroups/rg/providers/Microsoft.Web/serverfarms/asp-empty",
      "name": "asp-empty",
      "type": "Microsoft.Web/serverfarms",
      "location": "eastus",
      "properties": {"numberOfSites": 0}
    }
  ])";

  auto runner = std::make_shared<FakeScriptRunner>(std::vector<azdash::CommandResult>{
      {0, "[]", ""},             // advisor
      {0, resources_json, ""},   // resources
      {0, "[]", ""},             // vms
  });

  azdash::AzureCliClient client(runner);
  azdash::CliOptions options;
  options.subscriptions = {"sub-1"};

  const auto findings = client.waste_findings(options);
  ASSERT_EQ(findings.size(), 5);

  EXPECT_EQ(findings[0].name, "prem-disk");
  EXPECT_TRUE(findings[0].recommendation.find("Premium/Ultra SSD disk") != std::string::npos);

  EXPECT_EQ(findings[1].name, "nsg-unused");
  EXPECT_TRUE(findings[1].recommendation.find("Network Security Group is not associated") != std::string::npos);

  EXPECT_EQ(findings[2].name, "rt-unused");
  EXPECT_TRUE(findings[2].recommendation.find("Route table is not associated") != std::string::npos);

  EXPECT_EQ(findings[3].name, "nat-unused");
  EXPECT_DOUBLE_EQ(findings[3].estimated_monthly_savings, 32.40);

  EXPECT_EQ(findings[4].name, "asp-empty");
  EXPECT_TRUE(findings[4].recommendation.find("App Service Plan has 0 hosted apps") != std::string::npos);
}

class FakeWasteRuntime final : public azdash::ICliAccountProvider,
                              public azdash::ICliCostProvider,
                              public azdash::ICliTrendProvider,
                              public azdash::ICliWasteProvider,
                              public azdash::ICliReportWriter,
                              public azdash::ICliSubscriptionAliasStore,
                              public azdash::ICliCostHistoryStore {
public:
  [[nodiscard]] auto runtime() const -> azdash::CliRuntime {
    return azdash::CliRuntime{out, err, *this, *this, *this, *this, *this, *this, *this, nullptr, &in, &runner};
  }

  [[nodiscard]] auto account(const azdash::CliOptions&) const -> azdash::AccountInfo override {
    return {"sub", "sub-name", "tenant", "user"};
  }
  [[nodiscard]] auto current_month_costs(const azdash::CliOptions&) const -> std::vector<azdash::ServiceCost> override { return {}; }
  [[nodiscard]] auto previous_month_costs(const azdash::CliOptions&) const -> std::vector<azdash::ServiceCost> override { return {}; }
  [[nodiscard]] auto six_month_trends(const azdash::CliOptions&) const -> std::vector<azdash::MonthCost> override { return {}; }
  [[nodiscard]] auto waste_findings(const azdash::CliOptions&) const -> std::vector<azdash::WasteFinding> override {
    return {
        {"network", "/subscriptions/s/resourceGroups/rg/providers/Microsoft.Network/networkSecurityGroups/nsg1",
         "Microsoft.Network/networkSecurityGroups", "nsg1", "eastus", "Orphaned NSG", 0.0},
        {"network", "/subscriptions/s/resourceGroups/rg/providers/Microsoft.Network/routeTables/rt1",
         "Microsoft.Network/routeTables", "rt1", "eastus", "Orphaned RT", 0.0},
        {"network", "/subscriptions/s/resourceGroups/rg/providers/Microsoft.Network/natGateways/nat1",
         "Microsoft.Network/natGateways", "nat1", "eastus", "Idle NAT", 32.40},
        {"appservice", "/subscriptions/s/resourceGroups/rg/providers/Microsoft.Web/serverfarms/asp1",
         "Microsoft.Web/serverfarms", "asp1", "eastus", "Empty ASP", 0.0},
    };
  }
  [[nodiscard]] auto resolve_path(const std::string& p, const std::string&) const -> std::filesystem::path override { return p; }
  void write_cost(const std::filesystem::path&, const azdash::AccountInfo&, const std::vector<azdash::CostComparisonRow>&) const override {}
  void write_trend(const std::filesystem::path&, const azdash::AccountInfo&, const std::vector<azdash::MonthCost>&) const override {}
  void write_waste(const std::filesystem::path&, const azdash::AccountInfo&, const std::vector<azdash::WasteFinding>&) const override {}
  [[nodiscard]] auto list() const -> std::vector<azdash::SubscriptionAlias> override { return {}; }
  [[nodiscard]] auto resolve(const std::string& s) const -> std::string override { return s; }
  void set(const std::string&, const std::string&) const override {}
  [[nodiscard]] auto remove(const std::string&) const -> bool override { return true; }
  void record(const azdash::CostSnapshot&) const override {}
  [[nodiscard]] auto snapshots() const -> std::vector<azdash::CostSnapshot> override { return {}; }

  mutable std::ostringstream out;
  mutable std::ostringstream err;
  mutable std::istringstream in;
  mutable FakeScriptRunner runner;
};

TEST(CliWasteRemediationTest, GeneratesDeleteCommandsForNewResourceTypes) {
  const auto script_path = std::filesystem::temp_directory_path() / "azdash-test-remediation" / "cleanup.sh";
  std::filesystem::remove_all(script_path.parent_path());

  FakeWasteRuntime fake_runtime;
  azdash::CliOptions options;
  options.command = azdash::CommandKind::Waste;
  options.remediation_path = script_path.string();

  const auto exit_code = azdash::run(options, fake_runtime.runtime());
  EXPECT_EQ(exit_code, 0);

  ASSERT_TRUE(std::filesystem::exists(script_path));
  std::string content;
  {
    std::ifstream file(script_path);
    content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  }

  EXPECT_TRUE(content.find("az network nsg delete --ids \"/subscriptions/s/resourceGroups/rg/providers/Microsoft.Network/networkSecurityGroups/nsg1\"") != std::string::npos);
  EXPECT_TRUE(content.find("az network route-table delete --ids \"/subscriptions/s/resourceGroups/rg/providers/Microsoft.Network/routeTables/rt1\"") != std::string::npos);
  EXPECT_TRUE(content.find("az network nat gateway delete --ids \"/subscriptions/s/resourceGroups/rg/providers/Microsoft.Network/natGateways/nat1\"") != std::string::npos);
  EXPECT_TRUE(content.find("# az appservice plan delete --ids \"/subscriptions/s/resourceGroups/rg/providers/Microsoft.Web/serverfarms/asp1\" --yes") != std::string::npos);

  std::filesystem::remove_all(script_path.parent_path());
}

TEST(CliWasteRemediationTest, DryRunSkipsFileCreation) {
  const auto script_path = std::filesystem::temp_directory_path() / "azdash-test-dryrun" / "cleanup.sh";
  std::filesystem::remove_all(script_path.parent_path());

  FakeWasteRuntime fake_runtime;
  azdash::CliOptions options;
  options.command = azdash::CommandKind::Waste;
  options.remediation_path = script_path.string();
  options.dry_run = true;

  const auto exit_code = azdash::run(options, fake_runtime.runtime());
  EXPECT_EQ(exit_code, 0);

  // File should NOT be created in dry run mode
  EXPECT_FALSE(std::filesystem::exists(script_path));
  EXPECT_TRUE(fake_runtime.out.str().find("Dry Run Complete") != std::string::npos);
}

TEST(CliWasteRemediationTest, InteractiveRemediationAppliesConfirmedAndSkipsRejected) {
  FakeWasteRuntime fake_runtime;
  // 4 findings: "y" for 1st, "n" for 2nd, "yes" for 3rd, "" for 4th
  fake_runtime.in.str("y\nn\nyes\n\n");

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Waste;
  options.interactive = true;

  const auto exit_code = azdash::run(options, fake_runtime.runtime());
  EXPECT_EQ(exit_code, 0);

  const auto output = fake_runtime.out.str();
  EXPECT_TRUE(output.find("[Interactive Remediation Mode]") != std::string::npos);
  EXPECT_TRUE(output.find("Interactive Remediation Complete") != std::string::npos);
  EXPECT_TRUE(output.find("2 of 4 resources remediated") != std::string::npos);
  EXPECT_EQ(fake_runtime.runner.executed_commands.size(), 2u);
}

TEST(CliWasteRemediationTest, InteractiveRemediationRespectsDryRun) {
  FakeWasteRuntime fake_runtime;
  fake_runtime.in.str("y\ny\ny\ny\n");

  azdash::CliOptions options;
  options.command = azdash::CommandKind::Waste;
  options.interactive = true;
  options.dry_run = true;

  const auto exit_code = azdash::run(options, fake_runtime.runtime());
  EXPECT_EQ(exit_code, 0);

  const auto output = fake_runtime.out.str();
  EXPECT_TRUE(output.find("[Dry-run] Would execute:") != std::string::npos);
  EXPECT_EQ(fake_runtime.runner.executed_commands.size(), 0u);
}

TEST(CliParserDryRunTest, ParsesDryRunFlag) {
  const std::vector<std::string> args = {"--dry-run", "waste"};
  const auto options = azdash::parse_args(args);
  EXPECT_TRUE(options.dry_run);
  EXPECT_EQ(options.command, azdash::CommandKind::Waste);
}

TEST(CliParserInteractiveTest, ParsesInteractiveAndProjectionFlags) {
  const std::vector<std::string> args = {"-i", "--projection", "weighted", "waste"};
  const auto options = azdash::parse_args(args);
  EXPECT_TRUE(options.interactive);
  EXPECT_EQ(options.projection_mode, azdash::ProjectionMode::Weighted);
  EXPECT_EQ(options.command, azdash::CommandKind::Waste);

  const std::vector<std::string> args_linear = {"--projection", "linear", "cost"};
  const auto options_linear = azdash::parse_args(args_linear);
  EXPECT_EQ(options_linear.projection_mode, azdash::ProjectionMode::Linear);

  const std::vector<std::string> args_bad = {"--projection", "invalid", "cost"};
  EXPECT_THROW((void)azdash::parse_args(args_bad), std::invalid_argument);
}

} // namespace
