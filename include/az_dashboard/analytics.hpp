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
#include <vector>

namespace azdash {

/**
 * @brief Aggregates numeric values by a string key.
 * @tparam Range Input range type.
 * @tparam KeyFn Callable that returns a string key.
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
    totals[key_fn(value)] += value_fn(value);
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
    std::ranges::transform(selector, selector.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    wanted.insert(selector);
  }

  std::vector<std::ranges::range_value_t<Range>> filtered;
  for (const auto& value : values) {
    auto key = key_fn(value);
    std::ranges::transform(key, key.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (wanted.contains(key)) {
      filtered.push_back(value);
    }
  }
  return filtered;
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

} // namespace azdash
