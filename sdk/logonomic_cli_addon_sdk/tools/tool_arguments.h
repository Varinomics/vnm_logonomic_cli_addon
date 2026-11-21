#pragma once

#ifdef _WIN32
#include "logonomic_cli_addon/package_source.h"

#include <filesystem>
#include <string>
#include <vector>

namespace logonomic::cli_addon::tools {

// wmain receives the original Windows Unicode command line. Narrow main's
// argv uses the active multibyte code page and is not necessarily UTF-8.
// Keep every converted string alive while the existing parser runs.
inline int run_utf8_arguments(int argc, wchar_t** argv, int (*run)(int, char**))
{
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(path_to_utf8(std::filesystem::path(argv[i])));
    }
    std::vector<char*> pointers;
    pointers.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    return run(argc, pointers.data());
}

} // namespace logonomic::cli_addon::tools
#endif
