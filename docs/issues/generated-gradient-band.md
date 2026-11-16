# Issue: sharp diagonal band in generated page gradients

Reported 2026-10-17 while visually reviewing `examples/pe-resources.toml`.

## Expected behavior

A single hexadecimal `color` seeds a subtle gradient. A true solid surface uses
an explicit named gradient whose endpoint colors at positions 0 and 1 match.
Built-in schemes should also transition gradually across the page.

## Cause and scope

The shared `palette()` helper in `src/colors.cpp` generated these page stops:

| Position | Color |
| --- | --- |
| 0 | Base color |
| 0.010 | Base color |
| 0.035 | Base mixed with 55% white |
| 1 | Far-edge color |

This concentrates a large brightness change into 2.5% of the ramp, then often
reverses toward a more saturated far-edge color. The oblique native gradient
turns it into the visible diagonal band. The `mix()` calculation itself is
correct; the generated stops were inappropriate for a gradual page transition.

The problem affects all twelve built-in palettes and hexadecimal seeds. The
editable `colors-light.toml` and `colors-dark.toml` already use widely spaced
stops. Explicit gradients, including older definitions in `colors.toml`, draw
exactly the stops they declare and may intentionally contain sharp highlights.

## Resolution

Generated pages now interpolate from the same base color at 0 to the same
far-edge color at 1. The inactive-tab ramp, hover colors, outline, gradient
geometry and image rendering are unchanged. Single hexadecimal seeds retain
their existing generated endpoint shades.

Custom definitions are not smoothed or rewritten. Their identical endpoint
colors still produce a solid fill. Installed personal palette files are
preserved during development acceptance.

## Verification

The native PNG exporter reproduced the band before the fix using both
`#F3C982` and compiled built-ins, with an adjacent empty `colors-builtin.toml`
and `colors = "builtin"` to exclude personal overrides. A focused regression
test failed on the original implementation: a channel changed by 140 levels
over 2.5% of the ramp. Generated ramps now have a bounded rate of color change;
explicit flat fills and authored sharp stops have separate coverage.

Release verification passed: 322 assertions in the twelve `[colors]` cases,
plus `Launcher.DrawingSmoke`. Native before/after renders were inspected for
Orange and the original five-icon resource example. A contact sheet of all
twelve corrected built-in palettes was also inspected. The original palette
endpoints remain covered by the existing tests. Debug, ordinary-header builds
and the full test suite were not run.

To reproduce a compiled scheme without personal overrides, place an empty
`colors-builtin.toml` beside a test configuration containing:

```toml
schema_version = 1
colors = "builtin"
[[tabs]]
name = "Generated gradient"
color = "#F3C982" # Or a built-in name such as "orange".
items = [{icon = {stock = "computer"}}]
```

Render it with `tools/render-lighting-previews.ps1 -ConfigurationPath <path>`.
The native exporter preserves the configuration and any adjacent runtime file.

Visual sign-off remains with John. Native previews show the actual renderer's
output; they do not establish aesthetic approval.
