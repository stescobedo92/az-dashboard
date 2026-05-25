#include "az_dashboard/cli.hpp"
#include "az_dashboard/render.hpp"

#include <span>
#include <iostream>
#include <exception>
#include <string>
#include <vector>

auto main(int argc, char** argv) -> int {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (auto index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }

  try {
    const auto options = azdash::parse_args(std::span<const std::string>(args.data(), args.size()));
    return azdash::run(options);
  } catch (const std::exception& error) {
    azdash::render_error(error.what(), std::cerr);
    azdash::render_help_screen(std::cerr);
    return 1;
  }
}
