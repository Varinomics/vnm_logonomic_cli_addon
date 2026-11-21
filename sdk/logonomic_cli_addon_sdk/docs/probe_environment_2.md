# Account probe and usage environment contracts

`auth-status@2`, `quota@2`, and `usage-scan@3` each own their
`environment_names` declaration. None depends on another probe or on
structured-session. Auth and quota require the array, using the launcher-discovery
declaration grammar. Usage accepts that declaration or `{}` for an empty
additional-name list. The host decodes the selected current sidecar directly;
another capability's declaration supplies no authority for this projection.

Requests carry the required frozen `launch_environment` from the session context
grammar. It preserves the selected launcher's expanded profile and executable
environment, restricted to the discovery baseline, the capability's declared
names, and admitted explicitly authored launcher names. Absent names stay absent.
The adapter uses this record rather than its supervisor's ambient environment.
Windows name aliases are compared case-insensitively and cannot coexist.
Unknown members, malformed platforms, reserved authority names and values outside
the environment bounds fail decoding.

The host captures this context once for an operation. Usage pagination keeps
the same account, generation, selection and environment throughout the scan.
A later ambient change is visible only through a new explicit host capture.

Auth and quota preparation use the current chunked `Structured_prepare_result`
grammar, including optional persistent stdin. Their inspection request uses
the chunked structured inspection facts. Their observation responses retain
the independently bounded plain `protocol_state` string. See
[auth-status@2](auth_status_2.md) and [quota@2](quota_2.md) for their distinct
completion, optional input and observation rules.

[account-refresh@1](account_refresh_1.md) reuses these current frozen and chunked
probe requests. Its capability version remains 1. Shared record grammar reuse
does not require adopting a structured-session capability.
