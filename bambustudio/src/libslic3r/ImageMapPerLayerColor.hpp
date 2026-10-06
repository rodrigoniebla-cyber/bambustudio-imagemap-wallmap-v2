// Bambu Studio port of the "one tool-change per layer" image/texture color
// rotation from OrcaSlicer-ImageMap (Phase 1: solver + outer-wall line-width
// modulation + tool-change collapsing, driven by existing 3MF filament
// painting; no texture/image import).
//
// Ported from:
//   Origin repo:  https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap
//                 (release v1.0.22, commit 92548381056dbf72836b0a1bdc455f238218dbfb)
//   Origin files: src/libslic3r/TextureMapping.cpp
//                   (safe_mod, build_balanced_component_sequence,
//                    TextureMappingManager::resolve_zone_component, parse_hex_color)
//                 src/libslic3r/TextureMappingOffset.cpp
//                   (component_weights_for_sample -> generic solver path)
//                 src/libslic3r/GCode.cpp
//                   (outer-wall gradient line-width clamping formulas)
//   Original author: sentientstardust. License: AGPLv3 (same as Bambu Studio).
//
// The vendored solver dependencies live in src/imagemap/ (see its README.md).

#ifndef slic3r_ImageMapPerLayerColor_hpp_
#define slic3r_ImageMapPerLayerColor_hpp_

#include <array>
#include <map>
#include <string>
#include <vector>

#include "Point.hpp"

#include "ColorSolver.hpp"

namespace Slic3r {

class Print;
class PrintConfig;

namespace ImageMapPerLayer {

// True when the master toggle "image_map_per_layer_color_rotation" is enabled.
// Everything in this module must be behind this check; with the toggle off
// (the default) no caller may alter its behavior in any way.
bool enabled(const PrintConfig &config);

// 0-based filament ids used by object-body volumes (painted / per-part
// assigned, via ModelVolume::get_extruders()), excluding filaments used for
// support or support interface anywhere in the print, sorted ascending.
//
// This is a Print-level *approximation* of the rotation set, useful before
// any slicing has happened. It is NOT what ToolOrdering actually uses to
// decide the live rotation set: on real multi-part/painted projects this was
// found to disagree with reality in both directions versus the real per-layer
// extruder usage --
//   - enumerating print.num_print_regions() (an earlier version of this
//     function) over-includes filament slots that are merely *declared*
//     somewhere in the project's configuration but never actually used on
//     this object (Bambu Studio pre-creates one PrintRegionConfig per
//     configured filament slot regardless of usage) -- this was found to
//     fully disable the feature on real projects that simply had an unused,
//     unrelated virtual "mixed" AMS slot elsewhere in the filament list;
//   - object_extruders()/get_extruders() itself can *under*-include a color
//     that is genuinely extruded on every single layer, depending on how the
//     paint/volume data happens to be structured.
// ToolOrdering::resolve_image_map_per_layer_filaments() therefore derives the
// authoritative rotation set directly from m_layer_tools[*].extruders (the
// real, already-resolved per-layer extruder lists -- literally what feeds the
// resulting G-code), not from this function; GCode::init_image_map_per_layer_color()
// in turn reads that same authoritative set via
// print.tool_ordering().image_map_rotation_filaments() so both sides of the
// feature (tool-change collapsing and outer-wall width modulation) always
// agree. This function remains as a lighter-weight, pre-slicing estimate.
//
// LOW-CONFIDENCE MAPPING NOTE: OrcaSlicer-ImageMap derives its rotation set
// from explicit TextureMappingZone component ids (an explicit per-zone
// filament list); Bambu Studio has no zones in Phase 1, hence the
// volume/paint-based approximation here.
std::vector<unsigned int> rotation_filaments(const Print &print);

// Parse image_map_component_filaments: 1-based filament numbers separated by
// commas, spaces or semicolons ("1,2,3,4"). Returns them 0-based, in the order
// given (that order is the layer rotation order, as with the origin's zone
// component list), with duplicates, out-of-range numbers and anything that is
// not a number dropped. Empty input yields an empty list, meaning "derive the
// rotation from what the layers print".
std::vector<unsigned int> parse_component_filaments(const std::string &text, size_t num_filaments);

// Balanced per-layer sequence over the rotation set.
// Port of build_balanced_component_sequence() (TextureMapping.cpp); Phase 1
// uses equal weights, which yields a plain cycle (e.g. C->M->Y->K->C->...).
std::vector<unsigned int> rotation_sequence(const std::vector<unsigned int> &rotation,
                                            const std::vector<int>          &weights = {});

// Active filament for a layer. Port of the tail of
// TextureMappingManager::resolve_zone_component(): sequence[layer % size].
unsigned int active_filament_for_layer(const std::vector<unsigned int> &sequence, int layer_index);

// Parse "#RRGGBB" (extra characters such as an alpha suffix are ignored) into
// linear 0..1 sRGB components. Port of parse_hex_color() (TextureMapping.cpp).
bool parse_filament_color(const std::string &hex, std::array<float, 3> &rgb_out);

// Generic color solver over the rotation set: converts the color of a painted
// target filament into per-rotation-filament deposition weights.
// Port of the generic-solver branch of component_weights_for_sample()
// (TextureMappingOffset.cpp), backed by the vendored ColorSolver library.
class Solver
{
public:
    Solver() = default;

