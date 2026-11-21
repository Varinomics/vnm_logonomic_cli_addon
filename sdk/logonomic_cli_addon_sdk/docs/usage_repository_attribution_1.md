# usage-repository-attribution@1

This optional capability authorizes a repository-only correction to an accepted
usage record. Its probe declaration is an empty object. `scan` uses the unchanged
`usage-scan@3` request and page grammars. `incremental` uses the unchanged
`usage-incremental@1` request and result grammars. Both require live
`usage-scan@3`; `incremental` additionally requires live `usage-incremental@1`.
Profile selection, account acknowledgement, candidate sealing, continuation,
cancellation and atomic cache/history publication retain those contracts.

A freshly corrected record carries
`capabilities["usage-repository-attribution"] = {"version": 1}`. Authority exists
only on these operations, never on ordinary usage scan operations. The provider
must bind continuation and candidate selection to the chosen operation so an
ordinary scan cannot resume a correction candidate. A recognized malformed tag
rejects the candidate. An unsupported tag version establishes no authority.
Absence does not affect unrelated capabilities or ordinary numeric updates.

The host compares against its own accepted record, which may contain attribution
retained from an earlier observation and differ from the selected provider cache.
Identity, aggregation, completed date, total, token categories and coverage, source
path, model, counter field and counter semantics must remain identical. The
recognized explicit repository partition must cover exactly the accepted total.
Only the repository-preservation guard for a partial cumulative record may be
relaxed. Delta-event immutability and numeric aggregate evidence rules remain in
force. The host does not resolve filesystem paths to certify this correction.

The provider must have a live native source witness for the record's repository,
independent of cached attribution or a session-level fallback. Missing or
unresolved evidence preserves accepted attribution. A matching host-selected native
import seed is an accepted observation: preserve its accounting and provenance
before applying the witnessed correction. A numeric update must use the ordinary
accounting rules without a correction tag; a later confirming observation may
correct its repository. No transcript recount or cache-format migration is required
to adopt the capability.

The tag identifies a correction in this candidate, not permanent authority. The
host strips recognized tags before shared persistence. Providers must not replay
stale tags on later numeric changes or downgrade continuations to ordinary scans.
Temporal observations retain their independent validation and parent bindings;
repository changes must not double-count an archived temporal parent or expose its
old repository as the current allocation.
