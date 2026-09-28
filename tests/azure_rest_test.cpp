#include "az_dashboard/azure_rest.hpp"
#include "az_dashboard/cli.hpp"
#include "az_dashboard/models.hpp"

#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

class FakeHttpRequester final : public azdash::IHttpRequester {
public:
  void set_response(const std::string& url_substring, int status_code, const std::string& body) {
    responses_[url_substring] = {status_code, body};
  }

  [[nodiscard]] auto get(const std::string& url, const std::vector<std::string>& headers) const
      -> azdash::HttpResponse override {
    std::lock_guard lock(mutex_);
    get_calls_.emplace_back(url, headers);
    for (const auto& [pattern, resp] : responses_) {
      if (url.find(pattern) != std::string::npos) {
        return resp;
      }
    }
    return {404, R"({"error": "not found"})"};
  }

  [[nodiscard]] auto post(const std::string& url, const std::string& body,
                          const std::vector<std::string>& headers) const -> azdash::HttpResponse override {
    std::lock_guard lock(mutex_);
    post_calls_.emplace_back(url, body, headers);
    for (const auto& [pattern, resp] : responses_) {
      if (url.find(pattern) != std::string::npos) {
        return resp;
      }
    }
    return {200, R"({"access_token": "fake-jwt-token-12345", "expires_in": 3600})"};
  }

  mutable std::mutex mutex_;
  mutable std::vector<std::pair<std::string, std::vector<std::string>>> get_calls_;
  mutable std::vector<std::tuple<std::string, std::string, std::vector<std::string>>> post_calls_;

private:
  std::map<std::string, azdash::HttpResponse> responses_;
};

class FakeRunner final : public azdash::ICommandRunner {
public:
  explicit FakeRunner(azdash::CommandResult result) : result_(std::move(result)) {}

  [[nodiscard]] auto run(const azdash::ProcessCommand& command, const azdash::ProcessRunnerOptions&) const
      -> azdash::CommandResult override {
    last_command = command;
    return result_;
  }

  mutable azdash::ProcessCommand last_command;

private:
  azdash::CommandResult result_;
};

TEST(AzureRestTest, CredentialsValidation) {
  azdash::AzureRestCredentials empty_creds;
  EXPECT_FALSE(empty_creds.is_valid());

  azdash::AzureRestCredentials token_creds;
  token_creds.bearer_token = "eyJhbGciOi...";
  EXPECT_TRUE(token_creds.is_valid());

  azdash::AzureRestCredentials sp_creds;
  sp_creds.tenant_id = "tenant-1";
  sp_creds.client_id = "client-1";
  sp_creds.client_secret = "secret-1";
  EXPECT_TRUE(sp_creds.is_valid());
}

TEST(AzureRestTest, AcquiresAndCachesOAuth2Token) {
  auto requester = std::make_shared<FakeHttpRequester>();
  azdash::AzureRestCredentials creds{
      .tenant_id = "tenant-abc",
      .client_id = "client-xyz",
      .client_secret = "secret-123",
  };

  azdash::AzureRestClient client(creds, requester);

  const auto token1 = client.acquire_token();
  EXPECT_EQ(token1, "fake-jwt-token-12345");
  EXPECT_EQ(requester->post_calls_.size(), 1);

  // Second call should return cached token without posting again
  const auto token2 = client.acquire_token();
  EXPECT_EQ(token2, "fake-jwt-token-12345");
  EXPECT_EQ(requester->post_calls_.size(), 1);
}

TEST(AzureRestTest, UsesDirectBearerTokenWithoutPost) {
  auto requester = std::make_shared<FakeHttpRequester>();
  azdash::AzureRestCredentials creds{
      .bearer_token = "direct-bearer-token",
  };

  azdash::AzureRestClient client(creds, requester);

  const auto token = client.acquire_token();
  EXPECT_EQ(token, "direct-bearer-token");
  EXPECT_EQ(requester->post_calls_.size(), 0);
}

TEST(AzureRestTest, FetchesAccountInfoViaRest) {
  auto requester = std::make_shared<FakeHttpRequester>();
  requester->set_response("subscriptions/sub-100", 200, R"({
    "id": "/subscriptions/sub-100",
    "subscriptionId": "sub-100",
    "displayName": "Enterprise Production",
    "tenantId": "tenant-corp"
  })");

  azdash::AzureRestCredentials creds{.bearer_token = "valid-token"};
  azdash::AzureRestClient client(creds, requester);

  azdash::CliOptions options;
  options.subscriptions = {"sub-100"};

  const auto account = client.account(options);
  EXPECT_EQ(account.subscription_id, "sub-100");
  EXPECT_EQ(account.subscription_name, "Enterprise Production");
  EXPECT_EQ(account.tenant_id, "tenant-corp");
}

