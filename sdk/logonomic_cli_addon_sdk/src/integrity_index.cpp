#include "logonomic_cli_addon/integrity_index.h"
#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/limits.h"
#include "logonomic_cli_addon/version.h"
#include "strict_json.h"

#include <algorithm>
#include <map>
#include <set>

namespace logonomic::cli_addon {

namespace {

std::optional<Integrity_index> fail(
    Contract_failure* out_failure,
    const char*       reason_code,
    std::string       detail)
{
    if (out_failure != nullptr) {
        out_failure->reason_code = reason_code;
        out_failure->detail      = std::move(detail);
    }
    return std::nullopt;
}

struct Kind_name
{
    Resource_kind kind;
    const char*   name;
    const char*   segment;
};

constexpr Kind_name k_kind_names[] = {
    {Resource_kind::EXECUTABLE, "executable", k_segment_adapter},
    {Resource_kind::SIDECAR,    "sidecar",    k_segment_capabilities},
    {Resource_kind::HELPER,     "helper",     k_segment_helpers},
    {Resource_kind::ASSET,      "asset",      k_segment_assets},
    {Resource_kind::NOTICE,     "notice",     k_segment_notices},
    {Resource_kind::PROVENANCE, "provenance", k_segment_provenance},
};

// A notice or provenance reference: canonical, below the right directory.
bool is_valid_record_reference(std::string_view path, const char* required_segment)
{
    return is_valid_package_relative_path(path) && path_first_segment(path) == required_segment;
}

// Parses one `files[]` entry; static checks only.
bool parse_entry(const Json_value& value, Index_entry& out_entry, Contract_failure* out_failure)
{
    const auto entry_fail = [out_failure](const char* reason_code, std::string detail) {
        fail(out_failure, reason_code, std::move(detail));
        return false;
    };

    if (!value.is_object()) {
        return entry_fail(reason::k_invalid_index, "files contains a non-object entry");
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(value, {"path", "bytes", "sha256", "resource"});
    if (unexpected.has_value()) {
        return entry_fail(reason::k_invalid_index, "unexpected member files[]." + *unexpected);
    }

    const std::string* path = strict::string_member(value, "path");
    if (path == nullptr) {
        return entry_fail(reason::k_invalid_index, "files[].path is missing or not a string");
    }
    if (!is_valid_package_relative_path(*path)) {
        return entry_fail(reason::k_path_escape, "files[].path is not a canonical package-relative path: " + *path);
    }
    out_entry.path = *path;

    const std::optional<std::int64_t> bytes = strict::integer_member(value, "bytes", 0, (std::int64_t)limits::k_max_file_bytes);
    if (!bytes.has_value()) {
        const Json_value* raw_bytes = value.find("bytes");
        if (raw_bytes != nullptr && raw_bytes->is_integer() && raw_bytes->as_integer() > (std::int64_t)limits::k_max_file_bytes) {
            return entry_fail(reason::k_custody_bound_exceeded, "k_max_file_bytes exceeded by " + *path);
        }
        return entry_fail(reason::k_invalid_index, "files[].bytes is missing or not a non-negative integer for " + *path);
    }
    out_entry.bytes = (std::uint64_t)*bytes;

    const std::string* sha256 = strict::string_member(value, "sha256");
    if (sha256 == nullptr || !is_valid_sha256_hex(*sha256)) {
        return entry_fail(reason::k_invalid_index, "files[].sha256 is not 64 lowercase hex characters for " + *path);
    }
    out_entry.sha256_hex = *sha256;

    const Json_value* resource = strict::member_of_type(value, "resource", Json_type::OBJECT);
    if (resource == nullptr) {
        return entry_fail(reason::k_invalid_index, "files[].resource is missing or not an object for " + *path);
    }
    const std::optional<std::string> unexpected_resource =
        strict::unexpected_member(*resource, {"kind", "origin", "notice", "provenance"});
    if (unexpected_resource.has_value()) {
        return entry_fail(reason::k_invalid_index, "unexpected member files[].resource." + *unexpected_resource);
    }

    const std::string* kind_name = strict::string_member(*resource, "kind");
    const std::optional<Resource_kind> kind = kind_name == nullptr ? std::nullopt : parse_resource_kind(*kind_name);
    if (!kind.has_value()) {
        return entry_fail(reason::k_invalid_index, "files[].resource.kind is missing or unknown for " + *path);
    }
    if (resource_kind_for_path(*path) != *kind) {
        return entry_fail(reason::k_invalid_index, "files[].resource.kind disagrees with the path directory for " + *path);
    }
    out_entry.kind = *kind;

    const std::string* origin_name = strict::string_member(*resource, "origin");
    const std::optional<Resource_origin> origin = origin_name == nullptr ? std::nullopt : parse_resource_origin(*origin_name);
    if (!origin.has_value()) {
        return entry_fail(reason::k_invalid_index, "files[].resource.origin is missing or unknown for " + *path);
    }
    out_entry.origin = *origin;

    const std::string* notice     = strict::string_member(*resource, "notice");
    const std::string* provenance = strict::string_member(*resource, "provenance");
    if (*origin == Resource_origin::PACKAGE) {
        if (resource->find("notice") != nullptr || resource->find("provenance") != nullptr) {
            return entry_fail(reason::k_invalid_index, "package-origin entry carries notice or provenance: " + *path);
        }
        return true;
    }

    if (notice == nullptr) {
        return entry_fail(reason::k_third_party_record_invalid, "third-party entry has no notice reference: " + *path);
    }
    if (provenance == nullptr) {
        return entry_fail(reason::k_third_party_record_invalid, "third-party entry has no provenance reference: " + *path);
    }
    if (!is_valid_record_reference(*notice, k_segment_notices)) {
        return entry_fail(reason::k_third_party_record_invalid, "notice reference escapes notices/ for " + *path + ": " + *notice);
    }
    if (!is_valid_record_reference(*provenance, k_segment_provenance)) {
        return entry_fail(reason::k_third_party_record_invalid,
            "provenance reference escapes provenance/ for " + *path + ": " + *provenance);
    }
    out_entry.notice     = *notice;
    out_entry.provenance = *provenance;
    return true;
}

// The referenced record must be indexed and non-empty.
bool check_record_reference(
    const Integrity_index& index,
    const Index_entry&     entry,
    const std::string&     reference,
    const char*            what,
    Contract_failure*      out_failure)
{
    const Index_entry* record = index.find(reference);
    if (record == nullptr) {
        fail(out_failure, reason::k_third_party_record_invalid,
            std::string(what) + " of " + entry.path + " is not indexed: " + reference);
        return false;
    }
    if (record->bytes == 0) {
        fail(out_failure, reason::k_third_party_record_invalid,
            std::string(what) + " of " + entry.path + " is empty: " + reference);
        return false;
    }
    return true;
}

} // namespace

const char* resource_kind_name(Resource_kind kind)
{
    for (const Kind_name& entry : k_kind_names) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "asset";
}

const char* resource_origin_name(Resource_origin origin)
{
    return origin == Resource_origin::THIRD_PARTY ? "third-party" : "package";
}

std::optional<Resource_kind> parse_resource_kind(std::string_view name)
{
    for (const Kind_name& entry : k_kind_names) {
        if (name == entry.name) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

std::optional<Resource_origin> parse_resource_origin(std::string_view name)
{
    if (name == "package")     { return Resource_origin::PACKAGE; }
    if (name == "third-party") { return Resource_origin::THIRD_PARTY; }
    return std::nullopt;
}

std::optional<Resource_kind> resource_kind_for_path(std::string_view path)
{
    const std::string_view segment = path_first_segment(path);
    for (const Kind_name& entry : k_kind_names) {
        if (segment == entry.segment) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

const Index_entry* Integrity_index::find(std::string_view path) const
{
    for (const Index_entry& entry : files) {
        if (entry.path == path) {
            return &entry;
        }
    }
    return nullptr;
}

std::uint64_t Integrity_index::total_bytes() const
{
    std::uint64_t total = 0;
    for (const Index_entry& entry : files) {
        total += entry.bytes;
    }
    return total;
}

std::optional<Integrity_index> parse_integrity_index(std::string_view bytes, Contract_failure* out_failure)
{
    if (bytes.size() > limits::k_max_index_bytes) {
        return fail(out_failure, reason::k_custody_bound_exceeded, "k_max_index_bytes exceeded by the integrity index");
    }
    if (!is_valid_utf8(bytes)) {
        return fail(out_failure, reason::k_invalid_utf8, "integrity index is not valid UTF-8");
    }

    Json_parse_limits parse_limits;
    parse_limits.max_bytes = limits::k_max_index_bytes;
    Json_parse_error parse_error;
    const std::optional<Json_value> document = parse_json(bytes, parse_limits, &parse_error);
    if (!document.has_value()) {
        return fail(out_failure, reason::k_invalid_index,
            "integrity index is not valid JSON at byte " + std::to_string(parse_error.offset) + ": " + parse_error.message);
    }
    if (!document->is_object()) {
        return fail(out_failure, reason::k_invalid_index, "integrity index is not a JSON object");
    }
    const std::optional<std::string> unexpected = strict::unexpected_member(*document, {"index_version", "files"});
    if (unexpected.has_value()) {
        return fail(out_failure, reason::k_invalid_index, "unexpected member " + *unexpected);
    }

    const Json_value* version = document->find("index_version");
    if (version == nullptr || !version->is_integer()) {
        return fail(out_failure, reason::k_invalid_index, "index_version is missing or not an integer");
    }
    if (version->as_integer() != k_index_version) {
        return fail(out_failure, reason::k_invalid_index,
            "index_version " + std::to_string(version->as_integer()) + " is not supported");
    }

    const Json_value* files = strict::member_of_type(*document, "files", Json_type::ARRAY);
    if (files == nullptr) {
        return fail(out_failure, reason::k_invalid_index, "files is missing or not an array");
    }
    if (files->as_array().size() > limits::k_max_files_per_package) {
        return fail(out_failure, reason::k_custody_bound_exceeded, "k_max_files_per_package exceeded by the integrity index");
    }

    Integrity_index index;
    index.index_version = k_index_version;
    index.files.reserve(files->as_array().size());
    for (const Json_value& value : files->as_array()) {
        Index_entry entry;
        if (!parse_entry(value, entry, out_failure)) {
            return std::nullopt;
        }
        index.files.push_back(std::move(entry));
    }

    std::set<std::string> folded_paths;
    struct Path_node
    {
        std::string spelling;
        bool directory;
    };
    std::map<std::string, Path_node> folded_nodes;
    for (std::size_t i = 0; i < index.files.size(); ++i) {
        const std::string& path = index.files[i].path;
        if (i > 0) {
            const std::string& previous = index.files[i - 1].path;
            if (path == previous) {
                return fail(out_failure, reason::k_invalid_index, "files lists a path twice: " + path);
            }
            if (path < previous) {
                return fail(out_failure, reason::k_invalid_index, "files is not sorted bytewise at " + path);
            }
        }
        if (!folded_paths.insert(ascii_lowercase(path)).second) {
            return fail(out_failure, reason::k_invalid_index,
                "files lists paths that collide under ASCII case-folding: " + path);
        }

        // A canonical file list must also have a canonical directory tree.
        // Full-path checks miss A/x + a/y and the file/directory pair A + a/x.
        // Validate the index before any platform attempts to materialize it.
        for (std::size_t end = path.find('/');; end = path.find('/', end + 1)) {
            const bool directory = end != std::string::npos;
            const std::string prefix = path.substr(0, end);
            const auto [node, inserted] = folded_nodes.emplace(
                ascii_lowercase(prefix), Path_node{prefix, directory});
            if (!inserted && (node->second.spelling != prefix ||
                              node->second.directory != directory)) {
                return fail(out_failure, reason::k_invalid_index,
                    "files lists conflicting directory prefixes under ASCII case-folding: " + prefix);
            }
            if (!directory) {
                break;
            }
        }
    }

    if (index.total_bytes() > limits::k_max_generation_bytes) {
        return fail(out_failure, reason::k_custody_bound_exceeded, "k_max_generation_bytes exceeded by the integrity index");
    }

    for (const Index_entry& entry : index.files) {
        if (entry.origin != Resource_origin::THIRD_PARTY) {
            continue;
        }
        if (!check_record_reference(index, entry, entry.notice, "notice", out_failure) ||
            !check_record_reference(index, entry, entry.provenance, "provenance", out_failure))
        {
            return std::nullopt;
        }
    }

    return index;
}

std::string serialize_integrity_index(const Integrity_index& index)
{
    std::vector<const Index_entry*> ordered;
    ordered.reserve(index.files.size());
    for (const Index_entry& entry : index.files) {
        ordered.push_back(&entry);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Index_entry* a, const Index_entry* b) {
        return a->path < b->path;
    });

    Json_value files = Json_value::array();
    for (const Index_entry* entry : ordered) {
        Json_value resource = Json_value::object();
        resource.set("kind",   Json_value(resource_kind_name(entry->kind)));
        resource.set("origin", Json_value(resource_origin_name(entry->origin)));
        if (entry->origin == Resource_origin::THIRD_PARTY) {
            resource.set("notice",     Json_value(entry->notice));
            resource.set("provenance", Json_value(entry->provenance));
        }

        Json_value file = Json_value::object();
        file.set("path",     Json_value(entry->path));
        file.set("bytes",    Json_value((std::int64_t)entry->bytes));
        file.set("sha256",   Json_value(entry->sha256_hex));
        file.set("resource", std::move(resource));
        files.push_back(std::move(file));
    }

    Json_value document = Json_value::object();
    document.set("index_version", Json_value((std::int64_t)index.index_version));
    document.set("files", std::move(files));
    return serialize_json(document);
}

} // namespace logonomic::cli_addon