    // Reads filament_colour and the image_map_generic_solver_* options.
    // Returns false (and leaves the solver invalid) when colors are missing.
    bool init(const PrintConfig &config, const std::vector<unsigned int> &rotation);

    bool valid() const { return m_valid; }
    bool in_rotation(unsigned int filament_0based) const;

    // Weight in [0,1] of `active_filament` when approximating the color of
    // `target_filament`; both 0-based. Returns a negative value when either
    // filament is unknown / outside the rotation. Results are cached.
    float weight_for(unsigned int target_filament, unsigned int active_filament);

private:
    const std::vector<float> &weights_for_target(unsigned int target_filament);

    bool                                m_valid { false };
    std::vector<unsigned int>           m_rotation;
    std::vector<std::array<float, 3>>   m_component_colors;
    std::vector<std::array<float, 3>>   m_filament_colors;
    ColorSolverCandidateCache           m_candidate_cache;
    const ColorSolverCandidateSet      *m_candidates { nullptr };
    ColorSolverLookupMode               m_lookup_mode { ColorSolverLookupMode::ClosestMix };
    ColorSolverMode                     m_solver_mode { ColorSolverMode::OklabSoftCap4Dark4 };
    ColorSolverMixModel                 m_mix_model { ColorSolverMixModel::PigmentPainter };
    std::map<unsigned int, std::vector<float>> m_weights_by_target;
    std::vector<float>                  m_empty_weights;
};

// Volumetric flow (mm^3/mm) scale factor when an extrusion of rounded-rectangle
// cross-section (width x height) is re-issued at new_width. Returns 1 when the
// inputs are degenerate.
double flow_scale_for_width_change(float old_width_mm, float new_width_mm, float height_mm);

// ---------------------------------------------------------------------------
// Phase 2: outer-wall surface offsetting (the "variable wall" half).
//
// Phase 1 changed only the external perimeter's *line width*, leaving the
// toolpath centerline where the perimeter generator put it. That is not what
// the origin does, and on its own it is geometrically wrong: narrowing an
// extrusion in place pulls the outer surface in by only half the width loss
// and simultaneously opens an equal gap on the *inner* side, between the outer
// wall and the wall behind it.
//
// The origin pairs every width change with a centerline shift (origin
// GCode.cpp ~11108: centerline_shift = base_centerline_shift + 0.5 * width_delta,
// applied along the segment's inward normal). Phase 2 ports that shift. Because
// the port's colour target is constant per painted region, the weight — and so
// the shift — is constant along a whole external perimeter loop, which lets the
// shift be applied once as a clean miter offset of the closed loop rather than
// the origin's per-segment point displacement.
// ---------------------------------------------------------------------------

// The two halves of the effect are independent and composable, each behind its
// own toggle (image_map_wall_width_enable / image_map_wall_offset_enable), with
// a third toggle (image_map_wall_combined_preset) running both at fixed,
// non-editable settings. See WallModulationSettings below.

// Direction the *offset* half is allowed to move the wall's surface, relative
// to where the perimeter generator put it.
enum class WallOffsetDirection {
    // Symmetric: weight 0..1 maps the surface to -d/2 .. +d/2 about nominal.
    // Largest visual swing for a given distance, but the model's outer
    // dimensions grow by up to d/2.
    Both = 0,
    // Inward only: weight 0..1 maps the surface to -d .. 0. The wall never
    // travels past its nominal position, so outer dimensions are preserved
    // exactly and the whole budget goes into receding.
    Inward = 1,
    // Outward only: weight 0..1 maps the surface to 0 .. +d. The wall never
    // cuts into the model, so nothing is ever under-filled behind it; the
    // matching layer bulges proud instead. Dimensions grow by up to d.
    Outward = 2,
};

// Sentinel for the three millimetre settings below: derive the value from the
// nozzle diameter and the path's own nominal width instead of taking it
// literally. Mirrors the convention Bambu Studio already uses for
// outer_wall_line_width, whose 0 means "work it out from line_width".
inline constexpr float kAutoFromNozzle = 0.f;

// Everything that decides how one external perimeter is modulated.
//
// The width half and the offset half are two different ways of making the
// wall's surface move, with different trade-offs, and they add:
//
//  * Width half (`modulate_width`) - a port of the origin's outer-wall
//    gradient. Re-widths the extrusion and shifts the centerline by half the
//    width change, which pins the wall's *inner* edge and moves only the outer
//    surface. Because it changes how much material is laid down, it changes
//    the colour's opacity as well as its position -- a receding layer is both
//    further back and thinner, so it reads much weaker. Its costs: it cannot
//    be applied to bridged/overhang material (re-widthing that is not safe),
//    and pushed hard it asks for extrusions below what a nozzle can lay down
//    (hence min_printable_width_mm(), which is not in the origin).
//  * Offset half (`offset_surface`) - a pure translation of the ring. Width
//    and volumetric flow are left EXACTLY as the perimeter generator set them,
//    so nothing can be too thin to extrude and overhang loops need no special
//    case. Its cost: because the whole extrusion moves, a wall pushed outward
//    opens an equally sized gap behind it, against the wall it used to touch.
//
// Running both is what the "combined" preset does: the width half provides most
// of the surface travel plus the opacity change, and the offset half adds the
// rest of the travel on top, with the total still clamped to
// max_surface_offset_mm(). See combined_preset_settings().
struct WallModulationSettings
{
    // --- width half (image_map_wall_width_enable + its settings) -----------
    bool  modulate_width      { false };
    // false: the wall may only narrow, so the surface only ever recedes into
    //        the model and outer dimensions never grow (the origin's non-vertex
    //        "offset gradient" mode).
    // true:  the wall may also exceed its nominal width, so the surface swings
    //        outward as well as inward, roughly doubling the colour range.
    //
    //        DEVIATION FROM THE ORIGIN (deliberate): the origin's
    //        vertex-color-match widens the wall about a centerline pre-shifted
    //        inward by 0.5 * (max_line_width - nominal), which leaves the wall's
    //        inner edge far inside the nominal one, overlapping the wall behind
    //        it. The origin gets away with that because it also insets the slice
    //        surfaces at perimeter-generation time (origin LayerRegion.cpp,
    //        texture_mapping_offset_surface_inset_mm) so the space is reserved.
    //        That slice-time inset is Phase 3 scope here, so instead the inner
    //        edge is pinned and only the outer surface moves: no over-extrusion
    //        into the inner walls, no reserved space needed, same visual effect.
    bool  allow_widening      { false };
    // These three are absolute millimetres, but 0 means "derive it from the
    // nozzle and this path" (see kAutoFromNozzle). A fixed millimetre default
    // cannot be right on every machine: 0.32 mm is a sane lower bound for the
    // 0.42 mm wall of a 0.4 mm nozzle, and complete nonsense for the 0.22 mm
    // wall of a 0.2 mm nozzle, where it exceeds the nominal width and collapses
    // the sweep to nothing (narrow-only) or pins the wall permanently 45-91%
    // over-extruded (widening). Auto keeps the same *proportions* on any nozzle.
    float config_min_width_mm { kAutoFromNozzle };  // texture_mapping_outer_wall_gradient_min_line_width
    float config_max_width_mm { kAutoFromNozzle };  // texture_mapping_outer_wall_gradient_max_line_width
    float width_strength_pct  { 100.f };            // texture_mapping_outer_wall_gradient_global_strength

