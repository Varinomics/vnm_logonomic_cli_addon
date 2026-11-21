# attention-kind@1

This optional helper-ingress companion uses the existing capability-pair
invocation mechanism and operation `classify_event`. It depends on the package's
existing helper-ingress and lifecycle-events declarations. It does not replace
or change either base grammar. Select this pair only when declared live;
otherwise invoke helper-ingress as before. Providers share the classification
producer, not a second event transport.

The request is the existing classify-event request plus optional
`pending_conversational_request_id`. The host supplies only its current pending
conversational hint identity for the admitted session/epoch. It is not an
arbitrary attention ID and must not identify an interactive or permission request.

The reply contains the base classification plus optional
`attention_kind:{version:1,observation_index:0,kind:"conversational_hint"}`.
The other kinds are `interactive_question`, `permission`, and the internal
`interactive_question_candidate` and `permission_candidate`. The index selects
an existing attention observation; its evidence and request identity remain
authoritative. Conversational hints require inferred evidence and a nonempty
request identity. Native event names and IDs are never rewritten as kinds.

An interactive question candidate also requires inferred evidence and a
nonempty request identity. It is retained correlation state, not a visible
question or permission indicator. It must not hide other pending attention.
An ordinary inferred `interactive_question` remains a visible question; it is
not implicitly a candidate. Host-terminal corroboration may present the same
candidate ID as an inferred interactive question only after validating the
candidate's generation, epoch, and subsequent title sequence. Withdrawal of
that corroboration is not an answer and does not resolve the candidate.

A permission candidate requires inferred evidence and a nonempty identity. It
remains visible from the permission hook while native status is unavailable.
Authenticated native Action Required presentation can establish its pending
state; a subsequent positive native progress observation can withdraw that
inferred attention without asserting a user answer, turn completion or success.
During a working turn, missing and unrecognized titles do not settle permission
attention. Queued approvals may reuse the retained current presentation. A new hook delivery
refreshes its scope so a late result cannot settle the new request. The host
retains the candidate without an answer or tombstone when presentation resumes.
Native presentation is an aggregate level, not a per-hook approval outcome:
a later Action Required observation can re-arm a retained inactive candidate.
Root completion expires the hook inference unless the current authenticated
title still corroborates Action Required; classification of a later title
rechecks that fallback against the completed root phase. This also covers
titles without progress animation. Existing permission presentation stays
provisionally pending while a newer title is being classified. The next root
turn, interruption or session termination prunes the retained correlation. Inactive permission correlation
does not block attention, native wake or completed-result readiness. An
uncorroborated interactive question retains its existing pending semantics.

Only the companion decoder consumes this envelope. Missing, malformed, or
unknown kind metadata is unspecified and leaves valid base observations usable.
Invalid base observations still fail. An older peer never receives the envelope:
the provider emits it only in response to the explicitly selected companion pair.

### Bounded assistant reply evidence

`assistant_reply_evidence(complete_text)` scans a complete attributed reply and
returns null or a provider-local object with exactly these fields:

```json
{"scope":"final_paragraph_edges","context":"prose","prefix":"Which target?","suffix":"Which target?"}
```

The prefix and suffix are each at most 128 UTF-8 bytes, cut only at scalar
boundaries. They are edges of the final nonempty paragraph when it is prose, not
a complete message or a claim that an omitted middle was observed by the classifier. The
producer scans the whole reply before extracting them, so long earlier prose or
a long final paragraph does not hide a closing question. Earlier paragraphs are
not forwarded. Empty, invalid UTF-8, NUL-containing, quoted or code evidence is
omitted. Normal completion and native session/turn attribution remain the
provider's responsibility; partial, interrupted or unattributed output cannot
be made complete by this representation.

The context scan recognizes blank lines (including CRLF and whitespace-only
lines), backtick and tilde fences with their opening widths, leading blockquotes
and their lazy continuations, and indented code at paragraph boundaries. A
leading double quote or backtick also excludes the paragraph. It retains fence
and quote context before bounding text; a suffix of a code sample is never
relabelled as a whole prose reply. This conservative scan is not a full Markdown
renderer. Other Markdown containers and arbitrary language semantics are not
inferred.

`looks_like_reply_request_evidence` validates the explicit scope, context and
edge bounds. It applies the existing best-effort grammar: final `?` or `？`, or
the English prefixes `please tell me `, `please provide `, and
`let me know which ` (ASCII case-insensitive). Outer Markdown emphasis is ignored
within the retained edges. The complete-text `looks_like_reply_request` entry
point delegates to the same extraction and predicate. These are local text
hints, not semantic certainty; they use no model, external request or transcript
upload. Missing or excluded evidence yields no hint.

Providers can carry this object as `assistant_reply_evidence` in their existing
sanitized helper feed without changing `attention-kind@1`. A matched hint still
uses inferred evidence and a stable identity derived from event evidence and
the epoch/transition. Root user submission resolves only the pending
conversational ID supplied by the host. Native questions and permissions retain
their own pending request identities and evidence.