TEST(AzureRestTest, CurrentMonthCostsParsesUsageDetails) {
  auto requester = std::make_shared<FakeHttpRequester>();
  requester->set_response("usageDetails", 200, R"({
    "value": [
      {
        "properties": {
          "consumedService": "Microsoft.Compute",
          "pretaxCost": 85.50,
          "billingCurrency": "USD"
        }
      },
      {
        "properties": {
          "consumedService": "Microsoft.Storage",
          "pretaxCost": 14.50,
          "billingCurrency": "USD"
        }
      }
    ]
  })");

  azdash::AzureRestCredentials creds{.bearer_token = "valid-token"};
  azdash::AzureRestClient client(creds, requester);

  azdash::CliOptions options;
  options.subscriptions = {"sub-100"};

  auto costs = client.current_month_costs(options);
  ASSERT_EQ(costs.size(), 2);

  double total = 0.0;
  for (const auto& c : costs) {
    total += c.cost;
  }
  EXPECT_DOUBLE_EQ(total, 100.0);
}

TEST(AzureRestTest, BudgetsParsesConsumptionBudgets) {
  auto requester = std::make_shared<FakeHttpRequester>();
  requester->set_response("budgets", 200, R"({
    "value": [
      {
        "name": "CloudOpsBudget",
        "properties": {
          "amount": 3000.0,
          "currentSpend": {"amount": 2100.0, "unit": "USD"},
          "timeGrain": "Monthly"
        }
      }
    ]
  })");

  azdash::AzureRestCredentials creds{.bearer_token = "valid-token"};
  azdash::AzureRestClient client(creds, requester);

  azdash::CliOptions options;
  options.subscriptions = {"sub-100"};

  auto budgets = client.budgets(options);
  ASSERT_EQ(budgets.size(), 1);
  EXPECT_EQ(budgets[0].name, "CloudOpsBudget");
  EXPECT_DOUBLE_EQ(budgets[0].amount, 3000.0);
  EXPECT_DOUBLE_EQ(budgets[0].current_spend, 2100.0);
}

TEST(AzureRestTest, CommitmentRecommendationsParsesReservations) {
  auto requester = std::make_shared<FakeHttpRequester>();
  requester->set_response("reservationRecommendations", 200, R"({
    "value": [
      {
        "id": "rec-1",
        "properties": {
          "skuName": "Standard_D4s_v5",
          "term": "P1Y",
          "netSavings": 110.0,
          "currency": "USD"
        }
      }
    ]
  })");

  azdash::AzureRestCredentials creds{.bearer_token = "valid-token"};
  azdash::AzureRestClient client(creds, requester);

  azdash::CliOptions options;
  options.subscriptions = {"sub-100"};

  auto recs = client.commitment_recommendations(options);
  ASSERT_EQ(recs.size(), 1);
  EXPECT_EQ(recs[0].sku, "Standard_D4s_v5");
  EXPECT_EQ(recs[0].term, "1 Year");
  EXPECT_DOUBLE_EQ(recs[0].estimated_monthly_savings, 110.0);
}

TEST(AzureRestTest, CurlHttpRequesterExecutesAndParsesOutput) {
  const std::string mock_stdout = "{\"ok\":true}\n200";
  auto runner = std::make_shared<FakeRunner>(azdash::CommandResult{0, mock_stdout, ""});
  azdash::CurlHttpRequester requester(runner);

  auto resp = requester.get("https://example.com/api", {"Authorization: Bearer test"});
  EXPECT_EQ(resp.status_code, 200);
  EXPECT_EQ(resp.body, "{\"ok\":true}");
  EXPECT_EQ(runner->last_command.executable, "curl");
}

TEST(AzureRestTest, CliArgumentParserParsesRestFlag) {
  const std::vector<std::string> args = {"--rest", "cost"};
  auto options = azdash::parse_args(args);

  EXPECT_TRUE(options.use_rest);
  EXPECT_EQ(options.command, azdash::CommandKind::Cost);
}

} // namespace
