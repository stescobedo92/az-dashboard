#pragma once

#include "az_dashboard/models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azdash {

/**
 * @brief Application configuration loaded from .azdashrc or config.json.
 */
struct AppConfig {
  std::optional<std::string> default_subscription;
  std::optional<std::string> default_currency;
  std::optional<std::string> webhook_url;
  std::optional<std::string> management_group;
  std::optional<double> fail_if_exceeds_cost;
  std::optional<double> min_compliance_percent;
  std::vector<std::string> required_tags;
  std::optional<OutputFormat> output_format;
  std::optional<GroupBy> group_by;
  std::optional<ProjectionMode> projection_mode;
  std::optional<bool> fast_query;
  std::optional<bool> use_rest;
  std::optional<bool> no_cache;
};

/**
 * @brief Returns default directory for user-level azdash configuration (~/.azdash).
 */
[[nodiscard]] auto default_config_directory() -> std::filesystem::path;

/**
 * @brief Returns candidate paths to configuration files in discovery order:
 * 1. ./.azdashrc
 * 2. ./azdash.json
 * 3. ~/.azdash/config.json
 * 4. ~/.azdashrc
 */
[[nodiscard]] auto candidate_config_paths() -> std::vector<std::filesystem::path>;

/**
 * @brief Resolves active configuration path, or empty path if none exists.
 */
[[nodiscard]] auto resolve_config_path(const std::string& explicit_path = "") -> std::filesystem::path;

/**
 * @brief Parses JSON string into AppConfig.
 */
[[nodiscard]] auto parse_app_config(std::string_view json_text) -> AppConfig;

/**
 * @brief Loads AppConfig from the resolved configuration file path.
 */
[[nodiscard]] auto load_app_config(const std::string& explicit_path = "") -> AppConfig;

/**
 * @brief Applies AppConfig defaults to CliOptions for fields not explicitly set.
 */
void apply_config_defaults(CliOptions& options, const AppConfig& config);

} // namespace azdash
