#pragma once

#include <concepts>
#include <ranges>
#include <string>
#include <string_view>

namespace azdash {

/**
 * @brief Concept for records exposing a readable name field.
 */
template <typename T>
concept NamedRecord = requires(const T& value) {
  { value.name } -> std::convertible_to<std::string_view>;
};

/**
 * @brief Concept for records exposing a numeric cost field.
 */
template <typename T>
concept CostRecord = requires(const T& value) {
  { value.cost } -> std::convertible_to<double>;
};

/**
 * @brief Concept for functions that produce a string key from a value.
 */
template <typename Fn, typename T>
concept StringKeySelector = requires(Fn fn, const T& value) {
  { fn(value) } -> std::convertible_to<std::string>;
};

/**
 * @brief Concept for functions that produce a double value from a value.
 */
template <typename Fn, typename T>
concept DoubleValueSelector = requires(Fn fn, const T& value) {
  { fn(value) } -> std::convertible_to<double>;
};

/**
 * @brief Concept for ranges whose values can be processed by analytics helpers.
 */
template <typename Range>
concept InputRange = std::ranges::input_range<Range>;

} // namespace azdash
