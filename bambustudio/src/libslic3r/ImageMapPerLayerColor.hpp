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

// Map a solver weight to an outer-wall line width. Port of the non-vertex
// ("offset gradient") clamping formulas of GCode::_extrude() in origin
// GCode.cpp (lines ~11118-11144): the wall keeps its nominal width where the
// active filament fully matches the target color (weight 1) and narrows down
// toward the configured/safe minimum where it does not (weight 0), so the
// surrounding layers' colors show through.
//   base_outer_width_mm     nominal width of the path being modulated
//   config_min_width_mm     texture_mapping_outer_wall_gradient_min_line_width
//   config_max_width_mm     texture_mapping_outer_wall_gradient_max_line_width (upper cap)
//   global_strength_pct     texture_mapping_outer_wall_gradient_global_strength
//   layer_height_mm         used for the positive-spacing lower bound
//
// Kept as the width-only entry point (Phase 1 behaviour, and the width half of
// OuterWallMode::Inward). Phase 2 callers want compute_wall_modulation(),
// which additionally returns the centerline shift that actually moves the
// wall's outer surface.
float modulated_outer_wall_width(float weight,
                                 float base_outer_width_mm,
                                 float config_min_width_mm,
                                 float config_max_width_mm,
                                 float global_strength_pct,
                                 float layer_height_mm);

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

// How the outer wall's surface is allowed to move relative to where the
// unmodulated wall would have been.
enum class OuterWallMode {
    // Port of the origin's non-vertex "offset gradient" mode: the wall only
    // ever narrows, so its outer surface only ever recedes *into* the model,
    // by up to (nominal width - min width) * strength. The model never grows.
    Inward = 0,
    // Generalization of the origin's "vertex color match" mode: the width may
    // also exceed the path's nominal width, so the surface swings symmetrically
    // outward as well as inward around its nominal position. Gives a much
    // larger colour swing at the cost of changing outer dimensions by up to
    // the surface-offset cap (see max_surface_offset_mm()).
    //
    // DEVIATION FROM THE ORIGIN (deliberate): the origin's vertex-color-match
    // widens the wall about a centerline pre-shifted inward by
    // 0.5 * (max_line_width - nominal), which leaves the wall's *inner* edge
    // far inside the nominal one, overlapping the wall behind it. The origin
    // gets away with that because it also insets the slice surfaces at
    // perimeter-generation time (origin LayerRegion.cpp,
    // texture_mapping_offset_surface_inset_mm) so the space is reserved. That
    // slice-time inset is Phase 3 scope here, so instead this mode pins the
    // wall's inner edge to the nominal inner edge and lets only the outer
    // surface move. No over-extrusion into the inner walls, no reserved-space
    // requirement, same visual effect.
    Centered = 1,
};

// Result of modulating one external perimeter.
struct WallModulation
{
    // False when the modulation is a no-op and the caller should leave the
    // loop exactly as it found it.
    bool   active { false };
    // New extrusion width, mm.
    float  width_mm { 0.f };
    // Distance to move the toolpath centerline, mm. Positive moves the
    // centerline *toward the model's material* (so the outer surface recedes);
    // negative moves it away (the surface bulges outward).
    float  centerline_shift_mm { 0.f };
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

// Width + centerline shift for one external perimeter.
//
// The centerline shift is chosen so the wall's inner edge stays exactly where
// the perimeter generator put it:
//     shift = 0.5 * (nominal_width - new_width)
// which is identical to the origin's offset-gradient shift (there
// base_outer_width == nominal, so 0.5 * width_delta == 0.5 * (nominal - new)),
// and which makes the outer surface move by exactly (new_width - nominal).
//
//   weight                in [0,1] from Solver::weight_for(); 1 = the active
//                         filament fully matches the painted target colour
//   path_nominal_width_mm the width the perimeter generator assigned
//   config_min/max_mm     texture_mapping_outer_wall_gradient_{min,max}_line_width
//   global_strength_pct   texture_mapping_outer_wall_gradient_global_strength
//   layer_height_mm       for the positive-spacing lower bound
//   nozzle_diameter_mm    for max_surface_offset_mm()
WallModulation compute_wall_modulation(float         weight,
                                       float         path_nominal_width_mm,
                                       float         config_min_width_mm,
                                       float         config_max_width_mm,
                                       float         global_strength_pct,
                                       float         layer_height_mm,
                                       float         nozzle_diameter_mm,
                                       OuterWallMode mode);

// Offset a closed ring of points by `delta` (scaled units) using miter joins,
// preserving the vertex count so the caller can write the result back into the
// individual ExtrusionPaths that make up the ring.
//
// `delta` > 0 moves the ring toward its own interior. Callers wanting "toward
// the model's material" must negate it for hole loops, whose material is
// outside the ring.
//
// Returns false (leaving `out` untouched) when the ring is degenerate, when
// the offset would collapse or invert it, or when the arithmetic overflows —
// in which case the caller must leave the geometry unmodified.
bool offset_closed_ring(const Points &ring, double delta, Points &out);

} // namespace ImageMapPerLayer
} // namespace Slic3r

#endif // slic3r_ImageMapPerLayerColor_hpp_
