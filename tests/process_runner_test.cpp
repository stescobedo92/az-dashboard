#include "az_dashboard/windows_command.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

TEST(WindowsCommandTest, QuotesArgumentsThatContainSpaces) {
  EXPECT_EQ(azdash::detail::quote_windows_argument("simple"), "simple");
  EXPECT_EQ(azdash::detail::quote_windows_argument("has space"), "\"has space\"");
  EXPECT_EQ(azdash::detail::quote_windows_argument(""), "\"\"");
}

TEST(WindowsCommandTest, WrapsBatchScriptInvocationWithOuterQuotes) {
  const auto line = azdash::detail::build_cmd_wrapped_command_line("cmd.exe", "C:\\Az\\az.cmd",
                                                                   {"account", "show", "-o", "json"});

  EXPECT_EQ(line, "cmd.exe /d /s /c \"\"C:\\Az\\az.cmd\" \"account\" \"show\" \"-o\" \"json\"\"");
}

TEST(WindowsCommandTest, QuotesBatchPathWithSpaces) {
  const auto line = azdash::detail::build_cmd_wrapped_command_line(
      "cmd.exe", "C:\\Program Files\\Azure\\az.cmd", {"cost"});

  EXPECT_EQ(line, "cmd.exe /d /s /c \"\"C:\\Program Files\\Azure\\az.cmd\" \"cost\"\"");
}

TEST(WindowsCommandTest, KeepsInjectionLookingArgumentsQuotedForCmd) {
  const auto line = azdash::detail::build_cmd_wrapped_command_line("cmd.exe", "C:\\Az\\az.cmd",
                                                                   {"--subscription", "sub&rm", "-o", "json"});

  EXPECT_EQ(line, "cmd.exe /d /s /c \"\"C:\\Az\\az.cmd\" \"--subscription\" \"sub&rm\" \"-o\" \"json\"\"");
}

} // namespace
