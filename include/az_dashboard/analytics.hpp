#pragma once

#include "az_dashboard/concepts.hpp"
#include "az_dashboard/models.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azdash {

namespace detail {

template <typename Value>
auto string_key(Value&& value) -> std::string {
  if constexpr (std::convertible_to<Value, std::string>) {
    return std::string{std::forward<Value>(value)};
  } else {
    return std::string{std::string_view{std::forward<Value>(value)}};
  }
}

inline auto normalize_selector(std::string key) -> std::string {
  std::ranges::transform(key, key.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return key;
}

} // namespace detail

/**
 * @brief Aggregates numeric values by a string key.
 * @tparam Range Input range type.
 * @tparam KeyFn Callable that returns a string-like key.
 * @tparam ValueFn Callable that returns a numeric value.
 * @param values Source values.
 * @param key_fn Function used to select the aggregate key.
 * @param value_fn Function used to select the aggregate value.
 * @return Map of key to summed value.
 */
template <InputRange Range, typename KeyFn, typename ValueFn>
requires StringKeySelector<KeyFn, std::ranges::range_value_t<Range>> &&
         DoubleValueSelector<ValueFn, std::ranges::range_value_t<Range>>
auto aggregate_by(const Range& values, KeyFn key_fn, ValueFn value_fn) -> std::map<std::string, double> {
  std::map<std::string, double> totals;
  for (const auto& value : values) {
    totals[detail::string_key(key_fn(value))] += value_fn(value);
  }
  return totals;
}

/**
 * @brief Filters rows by a user-provided selector list.
 * @tparam Range Input range type.
 * @tparam KeyFn Callable that returns a comparable string key.
 * @param values Source values.
 * @param selectors Selectors requested by the user; an empty list keeps all rows.
 * @param key_fn Function used to select the row key.
 * @return Filtered rows in original order.
 */
template <InputRange Range, typename KeyFn>
requires StringKeySelector<KeyFn, std::ranges::range_value_t<Range>>
auto filter_selected(const Range& values, const std::vector<std::string>& selectors, KeyFn key_fn)
    -> std::vector<std::ranges::range_value_t<Range>> {
  if (selectors.empty()) {
    return {std::ranges::begin(values), std::ranges::end(values)};
  }

  std::set<std::string> wanted;
  for (auto selector : selectors) {
    wanted.insert(detail::normalize_selector(std::move(selector)));
  }

  std::vector<std::ranges::range_value_t<Range>> filtered;
  for (const auto& value : values) {
    auto key = detail::normalize_selector(detail::string_key(key_fn(value)));
    if (wanted.contains(key)) {
      filtered.push_back(value);
    }
  }
  return filtered;
}

/**
 * @brief Builds a fair current-versus-previous cost comparison from selector-based inputs.
 * @tparam CurrentRange Current billing-window range type.
 * @tparam PreviousRange Previous billing-window range type.
 * @tparam KeyFn Callable that returns a string-like service key.
 * @tparam ValueFn Callable that returns a numeric cost value.
 * @param current Current billing-window values.
 * @param previous Previous billing-window values.
 * @param key_fn Function used to select the service key.
 * @param value_fn Function used to select the cost value.
 * @return Sorted comparison rows by descending current cost.
 */
template <InputRange CurrentRange, InputRange PreviousRange, typename KeyFn, typename ValueFn>
requires StringKeySelector<KeyFn, std::ranges::range_value_t<CurrentRange>> &&
         StringKeySelector<KeyFn, std::ranges::range_value_t<PreviousRange>> &&
         DoubleValueSelector<ValueFn, std::ranges::range_value_t<CurrentRange>> &&
         DoubleValueSelector<ValueFn, std::ranges::range_value_t<PreviousRange>>
auto compare_costs_by(const CurrentRange& current,
                      const PreviousRange& previous,
                      KeyFn key_fn,
                      ValueFn value_fn) -> std::vector<CostComparisonRow> {
  const auto current_by_service = aggregate_by(current, key_fn, value_fn);
  const auto previous_by_service = aggregate_by(previous, key_fn, value_fn);

  std::set<std::string> services;
  for (const auto& [service, _] : current_by_service) {
    services.insert(service);
  }
  for (const auto& [service, _] : previous_by_service) {
    services.insert(service);
  }

  std::vector<CostComparisonRow> rows;
  rows.reserve(services.size());

  for (const auto& service : services) {
    const auto previous_it = previous_by_service.find(service);
    const auto current_it = current_by_service.find(service);
    const auto previous_cost = previous_it == previous_by_service.end() ? 0.0 : previous_it->second;
    const auto current_cost = current_it == current_by_service.end() ? 0.0 : current_it->second;
    const auto delta = current_cost - previous_cost;
    const auto percent = previous_cost == 0.0 ? (current_cost == 0.0 ? 0.0 : 100.0)
                                              : (delta / previous_cost) * 100.0;
    rows.push_back({service, previous_cost, current_cost, delta, percent});
  }

  std::ranges::sort(rows, [](const auto& lhs, const auto& rhs) {
    return lhs.current > rhs.current;
  });

  return rows;
}

/**
 * @brief Builds a fair current-versus-previous service cost comparison.
 * @param current Current billing-window service costs.
 * @param previous Previous billing-window service costs.
 * @return Sorted comparison rows by descending current cost.
 */
auto compare_costs(const std::vector<ServiceCost>& current,
                   const std::vector<ServiceCost>& previous) -> std::vector<CostComparisonRow>;

/**
 * @brief Sums a service cost collection.
 * @param costs Service costs to sum.
 * @return Total cost.
 */
auto total_cost(const std::vector<ServiceCost>& costs) -> double;

/**
 * @brief Computes end-of-month projected cost based on run rate.
 * @param current_total Cost elapsed in the current month.
 * @return Projected total at the end of the month.
 */
auto compute_projection(double current_total) -> double;

/**
 * @brief Computes the arithmetic mean.
 * @param values Input values.
 * @return Mean, or 0.0 for an empty input.
 */
auto mean(const std::vector<double>& values) -> double;

/**
 * @brief Computes the sample standard deviation.
 * @param values Input values.
 * @return Sample standard deviation, or 0.0 for fewer than two values.
 */
auto sample_stddev(const std::vector<double>& values) -> double;

/**
 * @brief Scores a cost total against past month totals.
 * @param past_totals Totals for completed months.
 * @param evaluated_total Total under test, usually the projected current month.
 * @param zscore_threshold Absolute z-score treated as anomalous.
 * @return Assessment with baseline statistics. Falls back to a 20 percent
 * deviation rule when the past totals have zero variance.
 */
auto assess_cost_anomaly(const std::vector<double>& past_totals,
                         double evaluated_total,
                         double zscore_threshold = 2.0) -> CostAnomalyAssessment;

} // namespace azdash
