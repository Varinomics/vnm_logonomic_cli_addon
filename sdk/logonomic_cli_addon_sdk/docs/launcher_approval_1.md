# launcher-approval@1

This optional capability edits invocation-level approval arguments in saved
launcher configurations. The provider owns argument meanings, aliases,
conflicts and placement. The host owns the bulk action, persistence and result
presentation. Neither side edits provider configuration through this capability.

The sidecar is `capabilities/launcher-approval@1.json`:

```json
{"capability_id":"launcher-approval","version":1,"payload":{}}
```

The payload is an empty object. Absence, an unknown version or an invalid
declaration makes this capability unavailable without preventing unrelated
capabilities from working. The bootstrap and common launch schemas are unchanged.

## Operations

Both operations receive the raw saved launcher tuple, using the existing
`Launcher_request` grammar and bounds, plus the platform identifier. The
`script_body` is not runtime-prepared argv. Environment text is passed through
for identity and preservation; it is not a source of approval policy to rewrite.
Objects have closed member sets.

### transform

```json
{
  "launcher": {
    "script_body": "work-cli --model fast",
    "runner_key": "direct",
    "environment_text": "WORK_PROFILE=personal"
  },
  "platform": "linux-x86_64",
  "action": "yolofy"
}
```

`action` is `yolofy` or `unyolofy`. Yolofy places the provider's canonical bypass
argument exactly once and replaces only understood equivalent or conflicting
approval arguments. Unyolofy removes or disables recognized invocation-level bypass forms.
It does not insert a default mode, restore a removed mode or promise that the
remaining configuration requires approval. Both actions are idempotent.
When removing an approval option would change ownership of a following operand,
the provider may retain an explicit inactive spelling only when its native
meaning is equivalent to omission, including configured approval policy.

Successful results have one of these shapes:

```json
{"disposition":"changed","script_body":"work-cli --approve-all --model fast"}
{"disposition":"unchanged","script_body":"work-cli --approve-all --model fast"}
{"disposition":"unsupported"}
```

`changed` returns a replacement script; `unchanged` returns the exact input
script. Both scripts satisfy the existing script bounds. `unsupported` has no
script and means the provider cannot safely interpret or edit this invocation.
Provider failures use the normal invocation error result. An unsupported
invocation is not a successful transformation.

The host validates the disposition against the original script and keeps the
runner, environment and other saved launcher fields. A stale asynchronous
result must not overwrite a launcher edited after the request began.

### comparison_key

The request has `launcher` and `platform` as above, without `action`:

```json
{"supported":true,"key":"opaque-provider-key"}
{"supported":false}
```

A supported key is nonempty UTF-8, contains no NUL or control characters and is
at most 256 bytes. An unsupported result has no key. Keys are opaque and compared
only within the same provider and capability generation. Hosts do not persist
them as launcher identity or interpret their contents.

Equal keys mean the invocations differ only in approval arguments the provider
understands. The executable, runner, platform, environment/profile selection,
non-approval arguments and option values remain significant. This permits an
additive rescan to recognize an existing launcher after an explicit approval
action without merging unrelated customized launchers. A missing capability
or unsupported comparison provides no such equivalence.

## SDK implementation helpers

`launcher_approval.h` supplies codecs, declaration validation, pure editing and
comparison helpers, and `handle_launcher_approval` for adapter dispatch.
Providers supply `Launcher_approval_rules`; the SDK contains no provider table.
Rules declare literal program basenames (including accepted executable
extensions), the canonical bypass argument, aliases, known approval modes,
conflicting flags and unrelated option arities. The canonical argument may be
an exact attached spelling such as `--approve-all=true`. Modes distinguish bypass values
from other understood values: Yolofy replaces both, while Unyolofy removes only
bypass values. A mode with no bypass values is preserved by Unyolofy.

