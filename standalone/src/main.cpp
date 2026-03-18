#include "atlas/version.hpp"

#include <cxxopts.hpp>
#include <format>
#include <iostream>
#include <string>
#include <unordered_map>

int main(int argc, char** argv) {
    cxxopts::Options options(ATLAS_NAME, "A modern C++ greeter application");

    // clang-format off
    options.add_options()
        ("h,help",    "Print usage")
        ("v,version", "Print version");
    // clang-format on

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help();
        return 0;
    }

    if (result.count("version")) {
        std::cout << std::format("{} v{}\n", ATLAS_NAME, ATLAS_VERSION);
        return 0;
    }

    return 0;
}
