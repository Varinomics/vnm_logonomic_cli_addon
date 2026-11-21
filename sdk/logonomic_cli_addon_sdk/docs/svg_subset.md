# SVG presentation subset

`presentation@1` admits SVG only through the SDK sanitizer. The original bytes
are never rendered. The sanitizer parses strict XML, applies the grammar below
and emits deterministic canonical SVG whose root `color` is the host-supplied
validated `#RRGGBB` value. `currentColor` therefore resolves deterministically.
Unsupported syntax fails closed.

## Document and elements

The root is one `svg` element in the SVG namespace. XML declarations are
accepted; document types, entities, processing instructions, comments and
foreign namespaces are rejected. Element names are case-sensitive. The only
elements are:

`svg`, `g`, `defs`, `use`, `path`, `rect`, `circle`, `ellipse`, `line`,
`polyline`, `polygon`, `linearGradient`, `radialGradient`, `stop`, `clipPath`,
`title` and `desc`. Their nesting follows the SVG content model enforced by
the SDK: gradients contain stops, clip paths contain shapes or local uses, and
shapes/uses contain only title or description metadata.

At most 1024 source elements, depth 16 and 32 attributes per element are
accepted. Every local `use` occurrence is expanded for accounting; the fully
expanded graph is at most 4096 elements and every reference chain is at most
four hops. References must be `#id`, resolve to exactly one local element and
be acyclic. External URLs and undeclared prefixes are rejected.

## Attributes and values

Only the attributes appropriate to each admitted element are accepted. The
complete admitted vocabulary is:

- structure: `xmlns`, `xmlns:xlink`, `version`, `id`, `viewBox`,
  `preserveAspectRatio`, `href`, `xlink:href`, `x`, `y`, `width`, `height`;
- geometry: `d`, `points`, `cx`, `cy`, `r`, `rx`, `ry`, `x1`, `y1`, `x2`,
  `y2`, `fx`, `fy`;
- paint: `fill`, `fill-opacity`, `fill-rule`, `stroke`, `stroke-opacity`,
  `stroke-width`, `stroke-linecap`, `stroke-linejoin`, `stroke-miterlimit`,
  `stroke-dasharray`, `stroke-dashoffset`, `opacity`, `color`, `stop-color`,
  `stop-opacity`, `clip-rule`, `clip-path`, `vector-effect`,
  `shape-rendering`;
- gradients and transforms: `offset`, `gradientUnits`, `gradientTransform`,
  `spreadMethod`, `clipPathUnits`, `transform`.

IDs are 1-64 ASCII letters, digits, underscore, hyphen, dot or colon, with a
letter, underscore or colon first, and are unique. Non-path attributes are at
most 1024 bytes. Numeric values are finite SVG decimal numbers. Geometry
lengths admit unitless, `px` and percentage values only where their attribute
grammar permits them. CSS, `style`, event attributes, scripts, animation,
filters, masks, embedded images and fonts are rejected. Paint admits `none`,
`currentColor`, `#RGB`, `#RRGGBB`, SVG 1.1 colour keywords, `rgb(...)`, and
local `url(#id)` references to admitted gradients; `clip-path` similarly
references a local `clipPath`. A transform list contains at most eight
`matrix`, `translate`, `scale`, `rotate`, `skewX` or `skewY` operations with
their exact argument counts.

## Paths and points

Each `d` or `points` value is at most 32 KiB. Path data accepts the complete
bounded SVG command grammar: `M/m`, `L/l`, `H/h`, `V/v`, `C/c`, `S/s`, `Q/q`,
`T/t`, `A/a` and `Z/z`; implicit repeated argument groups are accepted. Numbers
must be finite SVG decimal tokens. Arc flags are exactly `0` or `1`. A command
missing an argument group, an incomplete group, invalid separator or trailing
token is rejected.

`polyline` and `polygon` points contain complete x/y pairs only. Paths and
points together are bounded by 4096 drawn segments per attribute. A moveto
pair after the first is accounted as a lineto, and every repeated command
group counts independently.

## Canonical result

Canonical output has deterministic element/attribute ordering and escaping,
contains no external reference, and is independently bounded by the
presentation source limit. Rasterization occurs only from these sanitized
bytes. Any parse, grammar, reference or bound failure rejects the presentation
resource; the host then uses the package fallback icon path described by
presentation@1.
