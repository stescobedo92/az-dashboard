#pragma once

#include <concepts>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>

namespace azdash {

/**
 * @brief Concept for selector results that can be materialized as string keys.
 */
template <typename T>
concept StringKey = std::convertible_to<T, std::string> || std::convertible_to<T, std::string_view>;

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
 * @brief Concept for functions that produce a string-like key from a value.
 */
template <typename Fn, typename T>
concept StringKeySelector =
    std::invocable<Fn, const T&> && StringKey<std::remove_cvref_t<std::invoke_result_t<Fn, const T&>>>;

/**
 * @brief Concept for functions that produce a double value from a value.
 */
template <typename Fn, typename T>
concept DoubleValueSelector =
    std::invocable<Fn, const T&> && std::convertible_to<std::invoke_result_t<Fn, const T&>, double>;

/**
 * @brief Concept for ranges whose values can be processed by analytics helpers.
 */
template <typename Range>
concept InputRange = std::ranges::input_range<Range> && std::ranges::input_range<const Range>;

} // namespace azdash
