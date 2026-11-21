# Prebuilt package artifact staging

`logonomic_cli_addon_stage` assembles an offline payload from an explicit
artifact list. It has no provider roster, network access, package build step,
adapter execution, or active-installation update protocol.

```text
logonomic_cli_addon_stage --output <fresh-directory>
    [--input <artifact-set.json>] [--evidence <receipt.json>] [--platform <id>]
logonomic_cli_addon_stage --verify <directory> --evidence <receipt.json>
    [--platform <id>]
```

The platform defaults to the tool's host platform. Validation uses the SDK's
core protocol generations. An unsupported optional capability version stays
in its package unchanged; it does not reject unrelated capabilities or the
artifact set.

## Artifact list

The input is strict UTF-8 JSON with exactly these fields:

```json
{
  "artifact_set_version": 1,
  "packages": [
    {
      "directory": "/srv/packages/org.example.agent",
      "generation_digest": "41cf6794ba4200b839c05da2be8b5f0fbcf0787fcf3727e006c5b92a8a38b2d51"
    }
  ]
}
```

Every directory is an absolute native path expressed in UTF-8, without
control characters, at most `k_max_start_absolute_path_bytes`. Windows paths
may use forward slashes. Every digest is the exact 64 lowercase hexadecimal
SDK generation digest of the artifact, including bootstrap and index bytes.
It binds an already-produced artifact; it is not a forward version policy.
The list is bounded by `k_max_candidates_per_root`, and the JSON file by
`k_max_index_bytes`. Duplicate members, unknown fields, unsupported artifact
list versions, malformed paths and digests are rejected. An empty package
array and an omitted input file both specify zero packages.

Each directory is captured by its canonical path and validated with
`Package_validator`. Duplicate package IDs from validated metadata reject
the set, including two copies of the same generation. Mismatched digests,
mixed bootstrap/index generations, missing or changed files, links below
the chosen package root, and invalid integrity or third-party records fail
through the SDK's existing rules. Source directories are caller-selected;
links in the chosen root path resolve before validation.

## Output and failure behavior

The output is a fresh directory with one immediate child named by each
validated package ID. The output's parent must exist. Staging writes a fresh
temporary sibling, copies captured bootstrap/index bytes and bounded indexed
file bytes, validates the copied packages, and revalidates the source
generations before publishing the complete directory. It never modifies an
existing output. A failed copy removes its temporary directory; a process
crash can leave an unreferenced `.cli-addon-stage-*` sibling for the owning
payload assembler to discard.

Package files retain their exact bytes. POSIX directories and indexed
executables/helpers have mode `0755`; other files have mode `0644`. This is
the same resource classification the host uses to materialize executable
custody files. Modes are not inputs to the package generation digest.

When requested, the receipt is written outside the output directory to a
fresh file whose parent exists. The directory and receipt are an offline
assembly operation, not an atomic installation transaction. A caller must
seal or publish its payload only after the entire command succeeds. If
writing the receipt fails after directory publication, the command fails and
the caller discards that incomplete assembly. Concurrent writers to the same
offline destination are not supported.

Replacing, removing, or reverting packages means staging a new payload from
the desired exact list while reusing the already-built core binaries. The
payload's established publication owner handles activation and rollback.

## Independently versioned evidence

The receipt is canonical SDK-serialized JSON followed by one newline:

```text
{
  "prebuilt_package_evidence_version": 1,
  "packages": [
    {
      "package_id": <validated package ID>,
      "generation_digest": <validated generation digest>,
      "files": [
        {"path": <relative path>, "bytes": <integer>, "sha256": <digest>,
         "resource": <integrity-index resource object or null>}
      ]
    }
  ]
}
```

Packages sort bytewise by package ID; files sort bytewise by relative path.
The bootstrap and integrity index are included with `resource: null`.
Every indexed file carries the SDK integrity index's resource object,
including its kind, origin, and any third-party notice and provenance
references. A zero-package receipt has `packages: []`.

`--verify` validates every package directory using the SDK, rejects other
immediate root entries and mismatched package-directory names, reconstructs
this receipt from the validated bytes, and requires exact equality with the
receipt file. Only then does it print the canonical receipt to stdout.
An absent package root is equivalent to an empty directory only when the
receipt is exactly the canonical zero-package receipt, so a file-only archive
can omit that empty directory. A file or link at the root is rejected.
Consumers must use that successful verification result before treating a
receipt as evidence for release inventory, signing exclusion, or notices.
Unknown receipt versions fail this optional evidence operation without
changing the core protocol, bootstrap, index, or capability schemas.

Staging succeeds with exit code `0` and a plain `OK staged <count> packages`
diagnostic. Verification succeeds with exit code `0` and only the canonical
receipt on stdout. Contract and I/O failures use exit code `1`, no verified
receipt, and `FAIL <reason> <detail>` on stderr. Invalid command-line syntax
uses exit code `2`. Receipt verification is read-only and does not run an
adapter.
