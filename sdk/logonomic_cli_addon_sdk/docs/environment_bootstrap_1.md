# environment-bootstrap@1

The static declaration names one to four environment keys that the host may
load from the provider's profile `.env` files before native configuration is
resolved. Its object has exactly `environment_names`, `home_environment_name`,
`default_home_relative_path`, and `dotenv_relative_path`. Names are unique,
non-reserved environment names; the home selector is one of them. Both paths
are nonempty bounded package-relative paths.

The host preserves existing values, including empty values. It adds only a
nonempty declared value from a parsed dotenv file where the key is absent.
The package receives no dotenv bytes or paths.

Dependent requests carry `launch_environment.bootstrap: {version: 1, resolved_home: ...}` only after
a declared, live bootstrap has successfully frozen that request. A package which
requires this bootstrap must refuse dependent operations when that optional
record is absent; older hosts therefore cannot silently resolve the wrong
profile. Unknown record versions are refused locally. The record contains the host-derived absolute profile directory for identity
resolution, without granting managed-file write authority and leaves the common protocol version unchanged.
The host propagates only declared bootstrap values to the native child, and
rejects a preparation that changes their presence or values. It also preserves
the native platform-home and temporary-directory inputs when profile selection
depends on them. Bootstrap-backed process proposals retain the session workspace
so relative bootstrap paths keep their meaning. Other dotenv/authentication keys remain
with the vendor's later environment loader.

The profile resolver uses the `home-or-temp` native semantics documented by
managed-configuration@2. An empty Unix HOME remains empty, an absent platform
home uses an OS account lookup independent of parent environment, and invalid
Windows USERPROFILE is unavailable when a platform home is needed. A relative
temporary directory is interpreted under the frozen native working directory.
The proof carries the absolute selected profile; it never changes absence of
native environment keys.
