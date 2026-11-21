#include "sdk_test_support.h"

#include "logonomic_cli_addon/usage_repository_attribution.h"

namespace sdk_tests {

bool test_usage_repository_attribution_contracts()
{
    namespace addon = logonomic::cli_addon;
    using Status = addon::Usage_repository_attribution_tag_status;
    const char* id = __func__;
    bool ok = true;
    addon::Contract_failure failure;
    ok &= check(addon::decode_usage_repository_attribution_declaration(addon::Json_value::object(), &failure),
        id, "the independent capability declares no options");
    auto tag = addon::encode_usage_repository_attribution_tag();
    ok &= check(!addon::decode_usage_repository_attribution_declaration(tag, &failure),
        id, "a record tag is not a capability declaration");
    ok &= check(addon::decode_usage_repository_attribution_tag(tag, &failure) == Status::AVAILABLE,
        id, "version one grants the recognized repository-only tag");
    tag.set("total_tokens", addon::Json_value(1));
    ok &= check(addon::decode_usage_repository_attribution_tag(tag, &failure) == Status::INVALID,
        id, "recognized tags cannot smuggle accounting authority");
    tag.set("version", addon::Json_value(2));
    ok &= check(addon::decode_usage_repository_attribution_tag(tag, &failure) == Status::UNSUPPORTED,
        id, "future optional versions remain opaque and grant no authority");
    for (const auto& version : {addon::Json_value(), addon::Json_value(0), addon::Json_value(-1),
             addon::Json_value("1"), addon::Json_value(true)}) {
        tag = addon::Json_value::object();
        tag.set("version", version);
        ok &= check(addon::decode_usage_repository_attribution_tag(tag, &failure) == Status::INVALID,
            id, "malformed versions grant no correction authority");
    }
    ok &= check(addon::decode_usage_repository_attribution_tag(addon::Json_value::object(), &failure) == Status::INVALID,
        id, "missing versions grant no correction authority");
    return ok;
}

} // namespace sdk_tests
