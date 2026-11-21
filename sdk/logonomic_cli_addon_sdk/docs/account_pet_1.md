# account-pet@1

`account-pet@1` supplies an optional local pet for an account's launcher badge.
Its declaration payload is `{}`. It requires live `accounts@2`; absence or an
unsupported version affects only the badge.

The `inspect` operation receives `account_context` version 2, as resolved by
`accounts@2`. The optional absolute `normalization_directory` and
`home_directory` describe the launcher's frozen working directory and effective
`HOME` when its provider needs them to resolve relative or home-relative pet
selectors. The adapter must not substitute its own process environment.

The result echoes `account_context` exactly and contains:

- `pet`: `null`, or an object with `spritesheet_path`, `frame_width`,
  `frame_height`, `columns`, `rows`, `frames`, and `frame_durations_ms`.
- `watch_paths`: up to 32 absolute paths whose creation, replacement, removal,
  or modification can change the result, including when no pet is selected.
  These name exact file/existence inputs; directory contents are not implicit.
- Optional `watch_discoveries`: up to eight filtered file-discovery inputs,
  each containing an absolute `directory`, a `relative_directory_pattern`,
  a literal nonempty `name_prefix`, and one through eight literal `suffixes`.
  The relative pattern contains one through four slash-separated filename-glob
  segments, each at most 256 UTF-8 bytes, with no empty, `.` or `..` segment
  (1024 bytes total). Prefixes are at most 256 bytes; suffixes are nonempty and
  at most 32 bytes. Prefixes/suffixes contain no path separator or NUL. The host
  observes matching regular-file metadata and reconciles missing/new matching
  directories without treating unrelated directory activity as a pet change.
- Optional `unavailable_reason`: a nonempty UTF-8 diagnostic, at most 4096
  bytes, when a configured pet cannot be read. It requires `pet: null` and
  retains the observation paths so repairing the files can restore the badge.

`spritesheet_path` names a local PNG or WebP. The grid must cover the image
exactly, have at most 256 frames, and contain at most 16,777,216 pixels. Frame
width and height are each at most 4096 pixels. `frames` contains 1 through 256
zero-based row-major grid indices. `frame_durations_ms` has the same number of
entries, each between 17 and 2,147,483,647 milliseconds. The first listed frame
is the still badge; the listed sequence loops while its launcher is hovered.
The host reads at most 20 MiB of encoded image data and validates the format
and dimensions before publishing the badge.

`pet: null` without an unavailable reason means no pet is selected or pets are
disabled. A missing or malformed configured asset is unavailable, rather than
an absent selection. Providers own their native selection and asset formats;
the host does not inspect provider configuration or write to it. The native
asset remains local to its account, and the host publishes only the badge
frames needed by the desktop.
