# async-question-attention@1

This optional companion depends on live `attention-kind@1` and uses its
`classify_event` request unchanged. Select it only when declared live. Its reply
is the attention-kind reply plus an optional independently versioned record:

```
async_questions: {version: 1, observation_index: 0,
                  kind: "accepted", question_ids: ["opaque native identity"]}
```

`started` and `accepted` select a root interactive-question candidate observation.
The host retains one bounded identity set per native call on its existing
attention request. `started` is nonpresenting and records the native generation,
epoch and title sequence before the tool executes. Successful native acceptance
promotes that same request; it does not replace its earlier presentation proof
or imply an answer. Failed calls never promote the request. Accepted requests
use the existing generic interactive-question presentation while ordinary work
continues.

`answered` selects a root Working observation and carries only exact native reply
identities. The host subtracts matching questions; a partial answer leaves the
others pending. A native legacy whole-message identity may settle that call's
set. Unrelated prompts, tools and Working transitions do not resolve questions.
`reset` selects a native interrupted/completed-phase or session-end observation
and carries an empty identity array. Producers must emit it only for actual
native reset semantics; a completion attempt alone is insufficient.

The existing authenticated native presentation lane can reconcile residual local
skips at aggregate indicator scope. Evidence predating the call's `started`
admission cannot present or retire that call. A later Action Required observation
may arrive before successful acceptance and remains usable after promotion.
Only accepted requests with such presentation proof can be retired by subsequent
explicit native progress, or by newer native presentation after a completed-phase
hint. Missing or unrecognized titles during work do not resolve them. This does
not claim a skipped question was answered or expose native responder controls.

At most 128 unique identities of 512 UTF-8 bytes each are accepted per record.
Prompts and answers are never part of this record. Correlation is local to the
host's existing bounded attention state and resets with its session/epoch.
Missing, malformed or unknown optional record metadata leaves valid base
observations usable. Neither the lifecycle schema nor attention-kind@1 changes.
