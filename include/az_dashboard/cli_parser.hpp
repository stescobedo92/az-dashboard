#pragma once

#include "az_dashboard/models.hpp"

#include <span>
#include <string>

namespace azdash {

/**
 * @brief Command-line argument parser that translates tokenized argv into CliOptions.
 */
class ArgumentParser {
public:
  /**
   * @brief Parses an argument sequence (excluding argv[0]) into typed CliOptions.
   * @param args Command-line arguments.
   * @return Typed workflow options.
   * @throws std::invalid_argument On unknown flags, missing arguments, or invalid values.
   */
  [[nodiscard]] auto parse(std::span<const std::string> args) const -> CliOptions;
};

/**
 * @brief Parses process arguments into workflow options.
 * @param args Command-line arguments without argv[0]. Global flags may appear
 * before commands or after selector arguments.
 * @return Parsed options.
 *
 * Throws std::invalid_argument for unknown commands or flags, missing flag
 * values, unsupported output formats, and out-of-range threshold values.
 */
[[nodiscard]] auto parse_args(std::span<const std::string> args) -> CliOptions;

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
