#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/package_validator.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/protocol.h"
#include "logonomic_cli_addon/transcript.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logonomic::cli_addon {

// ---------------------------------------------------------------------------
// Process and environment contract helpers (docs/process_contract.md)

/// The facts the adapter environment is built from.
struct Adapter_environment_inputs
{
    std::string                                      platform_id;
    std::string                                      package_id;
    std::string                                      generation_digest;
    std::string                                      generation_nonce;
    std::string                                      package_root;        // custody directory, native form
    std::string                                      scratch_directory;   // host-owned scratch, native form
    std::vector<std::uint16_t>                       core_protocol_generations;
    std::string                                      host_product;
    std::vector<std::pair<std::string, std::string>> manifest_environment;   // bootstrap adapter.environment
};

/// The complete adapter environment: the allowlisted platform baseline taken
/// from `ambient`, the manifest's variables, and the reserved
/// `LOGONOMIC_ADDON_*` variables the host sets. Nothing else is inherited;
/// a manifest variable never overrides a reserved one. Sorted by name.
std::vector<std::pair<std::string, std::string>> build_adapter_environment(
    const Adapter_environment_inputs&                       inputs,
    const std::vector<std::pair<std::string, std::string>>& ambient);

/// Whether an unreserved desktop-to-custodian start environment fits the
/// composed entry and NAME+VALUE budgets. Reserved variables are appended by
/// the custody owner and use its separate supervisor-environment budget.
bool adapter_start_environment_within_budget(
    const std::vector<std::pair<std::string, std::string>>& environment);

/// Whether a complete adapter environment, including the seven reserved
/// variables, fits the supervisor's NAME=VALUE budget.
bool adapter_supervisor_environment_within_budget(
    const std::vector<std::pair<std::string, std::string>>& environment);

/// Whether the reserved portion of a complete adapter environment fits the
/// custody owner's seven-entry NAME=VALUE sub-budget.
bool adapter_reserved_environment_within_budget(
    const std::vector<std::pair<std::string, std::string>>& environment);

/// This process's environment as name/value pairs.
std::vector<std::pair<std::string, std::string>> current_process_environment();

/// Kills one process by ID (SIGKILL / TerminateProcess). A test helper for
/// cleaning up descendants a fixture deliberately left behind; it carries no
/// custody semantics.
bool kill_process(std::int64_t pid);

/// Names of the reserved variables the host sets.
inline constexpr const char* k_env_package_id                = "LOGONOMIC_ADDON_PACKAGE_ID";
inline constexpr const char* k_env_generation_digest         = "LOGONOMIC_ADDON_GENERATION_DIGEST";
inline constexpr const char* k_env_generation_nonce          = "LOGONOMIC_ADDON_GENERATION_NONCE";
inline constexpr const char* k_env_package_root              = "LOGONOMIC_ADDON_PACKAGE_ROOT";
inline constexpr const char* k_env_scratch_dir               = "LOGONOMIC_ADDON_SCRATCH_DIR";
inline constexpr const char* k_env_core_protocol_generations = "LOGONOMIC_ADDON_CORE_PROTOCOL_GENERATIONS";
inline constexpr const char* k_env_host_product              = "LOGONOMIC_ADDON_HOST_PRODUCT";

// ---------------------------------------------------------------------------
// Host simulator

/// The disposition a caller receives for one invocation
/// (docs/core_protocol.md, "Host-side dispositions").
enum class Host_status
{
    SUCCESS,
    UNAVAILABLE,
    CANCELLED,
    FAILED,   // the wire status `error`; named to stay clear of the ERROR macro of <windows.h>
    UNSUPPORTED_VERSION,
    UNKNOWN_CAPABILITY,
    PACKAGE_UNAVAILABLE,
    TIMED_OUT,
    GENERATION_RETIRED,
};

const char* host_status_name(Host_status status);

struct Host_outcome
{
    Host_status status = Host_status::PACKAGE_UNAVAILABLE;
    std::string reason_code;
    std::string message;
    Json_value  payload = Json_value::object();
};

