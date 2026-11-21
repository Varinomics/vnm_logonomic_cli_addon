#pragma once

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/json.h"

#include <cstdint>
#include <optional>
#include <string>

namespace logonomic::cli_addon {

enum class Description_kind { TITLE, SUMMARY, SUMMARY_UNCHANGED };

const char* description_kind_name(Description_kind kind);

// An explicit model submission, not inferred conversation text or a terminal
// observation. The host owns title precedence, summary policy and persistence.
struct Description_submission
{
    std::string provider_session_id;
    std::string transition_nonce;
    std::string request_id;
    Description_kind kind = Description_kind::TITLE;
    std::optional<std::string> title;
    std::optional<std::string> summary;

    bool operator==(const Description_submission&) const = default;
};

// Written by the authenticated host after its existing description policy.
// Admission of the carrier event alone is not proof that text was recorded.
struct Description_receipt
{
    std::string request_id;
    bool accepted = false;
    std::string reason;

    bool operator==(const Description_receipt&) const = default;
};

bool is_valid_description_submission(const Description_submission& submission);
Json_value encode_description_submission(const Description_submission& submission);
std::optional<Description_submission> decode_description_submission(
    const Json_value& value, Contract_failure* failure = nullptr);
Json_value encode_description_receipt(const Description_receipt& receipt);
std::optional<Description_receipt> decode_description_receipt(
    const Json_value& value, Contract_failure* failure = nullptr);

void encode_cooperative_description_request(Json_value& payload, std::uint32_t version);
void decode_cooperative_description_request(
    const Json_value& payload, std::uint32_t& version, std::optional<Contract_failure>& failure);
void encode_cooperative_description_result(
    Json_value& payload, const std::optional<Description_submission>& submission,
    const std::optional<Contract_failure>& failure = std::nullopt);
void decode_cooperative_description_result(
    const Json_value& payload, std::optional<Description_submission>& submission,
    std::optional<Contract_failure>& failure);

} // namespace logonomic::cli_addon
