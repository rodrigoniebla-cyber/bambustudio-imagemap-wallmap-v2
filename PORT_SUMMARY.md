# Port: "One Tool-Change Per Layer" Image/Texture Printing → Bambu Studio

**Phase 1** — per-layer color solver, outer-wall line-width modulation, and
tool-change collapsing.
**Phase 2** — the variable-wall half: the outer wall's *surface* now actually
moves, in and out, instead of only changing line width in place. See
[Phase 2: outer-wall surface offsetting](#phase-2-outer-wall-surface-offsetting).

Port of the per-layer color solver, outer-wall line-width modulation, and
tool-change-collapsing logic from
[sentientstardust-dev/OrcaSlicer-ImageMap](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap)
(release **v1.0.22**, commit `92548381056dbf72836b0a1bdc455f238218dbfb`) into
[bambulab/BambuStudio](https://github.com/bambulab/BambuStudio)
(base commit `926a7192574bcb9b3a732e1ec59a46d79cb45466`, version 02.08.02.61).

## How this repository is laid out

Because this repository does not itself contain the Bambu Studio sources, the
deliverable is a PR-ready change set against the base commit above:

| Path | Content |
|---|---|
| `bambustudio/src/...` | All **newly added** files, in their in-tree locations (vendored libraries + the ported module) |
| `patches/imagemap-port-modified-files.patch` | Unified diff of the **modifications to existing** BambuStudio files (500 lines, 10 files) |
| `apply.sh` | Copies the new files into a BambuStudio checkout and applies the patch |

## What the feature does (when enabled)

With the toggle on, and a model painted multi-color the normal Bambu Studio way
(3MF mmu-segmentation painting and/or per-object/per-part filament assignment):

1. **Tool-change collapsing** — `ToolOrdering` derives the *rotation set*: the
   filaments used by object-body print regions (walls/infill), excluding any
   filament used for support or support interface. Each layer is assigned one
   *active* filament, cycling through the set across layers
   (C→M→Y→K→C→M→Y→K…, the balanced-sequence port of
   `build_balanced_component_sequence`). Every painted body extrusion on that
   layer is remapped to the active filament, so the object body needs **at most
   one tool change per layer** instead of one per painted region.
2. **Outer-wall line-width modulation** — for each external-perimeter path, the
   painted region's original filament color is the *target*. The vendored
   **ColorSolver** ("Generic Solver", working with any loaded filament set, not
   just CMYK) converts the target color into per-rotation-filament deposition
   weights using the **Pigment Painter** optical mixing model (or the
   prusa-fdm-mixer model). The weight of the layer's active filament scales the
   wall's line width between the configured min line width and the path's
   nominal width: full width where the active filament matches the desired
   color, narrowed (receding, letting neighboring layers' colors show) where it
   does not. Over one rotation cycle the wall surface therefore approximates
   the painted color — the "overhang modulation" technique of the origin
   project, with the per-layer deposition ratios coming from the solver.

## Files changed

### New: vendored libraries (`bambustudio/src/imagemap/`)

Kept deliberately separate from Bambu Studio core code; wired into the build
via `src/CMakeLists.txt`, and only `colorsolver` is linked into `libslic3r`.

| Directory | Role | License (files preserved) |
|---|---|---|
| `src/imagemap/pigment-painter/` | Color-mixing prediction (embedded 37 MB PNG LUT in `lut_wide.png.c`) | **GPLv3** — `COPYING` vendored |
| `src/imagemap/colorsolver/` | Target RGB → filament deposition ratios | **AGPLv3** — original license headers preserved in both sources |
| `src/imagemap/prusa-fdm-mixer/` | Alternative mix-prediction model, linked by ColorSolver (transitive dependency of the origin's ColorSolver, vendored for completeness) | **MIT** — `LICENSE` vendored |

Every vendored file (including the LUT `.c` file and the CMakeLists) carries a
top-of-file provenance comment naming the origin repo, origin file, release,
and commit; all original license headers are intact below the provenance note.

### New: ported module

* `src/libslic3r/ImageMapPerLayerColor.{hpp,cpp}` — the Phase-1 core, with
  per-function provenance notes:
  * `safe_mod`, `build_balanced_component_sequence` — **ported verbatim** from
    origin `TextureMapping.cpp`.
  * `active_filament_for_layer` — port of the tail of
    `TextureMappingManager::resolve_zone_component` (`sequence[layer % size]`).
  * `parse_filament_color` — port of `parse_hex_color`.
  * `Solver` — port of the generic-solver branch of
    `component_weights_for_sample` (origin `TextureMappingOffset.cpp`):
    candidate-set construction + `solve_color_solver_weights_for_target`,
    with per-target caching.
  * `modulated_outer_wall_width` — port of the width-clamping formulas of the
    origin's outer-wall gradient path (`GCode.cpp` ~11118–11144), including
    the positive-spacing lower bound `layer_height·(1−π/4)+1e-4`.
  * `rotation_filaments` — **adaptation, not a verbatim port** (see
    low-confidence flags below).

### Modified existing files (the 500-line patch)

| File | Change |
|---|---|
| `src/libslic3r/PrintConfig.cpp/.hpp` | 10 new options (below), all `comDevelop`, defaults inert |
| `src/libslic3r/Preset.cpp` | The 10 keys added to the print-options list |
| `src/libslic3r/GCode/ToolOrdering.hpp` | `LayerTools::image_map_filament_resolution` map + identity `resolve_image_map()`; declaration + rotation accessor |
| `src/libslic3r/GCode/ToolOrdering.cpp` | The four `resolve_mixed(result)` sites become `resolve_image_map(resolve_mixed(result))`; `resolve_image_map_per_layer_filaments()` called from both `sort_and_build_data` overloads right after `resolve_mixed_filaments()` |
| `src/libslic3r/GCode.hpp/.cpp` | Solver member + `init_image_map_per_layer_color()` (called once in `do_export`) + `image_map_modulate_outer_wall_loop()`, called on the by-value loop copy at the top of `extrude_loop()`. **Phase 2 replaced Phase 1's `_extrude` shim with this**: `_extrude()` is back to its original signature and body |
| `src/CMakeLists.txt`, `src/libslic3r/CMakeLists.txt` | `add_subdirectory` for the three vendored libs; `colorsolver` added to `libslic3r`'s link list; new module sources registered |
| `src/slic3r/GUI/Tab.cpp` (`patches/imagemap-port-gui-wiring.patch`, applied separately by `apply.sh`) | New "Image map per-layer color (experimental)" optgroup in Process Settings > Others, wiring all 10 options into the UI (visible once Preferences > Develop mode is on; previously the options existed only in the config schema with no UI control) |

## The default-off toggle

* **`image_map_per_layer_color_rotation`** (`coBool`, default **false**,
  `comDevelop`) — master switch, defined in `PrintConfig.cpp`. Checked by
  `ImageMapPerLayer::enabled()`; every feature entry point returns immediately
  when it is false.
* Supporting options (all inert unless the master switch is on):
  * `texture_mapping_outer_wall_gradient_global_strength` (float, 100) — ported definition
  * `texture_mapping_outer_wall_gradient_max_line_width` (float, 0.95 mm) — ported definition
  * `texture_mapping_outer_wall_gradient_min_line_width` (float, 0.32 mm) — ported definition
  * `image_map_generic_solver_lookup_mode` (int, 0 = closest mix)
  * `image_map_generic_solver_mode` (int, 255 = slicer default → Oklab soft-cap)
  * `image_map_generic_solver_mix_model` (int, 0 = Pigment Painter)
  * `image_map_outer_wall_mode` (int, **Phase 2**, 0 = narrow inward,
    1 = narrow centered, **2 = offset only, the default**) — how the wall
    approximates the painted colour; see
    [Phase 2](#phase-2-outer-wall-surface-offsetting)
  * `image_map_wall_offset_distance` (float mm, **Phase 2**, default 0.15,
    max 0.35) — peak surface displacement in offset-only mode
  * `image_map_wall_offset_inward_only` (bool, **Phase 2**, default false) —
    offset-only mode moves the wall inward only, preserving outer dimensions

Changing any of these keys triggers a full reslice (they are intentionally not
listed in `Print::invalidate_state_by_config_options`, whose fallback branch
invalidates all steps — the conservative default).

## Disabled-state no-op audit (reasoning, not just assertion)

Every touched code path, traced with the toggle off:

1. **`ToolOrdering::resolve_image_map_per_layer_filaments`** — first statement
   clears `m_image_map_rotation` (already empty) and returns before touching
   any `LayerTools`. No state changes.
2. **`LayerTools::wall_filament / sparse_infill_filament / solid_infill_filament /
   extruder`** — the added `resolve_image_map()` wrapper does a `std::map::find`
   on a map that is empty when the feature is off, returning its argument
   unchanged. Identical return values, no side effects.
3. **`GCode::init_image_map_per_layer_color`** — sets
   `m_image_map_modulation_enabled = false` and returns at the toggle check.
4. **`GCode::extrude_loop`** — the added call to
   `image_map_modulate_outer_wall_loop()` returns false at its first statement
   when `m_image_map_modulation_enabled` is false, without reading or writing
   `loop`. The remainder of `extrude_loop` is textually unchanged, and
   `_extrude` is no longer touched at all (Phase 2 removed Phase 1's shim
   there).
5. **Vendored libraries** — linked into the binary but no code path calls into
   them unless the solver is initialized, which only happens behind the toggle.
6. **Config plumbing** — new options only add defaulted values.

**One honest caveat on "bit-for-bit":** exported G-code embeds the full print
config as `; key = value` comment lines between `CONFIG_BLOCK_START/END`
(`GCode::append_full_config` dumps every key). The 7 new keys therefore add 7
comment lines to that metadata block even when the feature is disabled. This is
inherent to adding any config option to Bambu Studio (the upstream
mixed-filament feature has the same effect) and has zero effect on toolpaths,
motion, tool changes, or print quality — every executable line of G-code is
unchanged. If literal byte-identity of the config comment block is required,
the keys would have to be removed from `Preset.cpp`'s print-options list, at
the cost of the options not being persistable.

## Reuse of existing tool-change G-code (requirement 4)

No tool-change G-code generation was added or altered. Tool changes are
emitted, exactly as before, by iterating `layer_tools.extruders` in
`GCode::process_layer` → `set_extruder` → the existing `change_filament_gcode`
/ machine G-code path (M620 family for AMS, nozzle-change handling for
dual-nozzle machines). The feature only **shrinks the per-layer extruder list**
in `ToolOrdering` before that machinery runs — the change is frequency only,
never shape.

## H2C / multi-extruder preservation (requirement 3)

H2C ("Hyper Multi Color") support in this code base is the multi-extruder /
multi-nozzle filament grouping machinery. Traced H2C-related paths and how the
change relates to each:

* `ToolOrdering::get_recommended_filament_maps` and the
  `fmmManual`/`fmmNozzleManual` branches (`ToolOrdering.cpp` ~1876–1899,
  including the explicit "处理H2C的…" branches) — **untouched**. They consume
  per-layer filament lists collected from `m_layer_tools`; with the feature on
  they simply see fewer filaments per layer (ordinary physical filament ids),
  the same contract under which the upstream mixed-filament feature already
  feeds them.
* `reorder_extruders_for_minimum_flush_volume` (flush/grouping optimization for
  multi-nozzle machines) — **untouched**; runs *after* the collapse, mirroring
  the position of the existing `resolve_mixed_filaments()`.
* Wipe-tower planning incl. the H2C prime-volume special case
  (`Print.cpp` ~3820, `PrimeVolumeMode::pvmSaving`) — **untouched**; plans
  fewer tool changes but with identical logic per change.
* H2C/X2D timelapse handling (`GCode.cpp` ~4467) — **untouched**.
* Support filaments are excluded from the rotation set and never remapped or
  removed from `LayerTools::extruders`, so support/interface tool changes —
  including their H2C nozzle handling — are byte-identical.

The deliberate design choice, per the task's guidance, was **not** to hook the
origin's zone resolution into `collect_extruders` (which has diverged heavily),
but to piggyback the exact insertion point and remap mechanism Bambu Studio
already uses for its own virtual-filament feature (`resolve_mixed_filaments`),
so every downstream H2C path is exercised the same way it already is today.
If both features are configured at once, the port refuses to activate
(mixed-filament wins) rather than compose untested interactions.

## Low-confidence areas (flagged in code comments too)

1. **Rotation-set derivation** (`ImageMapPerLayer::rotation_filaments`,
   flagged in `ImageMapPerLayerColor.hpp` and `ToolOrdering.cpp`): the origin
   derives components from explicit TextureMappingZone filament lists; Phase 1
   has no zones, so the set is derived from painted body print-region configs.
   This is an adaptation, not a proven mapping. Corner cases: a filament used
   both for support and painting is excluded from rotation (that region keeps
   normal tool changes, so such a layer can exceed one tool change); per-layer
   custom filament switches (`extruder_override`) are also remapped when they
   point into the rotation set.
2. **Layer indexing**: the rotation advances by index into the merged
   `m_layer_tools` list (all objects + support layers z-merged), not by
   per-object layer id. For a single object this matches the origin's
   `layer_index`; for multiple objects of different heights printed "by layer"
   it is an approximation. The wall-modulation side is index-independent (it
   keys off the actual active tool), so the two sides cannot disagree.
3. **Per-loop (not per-segment) modulation**: with painting, the color target is
   constant per region, so a constant width and centerline shift per loop is
   exact for the solver output; the origin's per-segment image sampling,
   dithering and halftone machinery is Phase 3 scope. The centerline shift
   itself *is* ported as of Phase 2 (see
   [Phase 2](#phase-2-outer-wall-surface-offsetting)); the origin's
   vertex-color-match widening is ported in adapted form (inner edge pinned
   rather than slice surfaces inset), which is the one deliberate deviation.
4. **Wipe-tower sizing**: fewer per-layer tool changes shrink wipe-tower
   partitions via the existing (`fill_wipe_tower_partitions`) logic after the
   collapse. This is the intended benefit but has not been exercised end-to-end.

## Bugs found and fixed via real-world verification

The feature was verified end-to-end against a real multi-color H2C project
(`3DBenchy_H2C_Multi_Color_Test_Print.3mf`, 9 declared filament slots, 4
actually painted onto the model, toggle on, no support). The first slice with
the toggle on showed **no improvement at all** (711 filament changes over 240
layers — the same as with the toggle off). Root-causing this against real
data (not synthetic test objects) surfaced three real defects, now fixed:

1. **`ToolOrdering::resolve_image_map_per_layer_filaments()`'s mixed-filament
   guard checked the entire project's `filament_is_mixed` array**, not just
   the filaments in the rotation set. Any project with a virtual/mixed AMS
   filament slot configured *anywhere* — even one never used by the object
   being sliced — silently disabled the feature for the whole print. Fixed to
   only bail out on filaments that are actually part of the rotation set.
2. **`rotation_filaments()`'s candidate derivation was unreliable in both
   directions on real projects.** The original implementation enumerated
   `print.num_print_regions()`, which Bambu Studio pre-creates one-per-
   declared-filament-slot regardless of usage — over-including unused/
   unrelated slots (this alone reproduced bug #1 above even after fixing the
   guard's scope, since the polluted candidate set still contained the unused
   mixed slot). Switching to `Print::object_extruders()` fixed the
   over-inclusion but then *under*-included a color that was genuinely
   painted and extruded on every layer, depending on the volume's paint data
   shape.
3. **Fix**: `ToolOrdering::resolve_image_map_per_layer_filaments()` now
   derives the rotation set directly from `m_layer_tools[*].extruders` — the
   real, already-resolved per-layer extruder lists that literally feed the
   resulting G-code — instead of any Print-level config/region/paint
   heuristic. `GCode::init_image_map_per_layer_color()` was changed to read
   that same authoritative set via `print.tool_ordering().image_map_rotation_filaments()`
   instead of independently recomputing it, so tool-change collapsing and
   outer-wall width modulation can never disagree. `ImageMapPerLayer::rotation_filaments()`
   (the original free function) remains as a lighter-weight, pre-slicing
   estimate — documented as such in `ImageMapPerLayerColor.hpp` — but is no
   longer on the critical path.

After all three fixes, the same Benchy project dropped from 711 filament
changes to **239 over 240 layers** — i.e. one change per layer, matching the
feature's design intent.

## Phase 2: outer-wall surface offsetting

### The bug Phase 1 shipped with

Phase 1 changed an external perimeter's `width` and `mm3_per_mm` and left its
toolpath centerline exactly where the perimeter generator put it. That is not
what the origin does, and on its own it is geometrically wrong. Narrowing an
extrusion in place moves *both* of its edges: the outer surface comes in by
only **half** the width loss, and an equal gap opens on the **inner** side,
between the outer wall and the wall behind it. So Phase 1 got half the intended
color swing and paid for it with a growing void inside the wall stack.

The origin never does this. Every width change is paired with a centerline
shift along the segment's inward normal (origin `GCode.cpp` ~11108:
`centerline_shift = base_centerline_shift + 0.5 * width_delta`). Phase 2 ports
that shift, which is what makes the wall's surface genuinely move.

### What moves, and how far

| | width sweep | surface moves | inner edge |
|---|---|---|---|
| Phase 1 (shipped) | nominal → min | inward by ½ the width loss | **drifts inward, opening a void** |
| `Inward` (mode 0) | nominal → min | inward by the full width loss | fixed |
| `Centered` (mode 1) | max → min, spanning nominal | **outward *and* inward** around nominal | fixed |
| `OffsetOnly` (mode 2, **default**) | **none — width untouched** | outward and/or inward by the configured distance | moves with the wall |

Both Phase 2 modes pin the wall's inner edge by shifting the centerline exactly
half the width change:

```
shift = 0.5 * (nominal_width - new_width)      // + = toward the material
```

so `centerline - width/2` is invariant and the surface moves by exactly
`new_width - nominal_width`. For mode 0 this is *identical* to the origin's
offset-gradient shift (there `base_outer_width == nominal`, so
`0.5 * width_delta == 0.5 * (nominal - new)`).

Surface movement is capped in both directions by a port of
`TextureMappingManager::max_component_surface_offset_mm()` (origin
`TextureMapping.cpp:2219`): `clamp(|nozzle diameter|, 0.01, 0.35)` mm. The
origin applies the same number as `max_width_delta_limit = min(effective_delta,
2 * max_allowed_distance)` (origin `GCode.cpp:11026`).

### Deliberate deviation from the origin's "vertex color match"

The origin's vertex-color-match mode widens the wall to
`texture_mapping_outer_wall_gradient_max_line_width` (default 0.95 mm vs a
0.42 mm nominal) about a centerline pre-shifted **inward** by
`0.5 * (max - nominal)`. That keeps the *outer* surface honest but leaves the
wall's inner edge ~0.2 mm inside the nominal one — straight through the wall
behind it. The origin can afford that because it *also* insets the slice
surfaces at perimeter-generation time
(`texture_mapping_offset_surface_inset_mm`, origin `LayerRegion.cpp`) so the
space is reserved before the walls are ever laid out.

That slice-time inset is a large, invasive piece of machinery and is Phase 3
scope here. Porting the origin's shift **without** it would over-extrude every
widened wall into its neighbor. So mode 1 instead pins the inner edge and lets
only the outer surface travel. It gets the same visual effect and a comparable
color range, needs no reserved space, and cannot over-extrude.

### Applied per loop, not per segment

The origin samples an image texture per path *segment*, so it displaces each
segment's endpoints individually inside its G-code emission loop
(`OuterWallGradientSegmentMod`). Here the color target of a painted region is
constant, so the weight — and therefore both the width and the shift — is
constant along a whole external perimeter loop.

That lets the shift be applied **once**, as a miter offset of the closed loop,
at the top of `GCode::extrude_loop()` — before seam placement, loop clipping and
scarf-seam construction ever see the geometry, all of which then operate on the
already-offset wall. `ImageMapPerLayer::offset_closed_ring()` does the offset
with proper miter joins, preserving the vertex count so the result can be
written straight back into the individual `ExtrusionPath`s that make up the ring.
It refuses (leaving the caller's geometry untouched) on degenerate rings and
whenever the offset would collapse or invert the ring — which is what happens on
a feature thinner than twice the offset.

This also *replaces* Phase 1's `_extrude` shim: `_extrude()` reverts to its
original signature and body, and the feature's only G-code-side hook is now the
one line at the top of `extrude_loop`.

### Why `OffsetOnly` is the default (the Benchy regression)

Modes 0 and 1 create the colour swing by **thinning** the extrusion. Verified
against the real `3DBenchy_H2C_Multi_Color_Test_Print.3mf`, that turned out to
have two failure modes that ruin real models, both visible as voids in the wall
and a broken-up surface concentrated at the **top and bottom** of the model
while the middle looked fine:

1. **Unextrudable lines.** The project had
   `texture_mapping_outer_wall_gradient_min_line_width` at its 0.05 mm floor
   (and max at 3 mm). The width-modulating modes honoured that literally,
   asking for **~0.07 mm external perimeters on a 0.4 mm nozzle**. Those do not
   extrude, and the missing material shows up as white speckling across the
   surface. Fixed generally by flooring the width at
   `min_printable_width_mm()` = `max(0.05, nozzle/2)` — 0.20 mm on a 0.4 mm
   nozzle — regardless of what the config asks for. This floor is **not** in
   the origin and is a deliberate addition.
2. **Overhang loops had to be skipped.** Re-widthing bridged material is not
   safe, so modes 0/1 skip any loop containing an overhang path. On a straight
   vertical wall that never happens and the surface is uniform; on a sloped
   region — a hull bottom, a roof — modulated and unmodulated loops end up
   side by side and the surface breaks up. That is precisely the "middle is
   fine, top and bottom are weird" signature.

`OffsetOnly` has neither problem *by construction*: it leaves `width` and
`mm3_per_mm` exactly as the perimeter generator set them and only translates
the ring, so nothing can be thinner than what the slicer already chose, and
there is no longer any reason to skip overhang loops — every external perimeter
is treated alike. It is therefore the default (`image_map_outer_wall_mode = 2`).

Its two settings:

* `image_map_wall_offset_distance` (float mm, default **0.15**, max 0.35) —
  peak surface displacement. Larger = stronger colour, rougher surface.
* `image_map_wall_offset_inward_only` (bool, default **false**) — when on, the
  wall only ever moves inward, preserving the model's outer dimensions; when
  off it swings symmetrically ±d/2 about nominal.

**Note for existing projects:** a 3MF saved before this change has
`image_map_outer_wall_mode = 0` stored in it and will keep using the
width-modulating path (now with the printable-width floor). Switch it to 2 to
get the new behaviour.

### What Phase 2 deliberately skips

* **In the width-modulating modes only (0 and 1), loops containing overhang
  paths are left entirely alone.** They are bridged/unsupported material with
  their own flow and speed handling. They would have to move with the rest of
  the ring to keep it connected, but re-widthing them is not safe, and shifting
  them without re-widthing would push them into the wall behind. `OffsetOnly`
  does not have this restriction (see above).
* **`reduce_outer_surface_texture`** (origin `GCode.cpp:11121`) is a *per-path
  average* correction: it re-centers the mean centerline shift across a path's
  segments. With a constant weight per path there is no per-segment variation to
  average, so it is a no-op by construction and was not ported.
* Loops whose paths do not all share one width, or whose junctions are not
  exact, are skipped rather than approximated.

## Out of scope (Phase 3)

Texture/image import, vertex-color import UI, gradient/halftone/projection
panels, per-segment sampling and dithering, the slice-time surface inset
(`texture_mapping_offset_surface_inset_mm`) that would allow the origin's true
vertex-color-match widening, top-surface contoning
(`TextureMappingContoning`), the raw-filament offset atlas, prime-tower image
painting, and 3MF persistence of zone definitions.

## Testing performed (and not performed)

* All four vendored translation units compile standalone (g++ 13, C++17),
  including the 37 MB LUT (compiled as C, as the origin build does).
* Every modified/added libslic3r file (`ImageMapPerLayerColor.cpp`,
  `ToolOrdering.cpp`, `GCode.cpp`, `PrintConfig.cpp`, `Preset.cpp`, and the
  headers they pull in) passes a full `g++ -fsyntax-only` check against the
  real Bambu Studio headers (system boost/TBB/eigen; OCCT stubbed with a
  header shim for checking only).
* A functional unit test (not committed; reproduced in this summary's history)
  exercised the ported logic with the real vendored libraries:
  * rotation sequence `{0,1,2,3}` → layers 0..8 yield `0 1 2 3 0 1 2 3 0`;
    weighted `{0:3, 1:1}` → `0 0 1 0`.
  * width modulation: weight 1 → nominal 0.42 mm, weight 0 → configured min
    0.32 mm, monotone in between; strength 0 disables modulation.
  * ColorSolver: a component color solves to weight ≈ 1.0 for itself; a mixed
    target yields weights summing to 1.0; both the prusa-fdm-mixer path and
    the Pigment Painter path (embedded-PNG LUT decode) produce sane output.
* **Since performed** (superseding the paragraph below): a full macOS arm64
  build (see `ci/build-macos.sh`), a Catch2 unit test
  (`tests/fff_print/test_imagemap.cpp`, build with `-DSLIC3R_BUILD_TESTS=ON`,
  target `imagemap_tests`) exercising `rotation_filaments()` against real
  `Print`/`PrintRegion` objects, and an end-to-end CLI slicing comparison
  against a real multi-color H2C project — see "Bugs found and fixed via
  real-world verification" above, which the end-to-end test surfaced and this
  port has since fixed.
* **Phase 2** adds Catch2 coverage in the same `imagemap_tests` target for the
  new geometry:
  * `Inward` mode never widens past nominal, is monotone in the solver weight,
    pulls the surface in by the *full* width loss (not half, as Phase 1 did),
    and leaves the wall's inner edge exactly where it was at every weight.
  * `Centered` mode moves the surface both outward and inward, keeps the inner
    edge pinned in both directions, and yields a strictly larger colour range
    than `Inward`.
  * Surface movement stays inside the nozzle-derived cap even when the
    configured max line width is absurd (3 mm); `max_surface_offset_mm()`
    matches the origin's `clamp(nozzle, 0.01, 0.35)`.
  * Strength 0 is an exact no-op in both modes.
  * `offset_closed_ring()` miters a square exactly (corner lands on the inset
    corner, not on an edge normal), works in both winding directions, grows on
    a negative delta, and *refuses* — leaving the caller's geometry untouched —
    on rings it would collapse or invert, on fewer than 3 points, and on a zero
    delta.
  All 8 cases / 123 assertions pass on macOS arm64 against the real built
  `libslic3r`, alongside a full app build that runs and reports all 8 config
  keys at their defaults (`image_map_outer_wall_mode = 0`, master toggle off).

  Two notes on running them, both pre-existing upstream conditions rather than
  anything this port introduces:
  * `imagemap_tests` links upstream `tests/fff_print/test_data.cpp` for its
    `mesh()` / `init_print()` helpers, which drags that file's own
    `Scenario: init_print functionality` TEST_CASE in with it. That scenario
    fails ("Objects could not fit on the bed") on stock BambuStudio
    02.08.02.61 — the port does not touch `test_data.cpp`, and the whole
    upstream `fff_print_tests` target no longer even *compiles* against this
    version (`test_gcodewriter.cpp` calls a `GCodeWriter::lift()` and reads a
    `GCodeConfig::retract_lift` that no longer exist). The `add_test()` entry
    therefore filters to `[ImageMapPerLayerColor]`.
  * `ctest` cannot launch any of BambuStudio's test targets on macOS — they
    are built as `.app` bundles and `ctest` looks for a bare executable
    (`libnest2d_tests` and every other upstream target fail identically). Run
    the binary directly:
    `./tests/fff_print/imagemap_tests.app/Contents/MacOS/imagemap_tests "[ImageMapPerLayerColor]"`.
* **Not performed**: any print on real hardware, and no end-to-end slice
  comparison of the Phase 2 geometry against a real project (the Phase 1
  tool-change collapsing numbers above predate it, and were measured before
  the centerline shift existed). No claim is made that the feature produces
  correct colors on a physical printer.
