// logonomic_cli_addon_host_simulator <package-dir> --script <file>
//     [--transcript <file>] [--platform <id>] [--nonce <32 hex>]
//     [--no-isolation] [--show-stderr]
//
// Starts the package's adapter as the host would, executes a transcript
// script (docs/conformance.md, "Scripts") and prints the normalized
// transcript, or writes it to a file. The transcript is the observation;
// the exit code is 0 whenever the script ran to completion, 1 when the
// script itself could not be executed, 2 on usage errors.

#include "tool_arguments.h"

#include "logonomic_cli_addon/host_simulator.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/transcript.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace addon = logonomic::cli_addon;

int usage()
{
    std::fprintf(stderr,
        "usage: logonomic_cli_addon_host_simulator <package-dir> --script <file> "
        "[--transcript <file>] [--platform <id>] [--nonce <32 hex>] [--no-isolation] [--show-stderr]\n");
    return 2;
}

std::optional<std::string> read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

int run(int argc, char** argv)
{
    std::string directory;
    std::string script_path;
    std::string transcript_path;
    bool show_stderr = false;
    addon::Simulator_options options;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--script" && i + 1 < argc)            { script_path = argv[++i]; }                                    else
        if (argument == "--transcript" && i + 1 < argc)        { transcript_path = argv[++i]; }                                else
        if (argument == "--platform" && i + 1 < argc)          { options.validation.platform_id = argv[++i]; }                 else
        if (argument == "--nonce" && i + 1 < argc)             { options.generation_nonce = argv[++i]; }                       else
        if (argument == "--no-isolation")                      { options.isolate_process_group = false; }                      else
        if (argument == "--show-stderr")                       { show_stderr = true; }                                         else
        if (!argument.empty() && argument[0] == '-')           { return usage(); }                                             else
        if (directory.empty())                                 { directory = argument; }                                       else {
            return usage();
        }
    }
    if (directory.empty() || script_path.empty() || !addon::is_valid_platform_id(options.validation.platform_id)) {
        return usage();
    }
    if (options.generation_nonce.has_value() && !addon::is_valid_generation_nonce_hex(*options.generation_nonce)) {
        return usage();
    }

    const std::optional<std::string> script_text = read_file(addon::path_from_utf8(script_path));
    if (!script_text.has_value()) {
        std::fprintf(stderr, "cannot read script %s\n", script_path.c_str());
        return 1;
    }
    addon::Json_parse_error parse_error;
    const std::optional<addon::Json_value> script = addon::parse_json(*script_text, addon::Json_parse_limits{}, &parse_error);
    if (!script.has_value()) {
        std::fprintf(stderr, "script is not valid JSON at byte %zu: %s\n", parse_error.offset, parse_error.message.c_str());
        return 1;
    }

    options.package_directory = addon::path_from_utf8(directory);
    addon::Transcript_recorder recorder;
    int exit_code = 0;
    {
        addon::Host_simulator simulator(std::move(options), &recorder);
        addon::Contract_failure failure;
        simulator.start(&failure);   // a failed start is itself recorded

        std::string script_error;
        if (!simulator.run_script(*script, &script_error)) {
            std::fprintf(stderr, "script error: %s\n", script_error.c_str());
            exit_code = 1;
        }
        if (show_stderr) {
            std::fprintf(stderr, "--- adapter stderr%s ---\n%s\n", simulator.stderr_truncated() ? " (truncated)" : "",
                simulator.stderr_text().c_str());
        }
    }

    const std::string text = recorder.text();
    if (transcript_path.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
    }
    else {
        std::ofstream output(addon::path_from_utf8(transcript_path), std::ios::binary | std::ios::trunc);
        output.write(text.data(), (std::streamsize)text.size());
        if (!output) {
            std::fprintf(stderr, "cannot write transcript %s\n", transcript_path.c_str());
            return 1;
        }
    }
    return exit_code;
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
