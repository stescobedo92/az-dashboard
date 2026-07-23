#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace azdash::detail {

/**
 * @brief Quotes a single argument for the Windows CommandLineToArgvW parser.
 *
 * Follows the standard MSVC backslash/quote rules so that spaces and embedded
 * quotes survive round-tripping into a child process argv.
 */
[[nodiscard]] auto quote_windows_argument(std::string_view argument) -> std::string;

/**
 * @brief Builds a `cmd.exe /d /s /c "..."` command line that runs a batch or
 * cmd script (such as the Azure CLI `az.cmd`) with typed arguments.
 *
 * The whole inner command is wrapped in one extra pair of quotes that `/s`
 * strips, and every token is individually quoted so cmd.exe treats separators
 * and redirection characters as literal data. `%VAR%` expansion cannot be fully
 * neutralized on a cmd.exe command line; realistic Azure identifiers such as
 * GUIDs, ISO dates, and service names do not contain `%VAR%` sequences.
 * @param interpreter Command interpreter token, usually the resolved cmd.exe path.
 * @param script_path Full path to the batch/cmd script to execute.
 * @param arguments Arguments forwarded to the script.
 * @return A command line suitable for CreateProcess.
 */
[[nodiscard]] auto build_cmd_wrapped_command_line(std::string_view interpreter,
                                                  std::string_view script_path,
                                                  const std::vector<std::string>& arguments) -> std::string;

} // namespace azdash::detail
