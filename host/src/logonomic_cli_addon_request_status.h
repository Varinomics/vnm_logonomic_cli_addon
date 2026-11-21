#pragma once

enum class Logonomic_cli_capability_status
{
    SUCCESS,
    UNAVAILABLE,          // the adapter said so
    CANCELLED,            // the adapter said so
    FAILED,               // wire status `error`, or a host-side hard failure (profile storage)
    UNSUPPORTED_VERSION,  // the capability is live at other versions only
    UNKNOWN_CAPABILITY,   // not live in this generation
    PACKAGE_UNAVAILABLE,  // not discovered, invalid, quarantined, transport conflict, registry unsettled
    TIMED_OUT,            // the deadline expired and no terminal result arrived within the cancel grace
    GENERATION_RETIRED,   // crash or protocol violation during the request
};

inline const char* logonomic_cli_capability_status_name(Logonomic_cli_capability_status status)
{
    switch (status) {
        case Logonomic_cli_capability_status::SUCCESS:             return "success";
        case Logonomic_cli_capability_status::UNAVAILABLE:         return "unavailable";
        case Logonomic_cli_capability_status::CANCELLED:           return "cancelled";
        case Logonomic_cli_capability_status::FAILED:              return "failed";
        case Logonomic_cli_capability_status::UNSUPPORTED_VERSION: return "unsupported_version";
        case Logonomic_cli_capability_status::UNKNOWN_CAPABILITY:  return "unknown_capability";
        case Logonomic_cli_capability_status::PACKAGE_UNAVAILABLE: return "package_unavailable";
        case Logonomic_cli_capability_status::TIMED_OUT:           return "timed_out";
        case Logonomic_cli_capability_status::GENERATION_RETIRED:  return "generation_retired";
        default:                                                   return "unknown";
    }
}
