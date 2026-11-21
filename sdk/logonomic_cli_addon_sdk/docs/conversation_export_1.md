# conversation-export@1

`conversation-export@1` lets a provider read one native conversation and render
it as UTF-8 plain text, Markdown, or highlighted conversation text. It is independent of interactive resume:
exporting a readable conversation neither launches the CLI nor requires that
the CLI can resume it. Packages without this optional capability remain usable.

## Declaration

The sidecar `capabilities/conversation-export@1.json` contains:

```json
{"capability_id":"conversation-export","version":1,"payload":{"environment_names":[]}}
```

The adapter also declares the pair in its handshake. Version 1 has one
operation, `export`. The shared writer supports `text`, `markdown`, and
`conversation`; unsupported formats fail only the requested export.

`environment_names` uses the launcher-discovery declaration grammar and lists
the provider's source-location variables beyond the standard ambient baseline.
The host freezes only this allowlisted source view, including bootstrap
evidence, at launch. Providers declare paths needed to locate history; this
capability does not need credentials or arbitrary authored environment values.

These names are optional under version 1. Providers that prepare source facts
per launch can instead declare [conversation-export@2](conversation_export_2.md),
which requires every declared name before the host captures a source. Both
versions use the operation, request, result and rendering contracts below.

## Request

The request payload contains:

| Member | Meaning |
|---|---|
| `provider_session_id` | Exact native conversation identity; a nonempty UTF-8 string without control characters. |
| `resume_record` | Optional opaque provider record from the source conversation, using the existing resume-record object grammar and bound. It is a locator, not permission to resume. |
| `working_directory` | Source working directory, with the existing launch working-directory bound. May be empty for a historical record that has none. |
| `launch_environment` | Existing `Launch_environment` record: platform, frozen allowlisted effective source environment, and bootstrap evidence where available. |
| `output_directory` | Fresh, canonical absolute host-owned directory for this invocation. |
| `format` | `text`, `markdown`, or `conversation`. |
| `compact` | Optional Boolean; absent means `false`. When true, export discussion content selected by the provider. |
| `profile_storage` | Optional existing managed-profile context; the usual exact echo rule applies. |
| `account_context` | Optional existing source account context; the usual exact echo rule applies. |

The host selects source identity and context together. It does not substitute
the launcher's currently selected account for a historical source account.
Providers resolve their own native files within that context and verify the
requested native identity before rendering. A provider that cannot identify
the source reports an explicit unavailable result; it must not guess another
conversation or use terminal scrollback.

## Successful result

The provider creates exactly one export file below the allocated directory:
`conversation.txt` for `text`, `conversation.md` for `markdown`, or
`conversation.vnm_chat` for `conversation`. The result
contains only its descriptor:

```json
{
  "format":"markdown",
  "file_name":"conversation.md",
  "bytes":1200,
  "message_count":4,
  "attachment_count":1,
  "incomplete_tail_omitted":false
}
```

`bytes` and `message_count` are positive integers. `attachment_count` is a
nonnegative count of attachment markers, not a claim that attachment binary
data was copied. The result additionally echoes `profile_storage` and
`account_context` when present in the request. An empty or unreadable source
does not produce a successful empty export.

The result echoes `compact: true` for a compact export. Both request and result
omit this member when false, retaining full-text compatibility with older
strict readers. The host checks the requested and returned projection; a
provider that ignores compact cannot pass off a full export as compact. This
extension is local to `conversation-export@1` and changes no base CLI contract.

No conversation body crosses the capability protocol. Its frame and string
bounds therefore do not limit the size of an exported history. The host checks
the format, fixed filename, regular-file identity, byte count and UTF-8 before
opening an editable unsaved document. For editor extraction, the host owns the
temporary directory until the document has copied its contents or the operation
is abandoned, then removes it. Continue in instead extracts full Markdown
(`compact=false`) into a fresh subdirectory beside the continuation brief. Its
pending creation owns that directory until the new Session's lineage takes
ownership; failure or cancellation removes it after the provider settles, and
ordinary history pruning removes it with the brief. The recipient is not
launched unless the source export succeeds and both files have been staged.

## Native snapshots and rendering

Providers own native parsing, message order, branch selection, duplicate
suppression and tool interpretation. The shared `Conversation_writer` renders
selected `Conversation_message` values incrementally. It preserves participant
text, Unicode and code, labels roles, and distinguishes tool calls, tool results
and attachment references. Markdown retains participant-authored Markdown and
fences literal tool/attachment content with a delimiter longer than any
backtick run in that content. Plain text preserves participant-authored text,
including any Markdown they wrote. Export does not execute tools, fetch URLs
or read referenced attachment bytes. Missing binary content has an explicit
provider-supplied marker.

Control bytes below U+0020 other than tab, line feed, carriage return and form
feed are represented as visible `\u00xx` escapes. This retains their values
while producing a text document the editor can open; terminal ANSI escapes do
not execute and their content is not silently removed.

Providers mark each block's `compact_visible` field. Its conservative default
is true; known tool activity, reasoning, intermediate updates, and internal
material are false. User messages, final replies, and assistant text whose
phase cannot be established remain visible. Encrypted-message placeholders
are explicitly retained, one entry per source message, in both projections.
The writer does not infer encryption from placeholder text or filter by phase.
Messages with no retained blocks emit no header and do not count as exported
messages. Compact filtering never alters native history; exporting again with
compact disabled produces the full retained conversation.

### Highlighted conversation text

`.vnm_chat` begins with `#!vnm-chat 1` and a blank line. Each content block has
a colon-fenced record, in native message/block order:

```text
#!vnm-chat 1

::: user
Please explain the change.
:::

::: assistant final
The change preserves the original source.
:::

::: assistant /tool-call "shell call-1"
{"command":"git status"}
:::
```

An opening record contains its fence, one space, the role, optional `final` or
`commentary` phase, and optional `/text`, `/reasoning`, `/tool-call`,
`/tool-result`, or `/attachment` block kind. A block label, when present, is a
JSON string following the kind. Roles containing anything beyond ASCII letters,
hyphen, or underscore are JSON strings too. Text without a label omits `/text`.
Provider labels `Thinking` and `Reasoning` select `/reasoning`.

The fence has at least three colons and is longer than every colon run in that
block's body. A line containing exactly that fence closes the record. The
writer inserts one newline between the body and closing fence, then one blank
line between records; arbitrary body Markdown, code, and apparent headings
remain literal. The framework's `.vnm_chat` lexer uses the same fence state to
highlight roles, phases, tool blocks, reasoning, and attachments. Editing and
saving operate on this ordinary text buffer; no hidden alternate history is
stored in the document.

The shared JSONL reader opens the native file read-only and freezes its initial
byte length. It processes newline-terminated records within that prefix and
does not follow later appends. An unfinished final record is omitted and sets
`incomplete_tail_omitted`. Malformed complete records fail explicitly. Native
record byte, member and string allowances follow the actual record size; the
JSON parser's existing nesting-depth protection still applies. Provider logs
with other storage formats implement equivalent snapshot semantics themselves.

Both reader and writer honor cancellation. The writer stages a private partial
file and publishes the fixed filename only after a successful close; failure
or cancellation removes its partial file. Providers do not modify source logs.

The desktop chooses the extraction format before opening the generated buffer.
Saving writes the current edited buffer through the ordinary document Save/Save
As flow; it does not re-extract and lose edits. The suggested filename keeps
the selected format's extension in that flow.