The shared editor accepts one bounded literal invocation in `direct`, `bash`,
Windows `cmd` or Windows PowerShell syntax. It checks executable basenames
case-insensitively on Windows and exactly elsewhere. Paths remain significant
in the comparison. PowerShell supports the literal `&` call operator for quoted
executable paths and doubled single quotes inside single-quoted strings.
PowerShell tokens containing decoded literal double quotes are unsupported:
Windows PowerShell's native argument passing can turn those quotes into new
argument boundaries. Scripts containing PowerShell's typographic quote
delimiters are also unsupported; the editor never treats words inside them as
separate arguments. A quote-leading PowerShell word immediately followed by
more non-whitespace text, such as `'first'second`, is unsupported because the
native shell creates separate arguments there. Fully quoted words and doubled
single quotes within them retain their normal support.
These boundaries follow PowerShell's
[quoting rules](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_quoting_rules)
and its [quote character definitions](https://github.com/PowerShell/PowerShell/blob/master/src/System.Management.Automation/engine/parser/CharTraits.cs).
Direct quoting follows the launcher's literal splitter. Direct and PowerShell
scripts containing non-ASCII Unicode whitespace are unsupported, so their
native separators cannot be mistaken for part of an option value.
The editor retains source spans rather than rebuilding a command from
inspection tokens, so unrelated argument spelling and value quoting survive.
Besides approval spans and their adjacent separator whitespace, it may normalize
the option/value join of a declared variadic argument to preserve operand ownership.

By default, mandatory-value options protect their following token and flags
consume no value. Providers whose native parser differs declare
`value_arguments_consume_options=false` and/or
`flag_arguments_consume_booleans=true`. For those rules, a separate dash-prefixed
mandatory operand or a literal `true`/`false` following a flag, respectively,
is unsupported. Editing also refuses a result that would newly expose such
an operand. These rules keep native parser ambiguities outside the bounded
editor; they do not implement another general option parser.

Optional-value options consume a following non-option token. Attached `--option=value` is supported
for declared value options. Edits preserve the ordered non-approval options,
their values and positional operands. An edit that would make an optional-value
option capture a previously positional word is unsupported. Tokens after `--`
remain operands. The first
positional token can name a provider-declared session command, placing the
canonical flag after it, or a blocked management command; other positionals
are prompt or session operands. Providers must declare only session commands
whose approval argument placement they understand and list their management
commands as unsupported. Unknown option arity or approval values, undeclared attached
values on boolean flags, missing values, empty arguments, malformed quoting,
multiple commands, shell expansion, pipelines, redirection and wrappers return
unsupported. The helper does not inspect script files or execute a CLI.

Comparison uses the same parser and hashes a canonical representation after
omitting the declared approval arguments. Settings/configuration payloads,
allowlists, deny rules, hooks and other unrelated values remain part of that
representation and remain unchanged by both actions. Providers with different
invocation semantics may implement the wire contract directly instead of
using the shared helper.

`variadic_arguments` declares repeated options whose values append in order.
Each `Launcher_approval_variadic_argument` names one spelling, whether it
requires at least one value, whether an attached value terminates that
occurrence, and whether empty values are permitted. `value_separator` declares
a native separator when empty components must also be rejected. Short declared
options support attached forms such as `-ivalue` and `-i=value`. Otherwise,
values continue until an option or `--`; a lone `-` remains a value. A separate
required first option-looking value is unsupported, including a native-consumed
`--`: native consumers can still act on its raw spelling independently of the
option parser. An already-attached option-looking value remains distinct.
PowerShell short attachments require a fully quoted token or fully quoted
attached value. Bare forms such as `-i=shot.png` and `-ishot.png` are unsupported:
PowerShell can split their punctuation into another native argument.

When an approval edit would expose a positional or session command to a
preceding variadic option, an attached-terminating option isolates its final
value. For example, `--image one.png two.png --approve-all prompt` can become
`--image one.png --image=two.png prompt`. The raw final value and its quoting
remain intact. A single value instead replaces the option-and-whitespace span
with the declared literal option name and `=`, so quoted option names do not
produce a different native argument boundary.
For a PowerShell short option, the editor instead quotes the complete attached
token, doubling literal apostrophes. The declared Windows runner is
`powershell.exe`: its native argument marshalling halves an even trailing
backslash run in a word containing space or tab. The shared parser projects
that native value before interpreting option roles, and generated short
attachments double that run before shell quoting. Long attachments retain the
original raw operand. Trailing backslashes without whitespace stay literal.
An odd trailing run with whitespace merges native arguments and is unsupported
for both edits and comparison, including unchanged edits. The complete decoded
word `--%` is also unsupported, even when quoted, because it controls native
parsing; attached or embedded data such as `-i='--%'` remains literal.

For a still-greedy option, Yolofy places its canonical argument at the needed
boundary. Unyolofy uses `inactive_bypass_argument` there when the provider has
declared an exact omission-equivalent native spelling. It does not insert `--`,
rewrite a positional as a prompt option, or choose a default permission mode.
If no declared spelling preserves the boundary, the edit is unsupported.

Both the final edit check and the comparison key use ordered operand-ownership
records, distinguishing absent from empty values. Safe variadic attachment and
repetition normalize to the same records; scalar and optional options retain
their attached-versus-separate distinction. Explicit `--` remains significant.
Thus a prompt is never equivalent to the same text consumed as an option value.
