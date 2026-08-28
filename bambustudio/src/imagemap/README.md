# Vendored color libraries (OrcaSlicer-ImageMap port)

This directory contains third-party/ported libraries vendored from
[OrcaSlicer-ImageMap](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap)
(release v1.0.22, commit `92548381056dbf72836b0a1bdc455f238218dbfb`) to support
the *image-map per-layer color rotation* feature (Phase 1 + Phase 2 port).

They are deliberately kept separate from Bambu Studio core code.

| Directory | What it is | License |
|---|---|---|
| `pigment-painter/` | Color mixing prediction (how filament layers optically combine). Contains an embedded PNG LUT (`lut_wide.png.c`). | GPLv3 (`pigment-painter/COPYING`) |
| `prusa-fdm-mixer/` | Alternative FDM color-mix prediction model; linked by ColorSolver. | MIT (`prusa-fdm-mixer/LICENSE`) |
| `colorsolver/` | Converts a target RGB into filament deposition ratios (the "generic solver"). | AGPLv3 (headers in the sources) |

All original license headers are preserved; each source file carries a
top-of-file provenance comment naming the origin repo/file/commit.

Only `colorsolver` is linked into `libslic3r` (it pulls in the two mixers).
Nothing in these libraries executes unless the feature toggle
`image_map_per_layer_color_rotation` is enabled (default: off).
