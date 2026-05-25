#pragma once

#include "az_dashboard/models.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Resolves the report output path.
 * @param requested Path requested by the user; empty or DEFAULT uses the current directory.
 * @param default_name Default file name used when requested is a directory-like path.
 * @return Absolute normalized path under the current working directory.
 *
 * Paths with an extension are treated as explicit files; directory-like paths
 * are joined with default_name. Parent traversal and outside-cwd targets throw
 * std::invalid_argument.
 */
[[nodiscard]] auto resolve_report_path(const std::string& requested,
                                       const std::string& default_name) -> std::filesystem::path;

/**
 * @brief Writes a PDF cost report.
 * @param path Destination PDF path.
 * @param account Azure account information.
 * @param rows Cost comparison rows.
 *
 * Parent directories are created as needed and PDF text delimiters are escaped.
 */
void write_cost_pdf(const std::filesystem::path& path,
                    const AccountInfo& account,
                    const std::vector<CostComparisonRow>& rows);

/**
 * @brief Writes a PDF trend report.
 * @param path Destination PDF path.
 * @param account Azure account information.
 * @param rows Monthly trend rows.
 *
 * Parent directories are created as needed and PDF text delimiters are escaped.
 */
void write_trend_pdf(const std::filesystem::path& path,
                     const AccountInfo& account,
                     const std::vector<MonthCost>& rows);

/**
 * @brief Writes a PDF waste report.
 * @param path Destination PDF path.
 * @param account Azure account information.
 * @param rows Waste findings.
 *
 * Parent directories are created as needed and PDF text delimiters are escaped.
 */
void write_waste_pdf(const std::filesystem::path& path,
                     const AccountInfo& account,
                     const std::vector<WasteFinding>& rows);

} // namespace azdash
