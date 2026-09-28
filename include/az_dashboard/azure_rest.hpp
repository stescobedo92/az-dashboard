#pragma once

#include "az_dashboard/azure_cli.hpp"
#include "az_dashboard/models.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief HTTP response representation.
 */
struct HttpResponse {
  int status_code{0};
  std::string body;
};

/**
 * @brief Abstract HTTP client interface used for Azure REST API operations.
 */
class IHttpRequester {
public:
  virtual ~IHttpRequester() = default;

  [[nodiscard]] virtual auto get(const std::string& url, const std::vector<std::string>& headers) const
      -> HttpResponse = 0;
  [[nodiscard]] virtual auto post(const std::string& url, const std::string& body,
                                  const std::vector<std::string>& headers) const -> HttpResponse = 0;
};

/**
 * @brief Azure Service Principal or Managed Identity REST credentials.
 */
struct AzureRestCredentials {
  std::string tenant_id;
  std::string client_id;
  std::string client_secret;
  std::string bearer_token;

  [[nodiscard]] auto is_valid() const -> bool {
    return !bearer_token.empty() || (!tenant_id.empty() && !client_id.empty() && !client_secret.empty());
  }

  [[nodiscard]] static auto from_env() -> AzureRestCredentials;
};

/**
 * @brief Standalone direct Azure REST API client implementing IAzureClient.
 */
class AzureRestClient : public IAzureClient {
public:
  AzureRestClient(AzureRestCredentials credentials,
                  std::shared_ptr<IHttpRequester> requester,
                  std::shared_ptr<ITrendCacheStore> cache = nullptr);

  [[nodiscard]] auto account(const CliOptions& options) const -> AccountInfo override;
  [[nodiscard]] auto current_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> override;
  [[nodiscard]] auto previous_month_costs(const CliOptions& options) const -> std::vector<ServiceCost> override;
  [[nodiscard]] auto six_month_trends(const CliOptions& options) const -> std::vector<MonthCost> override;
  [[nodiscard]] auto waste_findings(const CliOptions& options) const -> std::vector<WasteFinding> override;
  [[nodiscard]] auto budgets(const CliOptions& options) const -> std::vector<BudgetInfo> override;
  [[nodiscard]] auto commitment_recommendations(const CliOptions& options) const
      -> std::vector<CommitmentRecommendation> override;

  [[nodiscard]] auto acquire_token() const -> std::string;

private:
  [[nodiscard]] auto get_target_subscriptions(const CliOptions& options) const -> std::vector<std::string>;
  [[nodiscard]] auto request_json(const std::string& url) const -> nlohmann::json;

  AzureRestCredentials credentials_;
  std::shared_ptr<IHttpRequester> requester_;
  std::shared_ptr<ITrendCacheStore> cache_;
  mutable std::string cached_token_;
};

/**
 * @brief Curl-based implementation of IHttpRequester for environments with curl installed.
 */
class CurlHttpRequester : public IHttpRequester {
public:
  explicit CurlHttpRequester(std::shared_ptr<ICommandRunner> runner = nullptr);

  [[nodiscard]] auto get(const std::string& url, const std::vector<std::string>& headers) const
      -> HttpResponse override;
  [[nodiscard]] auto post(const std::string& url, const std::string& body,
                          const std::vector<std::string>& headers) const -> HttpResponse override;

private:
  std::shared_ptr<ICommandRunner> runner_;
};

} // namespace azdash
