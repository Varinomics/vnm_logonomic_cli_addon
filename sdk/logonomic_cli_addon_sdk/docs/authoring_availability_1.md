# authoring-availability@1

A provider can report whether a particular launcher and account meet its local
configuration requirements for workflow authoring. For example, two accounts
can differ because one profile runs native hooks. A host may use this check
when listing or selecting accounts. Hosts and packages negotiate the optional
capability independently of account discovery and session preparation.

## Declaration

```json
{"capability_id":"authoring-availability","version":1,"payload":{"environment_names":[]}}
```

Use the same environment-name declaration as launcher discovery. The host
includes only the baseline and the environment names declared for this check.

## Check

The request uses the launcher, launch_environment, default_home_fallback and
normalization_directory fields from the [accounts launcher request](accounts_2.md).
When the host selected an account, it also supplies the exact `account_context`
as a top-level member. The normalization directory resolves relative settings
paths and does not grant workspace access.

The adapter may inspect only the selected launcher's configuration and local
profile metadata needed to assess the configuration. It must not launch a
process, run hooks, send model requests, contact a service, or modify state.
An unreadable or ambiguous configuration cannot establish eligibility.

A successful check returns exactly `available` (boolean), the exact
`account_context` when supplied, and `reason` when unavailable. The reason
is a single line of plain UTF-8 text up to 1024 bytes and must contain a
non-space character. Describe the setting that prevents authoring without
returning its contents or credentials.

```json
{"available":false,"reason":"Hooks are enabled for this profile.","account_context":{"version":1,"adapter_account_key":"primary"}}
```

The host validates the result against the grant sent with the corresponding
request. An eligible result clears this static check. The normal session
capability and its preparation still establish usable native authoring.
Packages that omit the capability retain their existing preparation behavior.
