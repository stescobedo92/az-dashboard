#include "az_dashboard/config.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace azdash {
namespace {

[[nodiscard]] auto home_directory() -> std::filesystem::path {
  if (const auto* home = std::getenv("AZDASH_CONFIG_DIR"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home);
  }
#ifdef _WIN32
  if (const auto* userprofile = std::getenv("USERPROFILE"); userprofile != nullptr && *userprofile != '\0') {
    return std::filesystem::path(userprofile);
  }
  if (const auto* homedrive = std::getenv("HOMEDRIVE"); homedrive != nullptr && *homedrive != '\0') {
    if (const auto* homepath = std::getenv("HOMEPATH"); homepath != nullptr && *homepath != '\0') {
      return std::filesystem::path(std::string(homedrive) + homepath);
    }
  }
#else
  if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home);
  }
#endif
  return std::filesystem::current_path();
}

auto parse_output_format(const std::string& value) -> std::optional<OutputFormat> {
  if (value == "json") return OutputFormat::Json;
  if (value == "csv") return OutputFormat::Csv;
  if (value == "markdown" || value == "md") return OutputFormat::Markdown;
  if (value == "html") return OutputFormat::Html;
  if (value == "table") return OutputFormat::Table;
  return std::nullopt;
}

} // namespace

auto default_config_directory() -> std::filesystem::path {
  if (const auto* dir = std::getenv("AZDASH_CONFIG_DIR"); dir != nullptr && *dir != '\0') {
    return std::filesystem::path(dir);
  }
  return home_directory() / ".azdash";
}

auto candidate_config_paths() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> paths;
  const auto cwd = std::filesystem::current_path();
  paths.push_back(cwd / ".azdashrc");
  paths.push_back(cwd / "azdash.json");

  const auto user_dir = default_config_directory();
  paths.push_back(user_dir / "config.json");
  paths.push_back(user_dir / ".azdashrc");
  paths.push_back(home_directory() / ".azdashrc");
  return paths;
}

auto resolve_config_path(const std::string& explicit_path) -> std::filesystem::path {
  if (!explicit_path.empty()) {
    std::error_code ec;
    auto p = std::filesystem::path(explicit_path);
    if (std::filesystem::is_regular_file(p, ec)) {
      return p;
    }
    return p;
  }

  for (const auto& candidate : candidate_config_paths()) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      return candidate;
    }
  }
  return {};
}

auto parse_app_config(std::string_view json_text) -> AppConfig {
  AppConfig config;
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(json_text);
  } catch (...) {
    return config;
  }

  if (!j.is_object()) {
    return config;
  }

  auto get_str = [&](std::initializer_list<const char*> keys) -> std::optional<std::string> {
    for (const auto* key : keys) {
      if (j.contains(key) && j[key].is_string()) {
        return j[key].get<std::string>();
      }
    }
    return std::nullopt;
  };

  auto get_double = [&](std::initializer_list<const char*> keys) -> std::optional<double> {
    for (const auto* key : keys) {
      if (j.contains(key)) {
        if (j[key].is_number()) return j[key].get<double>();
        if (j[key].is_string()) {
          try {
            return std::stod(j[key].get<std::string>());
          } catch (...) {}
        }
      }
    }
    return std::nullopt;
  };

  auto get_bool = [&](std::initializer_list<const char*> keys) -> std::optional<bool> {
    for (const auto* key : keys) {
      if (j.contains(key) && j[key].is_boolean()) {
        return j[key].get<bool>();
      }
    }
    return std::nullopt;
  };

  config.default_subscription = get_str({"defaultSubscription", "default_subscription", "subscription"});
  config.default_currency = get_str({"defaultCurrency", "default_currency", "currency"});
  config.webhook_url = get_str({"webhookUrl", "webhook_url", "webhook"});
  config.management_group = get_str({"managementGroup", "management_group", "mg"});
  config.fail_if_exceeds_cost = get_double({"failIfExceeds", "fail_if_exceeds", "failIfExceedsCost", "fail_if_exceeds_cost"});
  config.min_compliance_percent = get_double({"minCompliance", "min_compliance", "minCompliancePercent", "min_compliance_percent"});
  config.fast_query = get_bool({"fast", "fastQuery", "fast_query"});
  config.use_rest = get_bool({"rest", "useRest", "use_rest"});
  config.no_cache = get_bool({"noCache", "no_cache"});

  if (auto out = get_str({"output", "outputFormat", "output_format"})) {
    config.output_format = parse_output_format(*out);
  }

  if (auto proj = get_str({"projection", "projectionMode", "projection_mode"})) {
    if (*proj == "weighted") config.projection_mode = ProjectionMode::Weighted;
    else if (*proj == "linear") config.projection_mode = ProjectionMode::Linear;
  }

  for (const auto* key : {"requiredTags", "required_tags"}) {
    if (j.contains(key) && j[key].is_array()) {
      for (const auto& item : j[key]) {
        if (item.is_string()) {
          config.required_tags.push_back(item.get<std::string>());
        }
      }
      break;
    }
  }

  return config;
}

auto load_app_config(const std::string& explicit_path) -> AppConfig {
  const auto path = resolve_config_path(explicit_path);
  if (path.empty()) {
    return {};
  }

  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return {};
  }

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return {};
  }

  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse_app_config(content);
}

void apply_config_defaults(CliOptions& options, const AppConfig& config) {
  if (options.subscriptions.empty() && config.default_subscription.has_value()) {
    options.subscriptions.push_back(*config.default_subscription);
  }
  if (options.webhook_url.empty() && config.webhook_url.has_value()) {
    options.webhook_url = *config.webhook_url;
  }
  if (options.management_group.empty() && config.management_group.has_value()) {
    options.management_group = *config.management_group;
  }
  if (!options.fail_if_exceeds_cost.has_value() && config.fail_if_exceeds_cost.has_value()) {
    options.fail_if_exceeds_cost = config.fail_if_exceeds_cost;
  }
  if (options.min_compliance_percent == 0.0 && config.min_compliance_percent.has_value()) {
    options.min_compliance_percent = *config.min_compliance_percent;
  }
  if (options.required_tags.empty() && !config.required_tags.empty()) {
    options.required_tags = config.required_tags;
  }
  if (!options.fast_query && config.fast_query.has_value()) {
    options.fast_query = *config.fast_query;
  }
  if (!options.use_rest && config.use_rest.has_value()) {
    options.use_rest = *config.use_rest;
  }
  if (!options.no_cache && config.no_cache.has_value()) {
    options.no_cache = *config.no_cache;
  }
  if (options.output == OutputFormat::Table && config.output_format.has_value()) {
    options.output = *config.output_format;
  }
  if (options.projection_mode == ProjectionMode::Linear && config.projection_mode.has_value()) {
    options.projection_mode = *config.projection_mode;
  }
}

} // namespace azdash
