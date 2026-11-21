#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

enum class Model_catalog_status { UNKNOWN, AVAILABLE, UNAVAILABLE };

const char* model_catalog_status_name(Model_catalog_status status);

// Native configured availability is distinct from the currently serving model
// and from a successful access/billing test. Absence of reasoning means unknown.
struct Model_catalog_entry
{
    std::string provider;
    std::string model;
    std::optional<std::string> display_name;
    std::optional<bool> reasoning;

    bool operator==(const Model_catalog_entry&) const = default;
};

// One atomic replacement sampled from the live root's native registry. A
// bounded prefix carries the actual total and explicit incompleteness.
struct Model_catalog_update
{
    std::string provider_session_id;
    std::string transition_nonce;
    std::uint64_t sequence = 0;
    Model_catalog_status status = Model_catalog_status::UNKNOWN;
    std::vector<Model_catalog_entry> models;
    std::uint32_t total_count = 0;
    bool truncated = false;
    std::string source;

    bool operator==(const Model_catalog_update&) const = default;
};

struct Model_catalog_state
{
    std::string provider_session_id;
    std::string transition_nonce;
    std::optional<std::uint64_t> sequence;
    Model_catalog_status status = Model_catalog_status::UNKNOWN;
    std::vector<Model_catalog_entry> models;
    std::uint32_t total_count = 0;
    bool truncated = false;
    std::string source;

    bool operator==(const Model_catalog_state&) const = default;
};

bool is_valid_model_catalog_update(const Model_catalog_update& update);
bool apply_model_catalog_update(Model_catalog_state& state, const Model_catalog_update& update);
void invalidate_model_catalog(Model_catalog_state& state);

void encode_model_catalog_request(Json_value& payload, std::uint32_t version);
void decode_model_catalog_request(
    const Json_value& payload, std::uint32_t& version, std::optional<Contract_failure>& failure);
void encode_model_catalog_update(
    Json_value& payload, const std::optional<Model_catalog_update>& update,
    const std::optional<Contract_failure>& failure = std::nullopt);
void decode_model_catalog_update(
    const Json_value& payload, std::optional<Model_catalog_update>& update,
    std::optional<Contract_failure>& failure);

} // namespace logonomic::cli_addon
