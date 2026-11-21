# lifecycle-correlation-lifetime@1

`lifecycle-correlation-lifetime@1` is an optional live RPC capability. It
keeps a package's authenticated helper context only while the host owns the
matching lifecycle correlation. It is live only with `lifecycle-events@2` and
a host-known live `helper-ingress` version (`@1`, `@2`, or `@3`) in the same
generation.

`reserve` is sent before classification and contains exactly:

```json
{"transition_nonce":"0123456789abcdef0123456789abcdef"}
```

The nonce has the existing `Session_correlation` grammar. The successful
result is exactly `{}`. Repeating `reserve` for an active nonce acknowledges
the same reservation and must not replace its frozen helper context. A package
must not classify a nonce without an active reservation.

`retire` removes the package's nonce maps, path state, and held watch
atomically. Its successful result is exactly `{}`. It is one of:

```json
{"transition_nonce":"0123456789abcdef0123456789abcdef","disposition":"abandoned"}
{"transition_nonce":"0123456789abcdef0123456789abcdef","disposition":"terminal-consumed","consumed_cursor":"opaque cursor"}
```

`consumed_cursor` is absent for `abandoned`. It is required for
`terminal-consumed`, follows the existing opaque observe-cursor grammar, and
must equal the held final cursor. A closed final page remains replayable until
that exact terminal-consumed retirement. Repeating a successful retirement is
an empty-success no-op; packages need no TTL or tombstone to retain it.

With this opt-in, a correlation begins only through a helper-admitted
transition after reserve acknowledgement. A polled START cannot mint an
unreserved replacement correlation and is explicitly refused. Child facts
and the terminal END remain valid poll observations. END must be on the final
page (`more: false`); after that page commits, the host retires using its exact
returned consumption cursor. A watch-window completion alone never retires.
