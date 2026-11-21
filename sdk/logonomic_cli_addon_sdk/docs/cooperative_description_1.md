# cooperative-description@1

This optional capability returns an explicit model-authored session title or
working summary to the host. It uses authenticated helper ingress and its
existing idempotency journal. Its sidecar payload is `{}`. It requires a live
helper-ingress pair and an already admitted native activation; it introduces no
new helper protocol version or vendor-specific host action.

The host keeps its existing title precedence, summary normalization, review,
word-count and persistence policies. Description submission is independent of
turn completion, attention and success. It cannot veto or complete a turn.

## Negotiation and submission

After capability intersection, a classify request includes
`cooperative_description: {version: 1}`. The SDK exposes this through
`Classify_event_request.cooperative_description_version`. Absence means no
support. A malformed optional request records `description_failure` without
invalidating ordinary lifecycle classification.

The adapter can return an explicit submission only on a separate `ignored`
classification with no lifecycle observations:

```json
{
  "classification": "ignored",
  "observations": [],
  "cooperative_description": {
    "version": 1,
    "submission": {
      "provider_session_id": "native-session",
      "transition_nonce": "0123456789abcdef0123456789abcdef",
      "request_id": "native-tool-call",
      "kind": "summary",
      "summary": "## Working state\n- Implemented the integration.\n",
      "title": "Integration review"
    }
  }
}
```

`title` requires a title and forbids summary text. `summary` requires summary
text and permits an optional revised title. `summary_unchanged` explicitly
reviews the existing summary, forbids replacement summary text, and permits
an optional revised title. Keeping a summary that does not exist remains a host
policy refusal. Omitted text is never inferred from conversation history.

The provider session ID and request ID are nonempty valid UTF-8, without control
characters, and at most 256 bytes. The nonce uses the existing 32-character
lowercase hexadecimal activation grammar. Title transport is bounded to 1 KiB;
summary transport to 16 KiB, with ordinary line breaks and tabs allowed. The
whole encoded submission is at most 24 KiB. These are transport bounds; the
host's existing title and summary policies can refuse otherwise valid text.
Every record has closed members.

`Description_submission` and its codec provide the shared schema. The optional
result can instead carry `failure: {reason_code, detail}`. A malformed action or
one attached to a lifecycle transition records an isolated description failure;
it neither mutates host descriptions nor invalidates valid lifecycle facts.

## Host authority and receipt

The host verifies capability negotiation, the live package generation, the
native process generation, provider session ID, activation nonce and epoch
before applying an action. It retains that scope through the existing policy's
reentrant publication boundaries. A clear that preserves the native session ID
still creates a different activation. Retired submissions cannot acquire it.

The adapter does not produce a successful description receipt. The host writes
the following reserved `helper_response.cooperative_description` object after
the existing policy returns:

```json
{"version": 1, "request_id": "native-tool-call", "accepted": true, "reason": ""}
```

Refusal uses `accepted: false` and a nonempty SDK reason code, such as
`stale_activation`, `already_assigned`, `no_recorded_summary`, or
`summary_unavailable`. For a summary with an optional revised title,
`summary_recorded_title_unavailable` means the combined request failed but
the summary was saved. The provider should report which field was saved.
The `Description_receipt` codec rejects unrelated policy
members. The event carrier's `accepted` flag alone says nothing about whether
the host recorded a title or summary.

The provider captures native invocation and activation identity before any
await. It uses the same journal identity for a transport retry, preserves an
unknown submission outcome, and does not retry automatically with a new key.
Acknowledgment releases the committed response under the existing journal
rules. Cancellation still terminates and reaps the helper's owned wrapper; it
does not revoke a host action that already committed.

Providers must preserve native tool availability and approval policy. A model
choosing not to submit a description does not block terminal lifecycle evidence.
