#pragma once

#include "az_dashboard/models.hpp"

#include <span>
#include <string>
#include <vector>

namespace azdash {

/**
 * @brief Parses process arguments into workflow options.
 * @param args Command-line arguments without argv[0].
 * @return Parsed options.
 */
[[nodiscard]] auto parse_args(std::span<const std::string> args) -> CliOptions;

/**
 * @brief Runs the selected workflow and writes output to stdout.
 * @param options Parsed command-line options.
 * @return Process exit code.
 */
auto run(const CliOptions& options) -> int;

/**
 * @brief Returns help text for the command-line interface.
 * @return User-facing help text.
 */
[[nodiscard]] auto help_text() -> std::string;

/**
 * @brief Converts an output format into a stable display string.
 * @param format Output format.
 * @return String representation.
 */
[[nodiscard]] auto to_string(OutputFormat format) -> std::string;

} // namespace azdash
