#pragma once

#include "az_dashboard/models.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Resolves the report output path.
 * @param requested Path requested by the user; empty uses the current directory.
 * @param default_name Default file name.
 * @return Final path.
 */
[[nodiscard]] auto resolve_report_path(const std::string& requested,
                                       const std::string& default_name) -> std::filesystem::path;

/**
 * @brief Writes a PDF cost report.
 * @param path Destination PDF path.
 * @param account Azure account information.
 * @param rows Cost comparison rows.
 */
void write_cost_pdf(const std::filesystem::path& path,
                    const AccountInfo& account,
                    const std::vector<CostComparisonRow>& rows);

/**
 * @brief Writes a PDF trend report.
 * @param path Destination PDF path.
 * @param account Azure account information.
 * @param rows Monthly trend rows.
 */
void write_trend_pdf(const std::filesystem::path& path,
                     const AccountInfo& account,
                     const std::vector<MonthCost>& rows);

/**
 * @brief Writes a PDF waste report.
 * @param path Destination PDF path.
 * @param account Azure account information.
 * @param rows Waste findings.
 */
void write_waste_pdf(const std::filesystem::path& path,
                     const AccountInfo& account,
                     const std::vector<WasteFinding>& rows);

} // namespace azdash