    // --- offset half (image_map_wall_offset_enable + its settings) ---------
    bool                offset_surface     { false };
    // Also kAutoFromNozzle-aware: auto is the full surface-travel cap, which
    // "in and out" spends as +/- cap/2. Use the toggle, not a zero distance, to
    // turn the offset half off.
    float               offset_distance_mm { kAutoFromNozzle };  // image_map_wall_offset_distance
    WallOffsetDirection offset_direction   { WallOffsetDirection::Both };

    // --- context, filled in by the caller from the path / printer ----------
    float layer_height_mm    { 0.2f };
    float nozzle_diameter_mm { 0.4f };

    bool any_effect() const { return modulate_width || offset_surface; }
};

// The fixed settings behind image_map_wall_combined_preset: both halves on,
// nothing user-adjustable, sized off the nozzle so they are safe on any
// machine. The surface-travel budget (max_surface_offset_mm()) is split evenly:
// the width half sweeps +/- cap/2 about the nominal width, and the offset half
// adds another +/- cap/2 on top, for a full-scale swing of +/- cap.
// `config_min_width_mm` is additionally floored at min_printable_width_mm() by
// compute_wall_modulation(), so this can never request an unextrudable line.
WallModulationSettings combined_preset_settings(float path_nominal_width_mm,
                                                float layer_height_mm,
                                                float nozzle_diameter_mm);

// Result of modulating one external perimeter.
struct WallModulation
{
    // False when the modulation is a no-op and the caller should leave the
    // loop exactly as it found it.
    bool   active { false };
    // New extrusion width, mm. Equal to the path's nominal width whenever the
    // width half is off, in which case `width_changed` is false and the caller
    // must leave every path's width and mm3_per_mm alone.
    float  width_mm { 0.f };
    bool   width_changed { false };
    // Distance to move the toolpath centerline, mm. Positive moves the
    // centerline *toward the model's material* (so the outer surface recedes);
    // negative moves it away (the surface bulges outward). Zero (and
    // `ring_moves` false) when the two halves happen to cancel out, which is a
    // legitimate outcome the caller must not confuse with "no modulation":
    // the width change still has to be applied.
    float  centerline_shift_mm { 0.f };
    bool   ring_moves { false };
    // Multiplier for the path's mm3_per_mm, relative to its nominal width.
    double flow_scale { 1.0 };
    // Signed distance the outer surface ends up from where it would have been,
    // positive = outward. Diagnostics/tests only.
    float  surface_offset_mm { 0.f };
};

// Hard cap on how far the outer surface may move from its nominal position,
// in either direction. Port of TextureMappingManager::max_component_surface_offset_mm()
// (origin TextureMapping.cpp:2219): clamp(|nozzle diameter|, 0.01, 0.35) mm.
// The origin applies it as max_width_delta_limit = min(effective_delta,
// 2 * max_allowed_distance) (origin GCode.cpp:11026).
float max_surface_offset_mm(float nozzle_diameter_mm);

// Width + centerline shift for one external perimeter, composing whichever of
// the two halves `settings` has enabled.
//
// Width half: the centerline shift is chosen so the wall's inner edge stays
// exactly where the perimeter generator put it,
//     shift_width = 0.5 * (nominal_width - new_width)
// which is identical to the origin's offset-gradient shift (there
// base_outer_width == nominal, so 0.5 * width_delta == 0.5 * (nominal - new)),
// and which makes the outer surface move by exactly (new_width - nominal).
//
// Offset half: a pure translation on top, shift_offset = -surface_offset.
//
// The two add:
//     centerline_shift = 0.5 * (nominal - width) - offset
//     surface travel   = (width - nominal) + offset
// and the *total* surface travel is clamped to max_surface_offset_mm(); any
// excess is taken out of the offset half, which is the one with no lower bound
// of its own.
//
//   weight                in [0,1] from Solver::weight_for(); 1 = the active
//                         filament fully matches the painted target colour
//   path_nominal_width_mm the width the perimeter generator assigned
//   settings              the two halves and their settings; see above
WallModulation compute_wall_modulation(float                         weight,
                                       float                         path_nominal_width_mm,
                                       const WallModulationSettings &settings);

// Smallest width worth asking a nozzle to extrude: half the nozzle diameter
// (0.2 mm on a 0.4 mm nozzle), floored at 0.05 mm. The width-modulating modes
// clamp to this regardless of texture_mapping_outer_wall_gradient_min_line_width,
// whose own lower bound of 0.05 mm is small enough to request lines that do not
// physically extrude and leave voids in the wall.
float min_printable_width_mm(float nozzle_diameter_mm);

// Miter limit for offset_closed_ring(): the largest multiple of |delta| a
// vertex may be displaced before the join is beveled instead. 2.0 is Clipper's
// default and leaves every corner up to 120 degrees exactly mitered.
inline constexpr double kOffsetRingMiterLimit = 2.0;

// Offset a closed ring of points by `delta` (scaled units) using miter joins,
// preserving the vertex count so the caller can write the result back into the
// individual ExtrusionPaths that make up the ring.
//
// `delta` > 0 moves the ring toward its own interior. Callers wanting "toward
// the model's material" must negate it for hole loops, whose material is
// outside the ring.
//
// `max_displacement_limit` (scaled units, <= 0 to disable) is a hard ceiling on
// how far any single vertex may move, applied on top of the miter limit. Pass
// the surface-travel cap: a miter join displaces a vertex by
// |delta| / cos(turn/2), so without an absolute ceiling a sharp corner puts the
// wall well outside the envelope the feature promises, as a spike standing
// clear of the wall path.
//
// Returns false (leaving `out` untouched) when the ring is degenerate, when
// the offset would collapse or invert it, or when the arithmetic overflows —
// in which case the caller must leave the geometry unmodified.
bool offset_closed_ring(const Points &ring, double delta, Points &out,
                        double max_displacement_limit = 0.);

} // namespace ImageMapPerLayer
} // namespace Slic3r

#endif // slic3r_ImageMapPerLayerColor_hpp_
