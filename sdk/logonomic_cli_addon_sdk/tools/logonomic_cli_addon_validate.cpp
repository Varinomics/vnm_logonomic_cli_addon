// logonomic_cli_addon_validate <package-dir> [--platform <id>] [--core-generation <n>]...
//
// Validates one package directory against the package contract and prints
// either the generation digest and declared capabilities or the reason code
// and detail of the first failure. Exit code 0 on success, 1 on failure,
// 2 on usage errors.

#include "tool_arguments.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/package_validator.h"

#include <cstdio>
#include <charconv>
#include <string>

namespace {

namespace addon = logonomic::cli_addon;

int usage()
{
    std::fprintf(stderr,
        "usage: logonomic_cli_addon_validate <package-dir> [--platform <id>] [--core-generation <n>]...\n");
    return 2;
}

} // namespace

int run(int argc, char** argv)
{
    std::string directory;
    addon::Validation_options options;
    bool generations_given = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--platform" && i + 1 < argc) {
            options.platform_id = argv[++i];
        }
        else
        if (argument == "--core-generation" && i + 1 < argc) {
            const std::string value = argv[++i];
            std::uint32_t generation = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), generation);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                !addon::is_valid_version(generation)) {
                return usage();
            }
            if (!generations_given) {
                options.host_core_protocol_generations.clear();
                generations_given = true;
            }
            options.host_core_protocol_generations.push_back(static_cast<std::uint16_t>(generation));
        }
        else
        if (!argument.empty() && argument[0] == '-') {
            return usage();
        }
        else
        if (directory.empty()) {
            directory = argument;
        }
        else {
            return usage();
        }
    }
    if (directory.empty() || !addon::is_valid_platform_id(options.platform_id)) {
        return usage();
    }

    addon::Directory_package_source source(addon::path_from_utf8(directory));
    addon::Package_validator validator(options);
    addon::Contract_failure failure;
    const std::optional<addon::Validated_package> package = validator.validate(source, &failure);
    if (!package.has_value()) {
        std::fprintf(stdout, "FAIL %s %s\n", failure.reason_code.c_str(), failure.detail.c_str());
        return 1;
    }

    std::string capabilities;
    for (const addon::Capability_pair& pair : package->declared_capabilities()) {
        capabilities += " " + pair.capability_id + "@" + std::to_string(pair.version);
    }
    std::fprintf(stdout, "OK %s %s bytes=%llu declared:%s\n",
        package->bootstrap.package_id.c_str(),
        package->generation_digest_hex.c_str(),
        (unsigned long long)package->total_bytes,
        capabilities.c_str());
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    return logonomic::cli_addon::tools::run_utf8_arguments(argc, argv, run);
}
#else
int main(int argc, char** argv)
{
    return run(argc, argv);
}
#endif
