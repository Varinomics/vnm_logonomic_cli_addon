#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief The common shape of a process proposal: what a capability returns
 * when host-owned work needs a helper process. The adapter proposes; the
 * host validates against the frozen request context and performs the
 * launch. Adapters never return shell command strings.
 */

enum class Working_directory_intent
{
    SESSION_WORKSPACE,
    PACKAGE_ROOT,
    SCRATCH_DIRECTORY,
    PROFILE_STORAGE_ROOT,
};

enum class Stdin_behavior
{
    CLOSED,
    INHERIT,
    PAYLOAD,   // stdin_payload is written, then stdin is closed
};

/// Only ATTACHED conforms; the others exist so that a proposal carrying them
/// can be decoded and rejected as `detached_helper_proposal`.
enum class Start_intent
{
    ATTACHED,
    DETACHED,
    NEW_SESSION,
    BREAKAWAY,
};

enum class Cleanup_intent
{
    REMOVE_SCRATCH,
    RETAIN_SCRATCH,
};

/**
 * @brief What a proposal names as its program: a canonical package path below
 * helpers/ or adapter/, or the program of the launcher the host recognized
 * (docs/process_contract.md, "The launcher-program reference"). On the wire
 * the first is the path string and the second is `{"launcher_program": true}`.
 * A package path converts implicitly because the path string is the
 * executable's own canonical spelling.
 */
struct Process_executable
{
    enum class Kind
    {
        PACKAGE_PATH,
        LAUNCHER_PROGRAM,
    };

    Process_executable() = default;
    Process_executable(std::string package_path)
    :
        path(std::move(package_path))
    {}
    Process_executable(const char* package_path)
    :
        path(package_path)
    {}

    static Process_executable launcher_program()
    {
        Process_executable executable;
        executable.kind = Kind::LAUNCHER_PROGRAM;
        return executable;
    }

    bool operator==(const Process_executable& other) const { return kind == other.kind && path == other.path; }
    bool operator!=(const Process_executable& other) const { return !(*this == other); }

    Kind        kind = Kind::PACKAGE_PATH;
    std::string path;   // PACKAGE_PATH only
};

/**
 * @brief What validate_process_proposal admits beyond the fixed rules: the
 * session contracts (structured, delegated and resume proposals) allow the
 * launcher-program reference, the conformance probe does not.
 */
struct Process_proposal_policy
{
    bool launcher_program_allowed = false;
};

struct Process_proposal
{
    Process_executable                               executable;
    std::vector<std::string>                         argv;
    Working_directory_intent                         working_directory = Working_directory_intent::SESSION_WORKSPACE;
    Stdin_behavior                                   stdin_behavior    = Stdin_behavior::CLOSED;
    std::string                                      stdin_payload;
    Start_intent                                     start             = Start_intent::ATTACHED;
    std::vector<std::string>                         environment_names;   // baseline names to pass through
    std::vector<std::pair<std::string, std::string>> environment;         // capability-scoped values
    std::vector<std::string>                         assets;              // canonical paths below assets/ or helpers/
    Cleanup_intent                                   cleanup           = Cleanup_intent::REMOVE_SCRATCH;
};

const char* working_directory_intent_name(Working_directory_intent intent);
const char* stdin_behavior_name(Stdin_behavior behavior);
const char* start_intent_name(Start_intent intent);
const char* cleanup_intent_name(Cleanup_intent intent);

/// Strict structural decode of the wire object (closed member set, typed
/// members, vocabulary names; `executable` is a string or exactly
/// `{"launcher_program": true}`). Reason codes: `frame_violation`,
/// `unexpected_member`. Policy is validate_process_proposal's job.
std::optional<Process_proposal> decode_process_proposal(const Json_value& value, Contract_failure* out_failure);

Json_value encode_process_proposal(const Process_proposal& proposal);

/// Policy: rejects shell entry points (`shell_adapter_entry_point`),
/// non-canonical executable or asset paths (`path_escape`), the
/// launcher-program reference unless the policy allows it (`path_escape`),
/// reserved or malformed environment names (`reserved_environment_variable`),
/// argv and environment bounds (`payload_too_large`), and any start intent
/// other than ATTACHED (`detached_helper_proposal`).
bool validate_process_proposal(
    const Process_proposal&        proposal,
    const Process_proposal_policy& policy,
    Contract_failure*              out_failure);

} // namespace logonomic::cli_addon
