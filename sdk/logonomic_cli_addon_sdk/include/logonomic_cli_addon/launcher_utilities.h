#pragma once

#include "adapter_runtime.h"
#include "json.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logonomic::cli_addon {

struct Launcher_request;

using Environment = std::vector<std::pair<std::string, std::string>>;

struct Probe_output
{
    std::string output;
    std::string errors;
};

// Launcher grammars differ in which whitespace surrounds words and PATH/PATHEXT
// entries. ASCII recognizes space, tab, CR and LF; UTF8 also recognizes the
// Unicode whitespace used by the direct launcher grammar.
enum class Launcher_whitespace
{
    ASCII,
    UTF8,
};

std::string trim(std::string_view value, Launcher_whitespace whitespace = Launcher_whitespace::UTF8);
std::string lower(std::string value);
std::string unquote(std::string_view value, Launcher_whitespace whitespace = Launcher_whitespace::UTF8);
std::string base_name(std::string_view value, Launcher_whitespace whitespace = Launcher_whitespace::UTF8);
// Limited launcher inspection grammar: quoted words retain their spelling,
// and segments split at the runner's unquoted separators, trim their edges and
// omit empty parts. Bash uses backslash escapes, PowerShell backticks, cmd carets.
std::vector<std::string> launcher_tokens(std::string_view text, std::string_view runner);
std::vector<std::string> launcher_segments(
    std::string_view line, std::string_view runner, Launcher_whitespace whitespace);
// Matching outer quotes are removed. Names follow [A-Za-z_][A-Za-z0-9_]*;
// empty values are retained and NUL bytes reject the assignment.
std::optional<std::pair<std::string, std::string>> environment_assignment(
    std::string_view text, Launcher_whitespace whitespace);
// Direct runner argument splitting: whitespace separates words, double quotes
// group words, and three consecutive double quotes produce one literal quote.
// Empty words are omitted; no shell expansion or escape processing is performed.
std::vector<std::string> literal_argv(std::string_view script);

struct Launcher_invocation
{
    std::vector<std::string> argv;
    std::vector<std::string> environment_names;
};

// Projects one bounded literal invocation without executing a shell. Direct
// uses the host's strict quote validation and Unicode whitespace; bash and
// Windows PowerShell/cmd share the approval editor's
// literal grammar. Unsupported expansions, operators or native-word ambiguity
// return nullopt. PowerShell follows the host's Windows powershell.exe runner.
// Leading bash assignments contribute names only: callers must obtain every
// value from the host's frozen effective environment before replacing a launch.
// When supplied, host_argv must match the direct words or the selected shell's
// complete host invocation and exact script. Present empty evidence is invalid.
std::optional<Launcher_invocation> literal_launcher_invocation(
    const Launcher_request& launcher, std::string_view platform_id,
    std::optional<std::span<const std::string>> host_argv = std::nullopt);

bool is_semantic_version(std::string_view value);
// Labels are literal, case-insensitive names. Labelled versions take precedence
// over other lines; a lone unlabelled version may have a leading v or V.
// nullopt rejects invalid UTF-8, NUL, output over 64 KiB or conflicting labelled
// versions; an empty string means valid text without a recognized version.
std::optional<std::string> parse_version_stream(
    std::string_view value, std::span<const std::string_view> labels,
    Launcher_whitespace whitespace = Launcher_whitespace::UTF8);
std::string environment_value(const Environment& environment, std::string_view name);
void set_environment(Environment& environment, std::string name, std::string value);
Environment environment_pairs(const Json_value& value);
// Normalize an absolute UTF-8 process folder lexically, using native separators.
// Retains link/junction/substituted-drive spelling; performs no I/O.
// Invalid UTF-8, NUL bytes and nonabsolute paths return nullopt.
std::optional<std::filesystem::path> normalized_resume_directory(std::string_view directory);
std::optional<std::filesystem::path> regular_file(const std::filesystem::path& path);
std::optional<std::filesystem::path> resolve_program(
    std::string_view program, const Environment& environment, std::string_view working_directory,
    Launcher_whitespace whitespace = Launcher_whitespace::UTF8);
bool native_image(const std::filesystem::path& path);
#ifdef _WIN32
// Returns the regular Node entry named by a complete stock npm cmd/PowerShell
// shim. Callers verify the package and its bin identity and distribute the
// npm cmd-shim ISC notice reproduced in src/npm_node_shim.cpp.
std::optional<std::filesystem::path> npm_node_shim_entry(const std::filesystem::path& shim);
#endif
// Prefer Direct for the already-selected native executable. Script wrappers
// retain their existing runner; this does not resolve PATH or run a process.
std::string preferred_discovery_runner(
    const std::filesystem::path& resolved_program, std::string_view fallback_runner);

#ifdef _WIN32
std::wstring wide(std::string_view text);
// CreateProcess/CRT argument quoting, not shell-language escaping.
std::wstring quote_argument(std::wstring_view argument);
#else
// A failed open closes both ends, resets them to -1 and preserves errno.
bool open_cloexec_pipe(int (&descriptors)[2]);
#endif

struct Probe_exchange
{
    std::string initial_input;
    std::function<std::optional<std::string>(std::string_view)> on_output;
    std::function<void()> tick;
    bool complete = false;
};

// argv must be nonempty and name an executable explicitly. Uses only the supplied
// environment. Without an exchange stdin is EOF and each output stream is bounded
// at 64 KiB. With an exchange each stream is bounded at 2 MiB; on_output receives
// accumulated stdout, nullopt rejects it, and a reply is appended to pending stdin.
// Completion closes stdin and allows 250 ms for final output before retirement.
// Cancellation, timeout, overflow, spawn failure or unconfirmed retirement produce
// no result. A completed exchange permits nonzero exit; EOF probes require exit zero.
// Retirement waits at most one second: Windows settles the private job, while
// POSIX retains the custodian's process group and confirms only its leader.
// Unreaped POSIX leaders remain in bounded custody; saturation refuses admission.
std::optional<Probe_output> probe(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, int timeout_ms, Invocation_context& context,
    Probe_exchange* exchange = nullptr);

// out_timed_out is true only when the execution deadline stopped the probe.
std::optional<Probe_output> probe_with_timeout_status(
    const std::vector<std::string>& argv, const Environment& environment,
    std::string_view working_directory, int timeout_ms, Invocation_context& context,
    bool& out_timed_out, Probe_exchange* exchange = nullptr);

} // namespace logonomic::cli_addon
