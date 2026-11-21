# presentation@1

`presentation@1` supplies provider-neutral display data for one package. The
sidecar is data, not UI code: it cannot name a component, script, font, URL or
external resource. The host owns every decoded/rasterized byte it publishes.

The pair is live only when all three authorities agree: the package has the
`presentation@1` sidecar, the adapter handshake advertises the same pair, and
the host adopts version 1. The host does not consume a sidecar merely because
the file exists. Version 1 has no operation; its sidecar payload is the record.

## Sidecar payload

```json
{
  "display_name": "Example CLI",
  "icon": {
    "light": "assets/example-light.svg",
    "dark": "assets/example-dark.png"
  },
  "accent": {
    "light": "#3366CC",
    "dark": "#88AAFF"
  }
}
```

`display_name` is UTF-8. The host trims the Unicode White_Space property and
then requires 1-40 Unicode scalar values, at most 120 UTF-8 bytes, with no C0,
C1 or DEL controls and no line or paragraph separator. The stored value is the
trimmed value.

`icon.light` and `icon.dark` are canonical package-relative paths whose first
segment is `assets`. They may be equal. Both paths must be present in the
custodied integrity index with kind `asset`; extension does not select media.

`accent` is optional as a whole. When present, both members are required and
each is `#RRGGBB`; lowercase hexadecimal input is normalized to uppercase.
Accent is a hint and never changes icon admission.

An unexpected or mistyped member is `presentation_invalid`.

## Icon admission

Each source is at most 256 KiB. Content beginning with the PNG signature is
validated as one complete non-animated PNG stream: a unique 13-byte IHDR is
first; dimensions, bit-depth/colour-type, compression, filter and interlace
fields are legal; critical chunks are known and ordered; indexed images have a
valid PLTE; all chunk types and CRCs are valid; IDAT chunks are consecutive;
IEND is empty and terminal; no APNG chunk is present. Pixel decompression and
canonical re-encoding belong to the host renderer.

Otherwise the first element must be `svg` and the bytes must pass the closed
subset in [svg_subset.md](svg_subset.md). SVG `currentColor` is rendered with
`#1F1F1F` for the light variant and `#F2F2F2` for the dark variant.

PNG width and height are each 16-1024 pixels. PNG dimensions and SVG viewBox
dimensions must have an aspect ratio within 1:4 and 4:1. The host rasterizes to
64x64 canonical PNG bounded to 32 KiB before publication. Source overrun is
`icon_too_large`; unsupported or malformed media is `icon_media_unsupported`;
dimension/aspect failure is `icon_dimensions_invalid`; SVG reports
`svg_subset_violation` or `invalid_utf8`.

## Publication and fallback

The canonical host resource identity includes package ID, canonical bytes,
presentation-record version and dimensions. Thus two packages with identical
source bytes do not share ownership. Consumers receive host-owned bytes and an
opaque package/resource identity, never a package path.

If the pair is not live or either record/icon is rejected, presentation alone
is unavailable. The package and its unrelated capabilities remain usable and
the product renders its documented generic label/icon fallback.
