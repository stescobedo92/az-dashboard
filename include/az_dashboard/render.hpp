#pragma once

#include "az_dashboard/models.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Renders cost comparison rows to an output stream.
 * @param rows Cost comparison rows.
 * @param format Desired output format.
 * @param out Output stream.
 */
void render_costs(const std::vector<CostComparisonRow>& rows, double projected_total, OutputFormat format, std::ostream& out);

/**
 * @brief Renders trend rows to an output stream.
 * @param rows Month trend rows.
 * @param format Desired output format.
 * @param out Output stream.
 */
void render_trends(const std::vector<MonthCost>& rows, OutputFormat format, std::ostream& out);

/**
 * @brief Renders waste findings to an output stream.
 * @param rows Waste findings.
 * @param format Desired output format.
 * @param out Output stream.
 */
void render_waste(const std::vector<WasteFinding>& rows, OutputFormat format, std::ostream& out);

/**
 * @brief Renders subscription aliases to an output stream.
 * @param rows Subscription alias rows.
 * @param format Desired output format.
 * @param out Output stream.
 */
void render_subscription_aliases(const std::vector<SubscriptionAlias>& rows, OutputFormat format, std::ostream& out);

/**
 * @brief Renders locally recorded cost snapshots to an output stream.
 * @param rows Cost snapshots in append order.
 * @param format Desired output format.
 * @param out Output stream.
 */
void render_cost_history(const std::vector<CostSnapshot>& rows, OutputFormat format, std::ostream& out);

/**
 * @brief Renders interactive help.
 * @param out Output stream.
 */
void render_help_screen(std::ostream& out);

/**
 * @brief Renders version information.
 * @param out Output stream.
 */
void render_version(std::ostream& out);

/**
 * @brief Renders update guidance.
 * @param out Output stream.
 */
void render_update_guidance(std::ostream& out);

/**
 * @brief Renders a success message.
 * @param title Short message title.
 * @param detail Message detail.
 * @param out Output stream.
 */
void render_success(const std::string& title, const std::string& detail, std::ostream& out);

/**
 * @brief Renders an error message.
 * @param message User-facing error text.
 * @param out Output stream.
 */
void render_error(const std::string& message, std::ostream& out);

} // namespace azdash
