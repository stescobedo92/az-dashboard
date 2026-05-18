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
void render_costs(const std::vector<CostComparisonRow>& rows, OutputFormat format, std::ostream& out);

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
 * @brief Renders version information.
 * @param out Output stream.
 */
void render_version(std::ostream& out);

} // namespace azdash
