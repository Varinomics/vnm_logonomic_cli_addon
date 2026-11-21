# managed-configuration@2

Version 2 retains the bounded opaque transform request/result and adds two
required declaration members. `selector_policy` is `raw` (use a nonempty
selector verbatim) or `raw-tilde` (expand exactly `~`, `~/` and `~\\`, using the
frozen platform home and upstream path.join/path.resolve semantics).
`default_home_policy` is `canonical-home` (canonicalize the native platform home
when available, otherwise retain its spelling, then append the declared default
relative directory) or `home-or-temp` (platform home, falling back to the frozen
temporary directory). All roots come from the host's frozen launcher context.
No runtime adapter value nominates a file target.

Version 1 retains its trimmed selector and existing fallback semantics. Version
2 is selected only when declared, adopted and live; an older host cannot fall
back to interpreting its new selector as version 1. Unknown policies refuse the
capability. The transform bytes and opaque grant format remain unchanged.

For raw overrides, filesystem resolution precedes lexical `..` reduction so
symlink-containing paths preserve the native target. Final symlink preservation,
locking, snapshot bounds, generation checks and compare-and-swap remain in
force. A normal launch preparation requesting managed configuration also runs
before exact resume. Its configuration, environment and per-launch lifecycle
admission complete on the same frozen context and generation before the resume
proposal is accepted; no separate resume mutation API is introduced.

The two default policies preserve their runtime home rules. `canonical-home`
uses a nonempty platform HOME/USERPROFILE, otherwise the OS account profile.
`home-or-temp` preserves an explicitly empty HOME on Unix (and selects the
native temporary directory when storage needs a default). On Windows a present
USERPROFILE shorter than three UTF-8 bytes is an unavailable native home; an
absent value uses the OS profile. The OS fallback is captured independently of
the parent's environment, so an empty child assignment cannot reuse a parent's
custom home. Only these adopted policies consume the new frozen fields;
version 1's existing fallback remains unchanged.

The adopted runtime home oracles are [Rust home_dir](https://doc.rust-lang.org/std/env/fn.home_dir.html),
[Node 22 Unix home lookup](https://github.com/nodejs/node/blob/v22.x/deps/uv/src/unix/core.c),
[Node 22 Windows home lookup](https://github.com/nodejs/node/blob/v22.x/deps/uv/src/win/util.c),
and [Node temporary-directory selection](https://github.com/nodejs/node/blob/v22.x/lib/os.js).
