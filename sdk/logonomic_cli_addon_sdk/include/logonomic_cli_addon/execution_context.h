#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logonomic::cli_addon {

enum class Execution_context_status
{
    UNKNOWN,
    CONFIRMED,
    UNAVAILABLE,
};

const char* execution_context_status_name(Execution_context_status status);

// An exact native default directory observation. The producer captures the
// transition nonce and advances sequence at the native source, before any
// asynchronous read or delivery. A nonce read from the host at delivery time
// cannot attribute an observation to an earlier activation of the same ID.
struct Execution_context_update
{
    std::string provider_session_id;
    std::string transition_nonce;
    std::uint64_t sequence = 0;
    Execution_context_status status = Execution_context_status::UNKNOWN;
    std::string environment;
    std::string cwd;
    std::string source;

    bool operator==(const Execution_context_update&) const = default;
};

// Bound by the host to one accepted native activation. Reset on every new
// activation, including A -> B -> A and a new activation of A -> A. Loss of
// observation clears the current fact but retains sequence to reject replays.
struct Execution_context_state
{
    std::string provider_session_id;
    std::string transition_nonce;
    std::optional<std::uint64_t> sequence;
    Execution_context_status status = Execution_context_status::UNKNOWN;
    std::string environment;
    std::string cwd;
    std::string source;

    bool operator==(const Execution_context_state&) const = default;
};

// Byte preserving: validates local platform absolute-path syntax, UTF-8, no
// control characters and the 4096-byte bound; does not access the filesystem,
// canonicalize symlinks, trim whitespace or replace a subdirectory by a root.
bool is_execution_local_path(std::string_view path);
bool is_valid_execution_context_update(const Execution_context_update& update);

// False for a different activation, duplicate, stale sequence or invalid fact.
bool apply_execution_context_update(
    Execution_context_state& state,
    const Execution_context_update& update);

void invalidate_execution_context(Execution_context_state& state);

// Presence-tagged capability-owned envelope. Omission means no update. A
// malformed optional envelope fails only this capability, never its adjacent
// lifecycle observation. Diagnostics have no authority to clear a live fact;
// loss requires an attributed UNKNOWN/UNAVAILABLE update.
void encode_execution_context_request(Json_value& payload, std::uint32_t version);
void decode_execution_context_request(
    const Json_value& payload,
    std::uint32_t& version,
    std::optional<Contract_failure>& failure);

void encode_execution_updates(
    Json_value& payload,
    const std::vector<Execution_context_update>& updates,
    const std::optional<Contract_failure>& failure = std::nullopt);
void decode_execution_updates(
    const Json_value& payload,
    std::vector<Execution_context_update>& updates,
    std::optional<Contract_failure>& failure);

} // namespace logonomic::cli_addon
