#pragma once

#include "az_dashboard/cli.hpp"
#include "az_dashboard/models.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Renders the visual representation of monthly cost trends for the TUI.
 */
[[nodiscard]] auto render_tui_trend_element(const std::vector<MonthCost>& trends,
                                            const std::string& currency) -> ftxui::Element;

/**
 * @brief Renders the visual representation of waste findings for the TUI.
 */
[[nodiscard]] auto render_tui_waste_element(const std::vector<WasteFinding>& findings,
                                            int selected_index) -> ftxui::Element;

/**
 * @brief Renders the cost drill-down view with visual bar charts for the TUI.
 */
[[nodiscard]] auto render_tui_cost_element(const std::vector<ServiceCost>& current_costs,
                                           int selected_index) -> ftxui::Element;

/**
 * @brief Launches the interactive FTXUI full-screen terminal dashboard.
 * @param options Parsed command options.
 * @param runtime CLI dependency bundle.
 * @return Process exit code.
 */
auto run_tui(const CliOptions& options, const CliRuntime& runtime) -> int;

} // namespace azdash
