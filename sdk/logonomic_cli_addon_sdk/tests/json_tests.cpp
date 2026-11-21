#include "sdk_test_support.h"

#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/json_utilities.h"

#if defined(__APPLE__)
#include <xlocale.h>
#include <cfenv>
#endif

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace sdk_tests {

namespace {

namespace addon = logonomic::cli_addon;

std::optional<addon::Json_value> parse(std::string_view text, const addon::Json_parse_limits& limits = {})
{
    return addon::parse_json(text, limits, nullptr);
}

bool rejects(std::string_view text, const addon::Json_parse_limits& limits = {})
{
    return !addon::parse_json(text, limits, nullptr).has_value();
}

} // namespace

bool test_json_accepts_rfc8259_documents()
{
    bool ok = true;
    const char* id = __func__;

    // RFC 8259 section 2: any value may be a top-level text, with the four
    // insignificant whitespace characters around it.
    ok &= check(parse(" \t\r\n{} \t\r\n").has_value(), id, "object with surrounding whitespace");
    ok &= check(parse("[]").has_value(),                id, "empty array");
    ok &= check(parse("\"text\"").has_value(),          id, "top-level string");
    ok &= check(parse("true").has_value(),              id, "top-level true");
    ok &= check(parse("false").has_value(),             id, "top-level false");
    ok &= check(parse("null").has_value(),              id, "top-level null");
    ok &= check(parse("0").has_value(),                 id, "top-level zero");
    ok &= check(parse("-0").has_value(),                id, "negative zero");
    ok &= check(parse("1.5e3").has_value(),             id, "fraction and exponent");
    ok &= check(parse("1E-2").has_value(),              id, "upper-case exponent");

    const std::optional<addon::Json_value> nested = parse(R"({"a":[1,{"b":null},"c"],"d":{"e":true}})");
    ok &= check(nested.has_value() && nested->is_object(), id, "nested document parses");
    if (nested.has_value()) {
        const addon::Json_value* a = nested->find("a");
        ok &= check(a != nullptr && a->is_array() && a->as_array().size() == 3, id, "array member has three items");
        ok &= check(a != nullptr && a->as_array()[1].find("b") != nullptr && a->as_array()[1].find("b")->is_null(), id, "nested null");
        ok &= check(nested->find("d") != nullptr && nested->find("d")->find("e")->as_boolean(), id, "nested boolean");
        ok &= check(nested->find("missing") == nullptr, id, "absent member is null pointer");
    }

    // Section 7: every escape, including a surrogate pair encoding one scalar.
    const std::optional<addon::Json_value> escapes = parse(R"("\"\\\/\b\f\n\r\t\u00e9\ud83d\ude00")");
    ok &= check(escapes.has_value(), id, "all escapes parse");
    if (escapes.has_value()) {
        ok &= check(escapes->as_string() == std::string("\"\\/\b\f\n\r\t") + "\xC3\xA9" + "\xF0\x9F\x98\x80", id,
            "escapes decode to the expected bytes");
    }

    const std::string ascii(64u * 1024u, 'a');
    const std::string unicode = "\xE4\xB8\xAD\xF0\x9F\x98\x80";
    const auto mixed = parse('"' + ascii + "\\n" + unicode + "\\u00e9" + ascii + '"');
    ok &= check(mixed.has_value() && mixed->as_string() == ascii + '\n' + unicode + "\xC3\xA9" + ascii, id,
        "long ASCII strings preserve adjoining escapes and UTF-8 scalars");

    // Member order is preserved as written.
    const std::optional<addon::Json_value> ordered = parse(R"({"z":1,"a":2,"m":3})");
    ok &= check(ordered.has_value() && ordered->as_object()[0].name == "z" && ordered->as_object()[2].name == "m", id,
        "member order preserved");
    return ok;
}

bool test_json_rejects_rfc8259_violations()
{
    bool ok = true;
    const char* id = __func__;

    ok &= check(rejects(""),                 id, "empty input");
    ok &= check(rejects("   "),              id, "whitespace only");
    ok &= check(rejects("{\"a\":1,}"),       id, "trailing comma in object");
    ok &= check(rejects("[1,]"),             id, "trailing comma in array");
    ok &= check(rejects("[,1]"),             id, "leading comma");
    ok &= check(rejects("{'a':1}"),          id, "single quotes");
    ok &= check(rejects("{a:1}"),            id, "unquoted member name");
    ok &= check(rejects("{\"a\" 1}"),        id, "missing colon");
    ok &= check(rejects("01"),               id, "leading zero");
    ok &= check(rejects("+1"),               id, "leading plus");
    ok &= check(rejects(".5"),               id, "leading decimal point");
    ok &= check(rejects("1."),               id, "trailing decimal point");
    ok &= check(rejects("1e"),               id, "empty exponent");
    ok &= check(rejects("1e+"),              id, "signed empty exponent");
    ok &= check(rejects("0x10"),             id, "hexadecimal");
    ok &= check(rejects("NaN"),              id, "NaN literal");
    ok &= check(rejects("Infinity"),         id, "Infinity literal");
    ok &= check(rejects("-Infinity"),        id, "negative Infinity literal");
    ok &= check(rejects("tru"),              id, "truncated literal");
    ok &= check(rejects("True"),             id, "capitalised literal");
    ok &= check(rejects("nul"),              id, "truncated null");
    ok &= check(rejects("\"unterminated"),   id, "unterminated string");
    ok &= check(rejects("\"tab\there\""),    id, "raw tab in string");
    ok &= check(rejects("\"nl\nhere\""),     id, "raw newline in string");
    ok &= check(rejects("\"\\x41\""),        id, "invalid escape");
    ok &= check(rejects("\"\\u12\""),        id, "truncated unicode escape");
    ok &= check(rejects("\"\\u12g4\""),      id, "non-hex unicode escape");
    ok &= check(rejects("\"\\ud800\""),      id, "lone high surrogate");
    ok &= check(rejects("\"\\udc00\""),      id, "lone low surrogate");
    ok &= check(rejects("\"\\ud800\\u0041\""), id, "high surrogate followed by non-surrogate");
    ok &= check(rejects("{} {}"),            id, "two top-level values");
    ok &= check(rejects("[1] x"),            id, "trailing garbage");
    ok &= check(rejects("// c\n1"),          id, "comments");
    ok &= check(rejects("\xEF\xBB\xBF{}"),   id, "byte-order mark");
    ok &= check(rejects("{\"a\":1 \"b\":2}"), id, "missing comma between members");
    ok &= check(rejects("[1 2]"),            id, "missing comma between items");
    ok &= check(rejects("{\"a\":}"),         id, "missing member value");
    ok &= check(rejects("[\"a\""),           id, "unterminated array");
    ok &= check(rejects("{\"a\":1"),         id, "unterminated object");

    addon::Json_parse_error error;
    ok &= check(!addon::parse_json("[1,2,x]", {}, &error).has_value() && error.offset == 5, id,
        "error offset points at the defect");
    return ok;
}

bool test_json_utf8_follows_rfc3629()
{
    bool ok = true;
    const char* id = __func__;

    // Well-formed sequences at each length and at the boundaries of the
    // RFC 3629 table.
    ok &= check(addon::is_valid_utf8("plain ascii"),               id, "ascii");
    ok &= check(addon::is_valid_utf8("\xC2\x80"),                  id, "U+0080 shortest form");
    ok &= check(addon::is_valid_utf8("\xDF\xBF"),                  id, "U+07FF");
    ok &= check(addon::is_valid_utf8("\xE0\xA0\x80"),              id, "U+0800 shortest form");
    ok &= check(addon::is_valid_utf8("\xE2\x82\xAC"),              id, "U+20AC");
    ok &= check(addon::is_valid_utf8("\xED\x9F\xBF"),              id, "U+D7FF just below the surrogates");
    ok &= check(addon::is_valid_utf8("\xEE\x80\x80"),              id, "U+E000 just above the surrogates");
    ok &= check(addon::is_valid_utf8("\xEF\xBF\xBF"),              id, "U+FFFF");
    ok &= check(addon::is_valid_utf8("\xF0\x90\x80\x80"),          id, "U+10000 shortest form");
    ok &= check(addon::is_valid_utf8("\xF4\x8F\xBF\xBF"),          id, "U+10FFFF");

    // Ill-formed: overlong, surrogates, above U+10FFFF, truncated, stray
    // continuation, invalid lead bytes.
    ok &= check(!addon::is_valid_utf8("\xC0\x80"),                 id, "overlong two-byte NUL");
    ok &= check(!addon::is_valid_utf8("\xC1\xBF"),                 id, "overlong two-byte U+007F");
    ok &= check(!addon::is_valid_utf8("\xE0\x80\x80"),             id, "overlong three-byte");
    ok &= check(!addon::is_valid_utf8("\xE0\x9F\xBF"),             id, "overlong three-byte U+07FF");
    ok &= check(!addon::is_valid_utf8("\xF0\x80\x80\x80"),         id, "overlong four-byte");
    ok &= check(!addon::is_valid_utf8("\xF0\x8F\xBF\xBF"),         id, "overlong four-byte U+FFFF");
    ok &= check(!addon::is_valid_utf8("\xED\xA0\x80"),             id, "surrogate U+D800");
    ok &= check(!addon::is_valid_utf8("\xED\xBF\xBF"),             id, "surrogate U+DFFF");
    ok &= check(!addon::is_valid_utf8("\xF4\x90\x80\x80"),         id, "above U+10FFFF");
    ok &= check(!addon::is_valid_utf8("\xF5\x80\x80\x80"),         id, "lead byte F5");
    ok &= check(!addon::is_valid_utf8("\xF8\x88\x80\x80\x80"),     id, "five-byte form");
    ok &= check(!addon::is_valid_utf8("\xFE"),                     id, "lead byte FE");
    ok &= check(!addon::is_valid_utf8("\xFF"),                     id, "lead byte FF");
    ok &= check(!addon::is_valid_utf8("\x80"),                     id, "stray continuation");
    ok &= check(!addon::is_valid_utf8("\xE2\x82"),                 id, "truncated three-byte");
    ok &= check(!addon::is_valid_utf8("\xF0\x9F\x98"),             id, "truncated four-byte");
    ok &= check(!addon::is_valid_utf8("\xC3\x28"),                 id, "bad continuation byte");
    ok &= check(!addon::is_valid_utf8("\xE2\x28\xA1"),             id, "bad second byte");

    // The parser applies the same rules inside strings.
    ok &= check(parse("\"\xE2\x82\xAC\"").has_value(),             id, "valid UTF-8 string parses");
    ok &= check(rejects("\"\xC0\x80\""),                           id, "overlong inside string rejected");
    ok &= check(rejects("\"\xED\xA0\x80\""),                       id, "surrogate inside string rejected");
    ok &= check(rejects("\"\xFF\""),                               id, "invalid byte inside string rejected");
    return ok;
}

bool test_json_bounds_and_duplicates()
{
    bool ok = true;
    const char* id = __func__;

    ok &= check(rejects(R"({"a":1,"a":2})"), id, "duplicate member names rejected");
    ok &= check(rejects(R"({"a":{"b":1,"b":2}})"), id, "nested duplicate member names rejected");
    ok &= check(parse(R"({"a":1,"A":2})").has_value(), id, "names differing in case are distinct");

    // Depth counts arrays and objects; a scalar has depth 0.
    std::string deep_32(32, '[');
    deep_32 += std::string(32, ']');
    std::string deep_33(33, '[');
    deep_33 += std::string(33, ']');
    ok &= check(parse(deep_32).has_value(), id, "depth 32 accepted at the default bound");
    ok &= check(rejects(deep_33),           id, "depth 33 rejected at the default bound");
    ok &= check(addon::json_depth(*parse(deep_32)) == 32, id, "json_depth reports 32");
    ok &= check(addon::json_depth(*parse("7")) == 0,      id, "json_depth of a scalar is 0");
    ok &= check(addon::json_depth(*parse("{\"a\":[]}")) == 2, id, "json_depth of object with empty array is 2");

    addon::Json_parse_limits tight;
    tight.max_depth = 2;
    ok &= check(parse("[[1]]", tight).has_value(), id, "depth at a custom bound accepted");
    ok &= check(rejects("[[[1]]]", tight),         id, "depth above a custom bound rejected");

    tight = addon::Json_parse_limits{};
    tight.max_members = 2;
    ok &= check(parse(R"({"a":1,"b":2})", tight).has_value(), id, "member count at bound accepted");
    ok &= check(rejects(R"({"a":1,"b":2,"c":3})", tight),     id, "member count above bound rejected");

    tight = addon::Json_parse_limits{};
    tight.max_string_bytes = 4;
    ok &= check(parse("\"abcd\"", tight).has_value(), id, "string at the byte bound accepted");
    ok &= check(rejects("\"abcde\"", tight),          id, "string above the byte bound rejected");
    ok &= check(rejects("\"\\u00e9\\u00e9\\u00e9\"", tight), id, "decoded bytes count against the bound");

    addon::Json_parse_error error;
    ok &= check(!addon::parse_json("\"abcdef\"", tight, &error).has_value() &&
        error.offset == 6 && error.message == "string length exceeds the limit", id,
        "ASCII string limit failure stops after the first excess byte");
    ok &= check(!addon::parse_json("\"abcde", tight, &error).has_value() &&
        error.offset == 6 && error.message == "unterminated string", id,
        "end of input retains precedence over an exceeded string limit");

    tight.max_string_bytes = 0;
    ok &= check(parse("\"\"", tight).has_value(), id, "empty string fits a zero byte limit");
    ok &= check(rejects("\"a\"", tight), id, "nonempty string exceeds a zero byte limit");

    tight.max_string_bytes = std::numeric_limits<std::uint64_t>::max();
    ok &= check(parse("\"unbounded ASCII\"", tight).has_value(), id,
        "maximum string byte limit does not overflow ASCII scanning");

    tight = addon::Json_parse_limits{};
    tight.max_bytes = 5;
    ok &= check(parse("[1,2]", tight).has_value(), id, "document at the byte bound accepted");
    ok &= check(rejects("[1,22]", tight),          id, "document above the byte bound rejected");

    // Arrays are bounded by the document size, not by the member bound.
    tight = addon::Json_parse_limits{};
    tight.max_members = 2;
    ok &= check(parse("[1,2,3,4,5]", tight).has_value(), id, "array items are not limited by max_members");
    return ok;
}

bool test_json_numbers_are_exact()
{
    bool ok = true;
    const char* id = __func__;

    const std::optional<addon::Json_value> max = parse("9223372036854775807");
    ok &= check(max.has_value() && max->is_integer() && max->as_integer() == std::numeric_limits<std::int64_t>::max(), id,
        "INT64_MAX is an exact integer");
    const std::optional<addon::Json_value> min = parse("-9223372036854775808");
    ok &= check(min.has_value() && min->is_integer() && min->as_integer() == std::numeric_limits<std::int64_t>::min(), id,
        "INT64_MIN is an exact integer");
    ok &= check(rejects("9223372036854775808"),  id, "INT64_MAX + 1 rejected");
    ok &= check(rejects("-9223372036854775809"), id, "INT64_MIN - 1 rejected");
    ok &= check(rejects("123456789012345678901234567890"), id, "very large integer rejected");

    const std::optional<addon::Json_value> negative_zero = parse("-0");
    ok &= check(negative_zero.has_value() && negative_zero->is_integer() && negative_zero->as_integer() == 0, id,
        "-0 is the integer zero");

    const std::optional<addon::Json_value> fraction = parse("1.0");
    ok &= check(fraction.has_value() && fraction->type() == addon::Json_type::NUMBER && !fraction->is_integer(), id,
        "1.0 is a NUMBER, not an INTEGER");
    ok &= check(fraction.has_value() && fraction->is_number() && fraction->as_number() == 1.0, id, "1.0 reads as 1.0");
    const std::optional<addon::Json_value> exponent = parse("1e2");
    ok &= check(exponent.has_value() && exponent->type() == addon::Json_type::NUMBER && exponent->as_number() == 100.0, id,
        "exponent forms are NUMBERs");
    ok &= check(rejects("1e400"),  id, "overflowing number rejected");
    ok &= check(rejects("-1e400"), id, "negative overflowing number rejected");
    ok &= check(rejects("1e-400"), id, "underflowing number rejected");

    const std::optional<addon::Json_value> max_double = parse("1.7976931348623157e308");
    ok &= check(max_double.has_value() && max_double->as_number() == std::numeric_limits<double>::max(), id,
        "largest finite double accepted");
    const std::optional<addon::Json_value> min_double = parse("2.2250738585072014e-308");
    ok &= check(min_double.has_value() && min_double->as_number() == std::numeric_limits<double>::min(), id,
        "smallest normal double accepted");
    const std::optional<addon::Json_value> subnormal = parse("5e-324");
    ok &= check(subnormal.has_value() && subnormal->as_number() == std::numeric_limits<double>::denorm_min(), id,
        "smallest subnormal double accepted");
    ok &= check(rejects("2e-324"), id, "nonzero decimal rounded to zero rejected");
    const std::optional<addon::Json_value> floating_negative_zero = parse("-0.0");
    ok &= check(floating_negative_zero.has_value() && floating_negative_zero->as_number() == 0.0 &&
        std::signbit(floating_negative_zero->as_number()), id, "floating negative zero preserves its sign");

#if defined(__APPLE__)
    locale_t comma_locale = newlocale(LC_NUMERIC_MASK, "de_DE.UTF-8", nullptr);
    ok &= check(comma_locale != nullptr, id, "comma-decimal locale is available");
    if (comma_locale) {
        locale_t previous_locale = uselocale(comma_locale);
        ok &= check(previous_locale != nullptr, id, "thread locale changed for decimal parsing check");
        if (previous_locale) {
            const std::optional<addon::Json_value> locale_number = parse("1.5");
            ok &= check(locale_number.has_value() && locale_number->as_number() == 1.5, id,
                "decimal point is independent of the ambient locale");
            ok &= check(rejects("1,5"), id, "ambient decimal comma remains invalid JSON");
            ok &= check(uselocale(nullptr) == comma_locale, id, "parser preserves the caller's locale");
            (void)uselocale(previous_locale);
        }
        freelocale(comma_locale);
    }

    const int previous_rounding = std::fegetround();
    const bool changed_rounding = std::fesetround(FE_UPWARD) == 0;
    ok &= check(changed_rounding, id, "upward rounding is available");
    if (changed_rounding) {
        const std::optional<addon::Json_value> rounded = parse("0.1");
        ok &= check(rounded.has_value() && rounded->as_number() == 0.1, id,
            "decimal parsing rounds to nearest independently of the ambient mode");
        ok &= check(std::fegetround() == FE_UPWARD, id, "parser restores the caller's rounding mode");
        (void)std::fesetround(previous_rounding);
    }
#endif

    const std::optional<addon::Json_value> integer = parse("42");
    ok &= check(integer.has_value() && integer->as_number() == 42.0, id, "as_number converts an integer");
    return ok;
}

bool test_json_serialization_forms()
{
    bool ok = true;
    const char* id = __func__;

    const std::optional<addon::Json_value> document = parse(R"({"z":[1,2.5,true,null],"a":{"y":"v","b":-3},"m":"text"})");
    SDK_CHECK(document.has_value(), "document parses");

    ok &= check(addon::serialize_json(*document) == R"({"z":[1,2.5,true,null],"a":{"y":"v","b":-3},"m":"text"})", id,
        "compact serialization preserves order and whitespace-free form");
    ok &= check(addon::serialize_json_canonical(*document) == R"({"a":{"b":-3,"y":"v"},"m":"text","z":[1,2.5,true,null]})", id,
        "canonical serialization sorts members bytewise at every level");

    // Bytewise order: uppercase before lowercase, shorter prefix first.
    const std::optional<addon::Json_value> order = parse(R"({"b":1,"B":2,"ab":3,"a":4})");
    ok &= check(addon::serialize_json_canonical(*order) == R"({"B":2,"a":4,"ab":3,"b":1})", id, "bytewise member order");

    // Minimal escapes: quote, backslash, the short controls, \u00XX for the
    // rest, everything else raw UTF-8.
    const addon::Json_value text(std::string("q\"b\\n\nr\rt\tf\fb\bc\x01" "\xC3\xA9/"));
    ok &= check(addon::serialize_json(text) == "\"q\\\"b\\\\n\\nr\\rt\\tf\\fb\\bc\\u0001\xC3\xA9/\"", id, "minimal escapes");

    const addon::Json_value empty("");
    ok &= check(addon::serialize_json(empty) == "\"\"" &&
        addon::serialize_json_canonical(empty) == "\"\"", id, "empty strings retain both quotes");

    const addon::Json_value controls(std::string(
        "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"
        "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f", 32));
    const std::string escaped_controls =
        "\"\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f"
        "\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a\\u001b"
        "\\u001c\\u001d\\u001e\\u001f\"";
    ok &= check(addon::serialize_json(controls) == escaped_controls &&
        addon::serialize_json_canonical(controls) == escaped_controls, id,
        "all consecutive control bytes use the minimal escapes");

    const std::string ascii(4096, 's');
    const std::string unicode = "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
    addon::Json_value boundaries = addon::Json_value::object();
    boundaries.set("\n\"\\", addon::Json_value(ascii + "\n\t" + unicode + ascii + "\r"));
    const std::string escaped_boundaries =
        "{\"\\n\\\"\\\\\":\"" + ascii + "\\n\\t" + unicode + ascii + "\\r\"}";
    ok &= check(addon::serialize_json(boundaries) == escaped_boundaries &&
        addon::serialize_json_canonical(boundaries) == escaped_boundaries, id,
        "member names and long string runs retain leading, adjacent and trailing escapes");

    const addon::Json_value malformed(std::string("\x80\xC0\xAF\xFF"));
    const std::string malformed_wire = "\"\x80\xC0\xAF\xFF\"";
    ok &= check(addon::serialize_json(malformed) == malformed_wire &&
        addon::serialize_json_canonical(malformed) == malformed_wire && rejects(malformed_wire), id,
        "serialization preserves caller bytes while the bounded parser rejects malformed UTF-8");

    // NUMBER values keep a fraction so their type survives a round trip;
    // INTEGER values never gain one.
    ok &= check(addon::serialize_json(addon::Json_value(1.0)) == "1.0",  id, "integral double keeps .0");
    ok &= check(addon::serialize_json(addon::Json_value(0.1)) == "0.1",  id, "shortest round-trip double");
    ok &= check(addon::serialize_json(addon::Json_value((std::int64_t)7)) == "7", id, "integer stays integer");
    const std::optional<addon::Json_value> round = parse(addon::serialize_json(addon::Json_value(2.0)));
    ok &= check(round.has_value() && round->type() == addon::Json_type::NUMBER, id, "NUMBER survives a round trip");

    // Round trip of the canonical form is a fixed point.
    const std::string canonical = addon::serialize_json_canonical(*document);
    ok &= check(addon::serialize_json_canonical(*parse(canonical)) == canonical, id, "canonical form is a fixed point");

    // Building values programmatically.
    addon::Json_value built = addon::Json_value::object();
    built.set("second", addon::Json_value(2));
    built.set("first", addon::Json_value("1"));
    built.set("second", addon::Json_value(22));
    ok &= check(addon::serialize_json(built) == R"({"second":22,"first":"1"})", id, "set replaces in place");
    ok &= check(built.erase("first") && !built.erase("first"), id, "erase removes once");
    ok &= check(addon::serialize_json(built) == R"({"second":22})", id, "erase leaves the rest");
    ok &= check(*document == *parse(addon::serialize_json(*document)), id, "equality after a round trip");
    return ok;
}

bool test_json_utilities()
{
    bool ok = true;
    const char* id = __func__;
    namespace json = addon::json;
    using Json = addon::Json_value;

    const Json value = json::object({{"name", Json("value")}, {"count", Json(2)}});
    ok &= check(addon::serialize_json(value) == R"({"name":"value","count":2})", id,
        "object construction preserves member order");
    ok &= check(json::member(value, "count") == Json(2), id, "member returns the stored value");
    ok &= check(json::member(value, "missing").is_null() && json::member(Json(2), "name").is_null(), id,
        "member returns null for absent keys and non-objects");
    ok &= check(json::text(value, "name") == "value" && json::text(value, "count").empty() &&
        json::text(value, "missing").empty(), id, "text accepts only string members");
    ok &= check(json::only_keys(value, {"name", "count", "optional"}) &&
        !json::only_keys(value, {"name"}), id, "allowed keys may be absent but unexpected keys fail");
    ok &= check(json::exact_keys(value, {"count", "name"}) &&
        !json::exact_keys(value, {"name", "count", "optional"}), id, "exact keys require the full set");
    ok &= check(!json::only_keys(Json(2), {}) && !json::exact_keys(Json(2), {}) &&
        json::exact_keys(Json::object(), {}), id, "key checks require an object, including the empty set");

    struct Bounded_case
    {
        Json        value;
        std::size_t maximum;
        bool        allow_empty;
        bool        accepted;
    };
    const Bounded_case cases[] = {
        {Json(2),                         2, false, false},
        {Json(""),                        0, false, false},
        {Json(""),                        0, true,  true},
        {Json("ab"),                      2, false, true},
        {Json("abc"),                     2, true,  false},
        {Json(std::string("a\0b", 3)),     3, true,  false},
        {Json("\xC3\xA9"),                1, false, false},
        {Json("\xC3\xA9"),                2, false, true},
        {Json("\n"),                      1, false, true},
    };
    for (const auto& test : cases) {
        ok &= check(json::bounded_string(test.value, test.maximum, test.allow_empty) == test.accepted, id,
            "bounded strings preserve byte limits, empty opt-in and NUL exclusion");
    }
    ok &= check(!json::bounded_string(Json(""), 1), id, "empty strings are rejected by default");
    ok &= check(json::parse("2") == std::optional<Json>(Json(2)) &&
        !json::parse(R"({"a":1,"a":2})"), id, "parse uses the SDK's strict parser");
    return ok;
}

} // namespace sdk_tests
