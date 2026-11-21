#pragma once

#include "logonomic_cli_addon/bootstrap.h"
#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/integrity_index.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sidecar.h"
#include "logonomic_cli_addon/version.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace logonomic::cli_addon {

/**
 * @brief The host facts a package is validated against: the platform whose
 * adapter must exist and the core protocol generations the host speaks.
 */
struct Validation_options
{
    std::string                platform_id = host_platform_id();
    std::vector<std::uint16_t> host_core_protocol_generations = {k_core_protocol_generation};
};

/**
 * @brief A package that passed every check, with the exact bytes the checks
 * were made on. The index carries the resource classification of every
 * file; the sidecars are every valid capability declaration, whether or not
 * a host adopts them.
 */
struct Validated_package
{
    Bootstrap_manifest              bootstrap;
    std::string                     bootstrap_bytes;
    Integrity_index                 index;
    std::string                     index_bytes;
    std::string                     adapter_relative_path;    // for the validated platform
    std::vector<Capability_sidecar> sidecars;                 // index order
    std::string                     generation_digest_hex;    // over the bytes as read from the source
    std::uint64_t                   total_bytes = 0;          // sum of index bytes

    std::vector<Capability_pair> declared_capabilities() const;
};

/**
 * @brief Validates a package directory against the package contract in one
 * fixed order: bootstrap, platform and core-generation membership, index,
 * adapter presence and executable format, third-party records, directory
 * listing (unindexed, missing and link entries), every file's size and
 * digest, sidecars, then the generation digest. The first failure is
 * reported with its reason code and the entry it names.
 */
class Package_validator
{
public:
    explicit Package_validator(Validation_options options);

    const Validation_options& options() const { return m_options; }

    /// Bounded read and strict parse of the bootstrap alone, which is what
    /// discovery needs to group candidates by package ID before validating
    /// the selected one.
    std::optional<Bootstrap_manifest> read_bootstrap(
        const Package_source& source,
        Contract_failure*     out_failure) const;

    std::optional<Validated_package> validate(
        const Package_source& source,
        Contract_failure*     out_failure) const;

private:
    Validation_options m_options;
};

/// True when the bytes begin with the ELF magic; the Linux half of the
/// shell-entry-point rule.
bool has_elf_magic(std::string_view bytes);

/// A native ARM64 Mach-O executable; shared libraries and other architectures
/// cannot act as a macos-arm64 adapter.
bool is_macos_arm64_executable(std::string_view bytes);

} // namespace logonomic::cli_addon