struct Simulator_options
{
    std::filesystem::path                       package_directory;
    Validation_options                          validation;
    Protocol_limits                             limits;
    std::string                                 host_product = "logonomic-cli-addon-host-simulator";
    std::string                                 host_version = "1.0.0";
    std::filesystem::path                       scratch_directory;       // created under the temp directory when empty
    std::filesystem::path                       profile_storage_root;    // below the scratch directory when empty
    std::optional<std::vector<Capability_pair>> adopted_capabilities;    // nullopt: every declared sidecar
    std::optional<std::string>                  generation_nonce;        // random when empty
    int                                         hello_deadline_ms     = limits::k_hello_deadline_ms;
    int                                         cancel_grace_ms       = limits::k_cancel_grace_ms;
    int                                         terminate_to_kill_ms  = limits::k_terminate_to_kill_ms;
    bool                                        isolate_process_group = true;
};

enum class Simulator_state
{
    NOT_STARTED,
    READY,
    RETIRED,
};

/// Per-invocation host behaviour.
struct Simulator_invocation_options
{
    std::optional<Profile_storage_context> profile_storage;           // injected into the payload
    bool                                   bypass_liveness = false;   // send even for a non-live pair
};

/**
 * @brief A host for one adapter generation with the host side of the core
 * protocol and none of Logonomic's custody: validates the package, spawns
 * the adapter with the contract environment, performs the handshake,
 * correlates invocations, enforces deadlines with cancel grace, applies the
 * profile-storage acknowledgment rule, and records a transcript. Every
 * violation retires the generation exactly as the contract prescribes;
 * there is no restart.
 */
class Host_simulator
{
public:
    Host_simulator(Simulator_options options, Transcript_recorder* recorder);
    ~Host_simulator();

    Host_simulator(const Host_simulator&)            = delete;
    Host_simulator& operator=(const Host_simulator&) = delete;

    /// Validates, spawns and completes the handshake. On failure the
    /// generation is retired with the reason returned.
    bool start(Contract_failure* out_failure);

    Simulator_state state() const;
    const Contract_failure& retirement() const;
    const Validated_package* package() const;
    const Session_tracker* session() const;
    std::string generation_nonce() const;
    std::string generation_digest() const;

    /// sidecar ∩ handshake ∩ adopted.
    std::vector<Capability_pair> live_capabilities() const;

    /// The request ID the next accepted invoke will carry; a caller sizing
    /// a frame against the effective bound needs its exact width.
    std::string next_request_id() const;

    /// Sends an invoke or refuses it locally; returns the request ID under
    /// which the outcome is reported. A nonpositive deadline selects the
    /// default, matching the desktop request broker.
    std::string invoke(
        const Capability_pair&              capability,
        std::string                         operation,
        Json_value                          payload,
        int                                 deadline_ms,
        const Simulator_invocation_options& invocation = Simulator_invocation_options());

    void cancel(const std::string& request_id);

    /// Pumps the session until the request has an outcome or the timeout
    /// passes; nullopt means it is still live.
    std::optional<Host_outcome> wait_outcome(const std::string& request_id, int timeout_ms);
    std::optional<Host_outcome> outcome(const std::string& request_id) const;
    static constexpr std::size_t k_retained_progress_frames = 256;
    /// Latest observations only; retaining them cannot grow with request age.
    std::vector<Progress_message> progress(const std::string& request_id) const;
    /// Total observations, saturating at the counter's representational maximum.
    std::uint64_t progress_count(const std::string& request_id) const;

    /// Pumps the session for the given time, observing whatever arrives.
    void pump(int ms);

    /// Sends shutdown and waits for the adapter to exit within the grace;
    /// otherwise terminates, then kills. True when the adapter exited by
    /// itself.
    bool shutdown(int grace_ms, int* out_exit_code);

    /// Kills the adapter tree now.
    void terminate();

    bool adapter_exited() const;
    std::optional<int> exit_code() const;
    bool exited_by_signal() const;
    std::int64_t adapter_pid() const;

    /// True when the process belongs to the adapter's process group (POSIX)
    /// or Job (Windows).
    bool owns_process(std::int64_t pid) const;

    std::string stderr_text() const;
    bool stderr_truncated() const;

    std::filesystem::path scratch_directory() const;
    std::filesystem::path profile_storage_root() const;
    const std::vector<std::string>& adapter_environment() const;

    /// Executes a transcript script (docs/conformance.md, "Scripts").
    bool run_script(const Json_value& script, std::string* out_error);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace logonomic::cli_addon
