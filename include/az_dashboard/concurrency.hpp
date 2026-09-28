#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <ranges>
#include <thread>
#include <type_traits>
#include <vector>

namespace azdash {

/**
 * @brief Transforms a range of items in parallel with bounded concurrency.
 *
 * Preserves the exact index order of results. If items.size() <= 1 or
 * max_concurrency <= 1, it executes synchronously with zero thread overhead.
 * If any worker throws an exception, remaining work is aborted and the first
 * exception is rethrown to the caller.
 *
 * @tparam Range Container or span conforming to std::ranges::random_access_range.
 * @tparam Func Callable accepting (const range_value_t<Range>&).
 * @param items Items to transform.
 * @param func Transformation function.
 * @param max_concurrency Maximum number of concurrent worker threads.
 * @return Vector of transformed results in the same order as items.
 */
template <typename Range, typename Func>
auto parallel_transform(const Range& items, Func&& func, std::size_t max_concurrency = 6) {
  using ItemType = std::ranges::range_value_t<Range>;
  using ResultType = std::invoke_result_t<Func, const ItemType&>;
  std::vector<ResultType> results(items.size());
  if (items.empty()) {
    return results;
  }

  if (items.size() == 1 || max_concurrency <= 1) {
    for (std::size_t i = 0; i < items.size(); ++i) {
      results[i] = func(items[i]);
    }
    return results;
  }

  const auto hardware_limit = static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency()));
  const auto num_threads = std::min({items.size(), max_concurrency, hardware_limit});

  std::atomic<std::size_t> next_index{0};
  std::exception_ptr first_exception{nullptr};
  std::mutex exception_mutex;

  std::vector<std::jthread> workers;
  workers.reserve(num_threads);
  for (std::size_t t = 0; t < num_threads; ++t) {
    workers.emplace_back([&]() {
      while (true) {
        auto idx = next_index.fetch_add(1, std::memory_order_relaxed);
        if (idx >= items.size()) {
          break;
        }
        {
          std::lock_guard lock(exception_mutex);
          if (first_exception) {
            break;
          }
        }
        try {
          results[idx] = func(items[idx]);
        } catch (...) {
          std::lock_guard lock(exception_mutex);
          if (!first_exception) {
            first_exception = std::current_exception();
          }
          break;
        }
      }
    });
  }

  workers.clear();

  if (first_exception) {
    std::rethrow_exception(first_exception);
  }
  return results;
}

} // namespace azdash
