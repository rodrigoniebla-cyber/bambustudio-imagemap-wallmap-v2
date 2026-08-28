// Bambu Studio port of the "one tool-change per layer" image/texture color
// rotation from OrcaSlicer-ImageMap. See ImageMapPerLayerColor.hpp for the
// full provenance note.
//
//   Origin repo:  https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap
//                 (release v1.0.22, commit 92548381056dbf72836b0a1bdc455f238218dbfb)
//   Origin files: src/libslic3r/TextureMapping.cpp, TextureMappingOffset.cpp, GCode.cpp
//   Original author: sentientstardust. License: AGPLv3 (same as Bambu Studio).

#include "ImageMapPerLayerColor.hpp"

#include "Print.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace ImageMapPerLayer {

// ---------------------------------------------------------------------------
// Ported verbatim from OrcaSlicer-ImageMap src/libslic3r/TextureMapping.cpp
// (static helpers safe_mod / build_balanced_component_sequence).
// ---------------------------------------------------------------------------

static int safe_mod(int value, int divisor)
{
    if (divisor <= 0)
        return 0;
    int out = value % divisor;
    if (out < 0)
        out += divisor;
    return out;
}

static std::vector<unsigned int> build_balanced_component_sequence(const std::vector<unsigned int> &ids,
                                                                   const std::vector<int>          &weights)
{
    if (ids.empty())
        return {};

    std::vector<int> counts;
    counts.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const int weight = i < weights.size() ? std::max(0, weights[i]) : 1;
        counts.emplace_back(weight);
    }
    if (std::all_of(counts.begin(), counts.end(), [](int v) { return v <= 0; }))
        counts.assign(ids.size(), 1);

    int total = std::accumulate(counts.begin(), counts.end(), 0);
    constexpr int MaxCycle = 64;
    if (total > MaxCycle) {
        const double scale = double(MaxCycle) / double(total);
        for (int &count : counts)
            count = count <= 0 ? 0 : std::max(1, int(std::lround(double(count) * scale)));
        total = std::accumulate(counts.begin(), counts.end(), 0);
    }
    if (total <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(total));
    std::vector<int> debt(ids.size(), 0);
    for (int step = 0; step < total; ++step) {
        size_t best_idx = 0;
        int best_debt = std::numeric_limits<int>::lowest();
        for (size_t idx = 0; idx < counts.size(); ++idx) {
            debt[idx] += counts[idx];
            if (debt[idx] > best_debt) {
                best_debt = debt[idx];
                best_idx = idx;
            }
        }
        sequence.emplace_back(ids[best_idx]);
        debt[best_idx] -= total;
    }
    return sequence;
}

// ---------------------------------------------------------------------------

bool enabled(const PrintConfig &config)
{
    return config.image_map_per_layer_color_rotation.value;
}

bool parse_filament_color(const std::string &hex, std::array<float, 3> &rgb_out)
{
    // Port of parse_hex_color() (TextureMapping.cpp).
    if (hex.size() < 7 || hex[0] != '#')
        return false;
    int r = 0, g = 0, b = 0;
    try {
        r = std::stoi(hex.substr(1, 2), nullptr, 16);
        g = std::stoi(hex.substr(3, 2), nullptr, 16);
        b = std::stoi(hex.substr(5, 2), nullptr, 16);
    } catch (...) {
        return false;
    }
    rgb_out = { float(r) / 255.f, float(g) / 255.f, float(b) / 255.f };
    return true;
}

std::vector<unsigned int> rotation_filaments(const Print &print)
{
    const PrintConfig &config = print.config();
    const size_t num_filaments = config.filament_colour.values.size();
    if (num_filaments < 2)
        return {};

    // Filaments used for support anywhere in the print are protected: they are
    // never rotated so the existing support tool-change behavior (including on
    // multi-extruder / H2C machines) is left completely untouched.
    std::set<unsigned int> protected_filaments;
    for (const PrintObject *object : print.objects()) {
        const int support = object->config().support_filament.value;
        if (support > 0 && size_t(support) <= num_filaments)
            protected_filaments.insert(unsigned(support - 1));
        const int support_interface = object->config().support_interface_filament.value;
        if (support_interface > 0 && size_t(support_interface) <= num_filaments)
            protected_filaments.insert(unsigned(support_interface - 1));
    }

    // print.num_print_regions() enumerates one PrintRegionConfig per filament
    // slot *declared in the project's configuration*, regardless of whether
    // that slot is actually painted onto (or assigned to) this object's
    // geometry — Bambu Studio pre-creates a region per configured filament
    // slot. object_extruders() instead derives the truly-used set from each
    // volume's real MMU-paint / per-part filament data (ModelVolume::get_extruders()),
    // so it correctly excludes filament slots that exist in the project but
    // were never actually extruded (e.g. unrelated virtual/mixed slots, or
    // AMS colors simply not used on this plate).
    std::set<unsigned int> rotation;
    for (unsigned int filament : print.object_extruders())
        if (filament < num_filaments && protected_filaments.count(filament) == 0)
            rotation.insert(filament);

    return std::vector<unsigned int>(rotation.begin(), rotation.end());
}

std::vector<unsigned int> rotation_sequence(const std::vector<unsigned int> &rotation,
                                            const std::vector<int>          &weights)
{
    return build_balanced_component_sequence(rotation, weights);
}

unsigned int active_filament_for_layer(const std::vector<unsigned int> &sequence, int layer_index)
{
    // Port of the tail of TextureMappingManager::resolve_zone_component().
    if (sequence.empty())
        return 0;
    return sequence[size_t(safe_mod(layer_index, int(sequence.size())))];
}

// ---------------------------------------------------------------------------
// Solver
// ---------------------------------------------------------------------------

static ColorSolverMode effective_solver_mode_from_config(int mode)
{
    // Port of TextureMappingZone::effective_generic_solver_mode(): the
    // sentinel 255 (GenericSolverDefault) resolves to the slicer default
    // OklabSoftCap4Dark4; other values clamp to the valid enum range.
    constexpr int GenericSolverDefault = 255;
    if (mode == GenericSolverDefault)
        return ColorSolverMode::OklabSoftCap4Dark4;
    return color_solver_mode_from_index(std::clamp(mode, 0, 2));
}

bool Solver::init(const PrintConfig &config, const std::vector<unsigned int> &rotation)
{
    m_valid = false;
    m_rotation = rotation;
    m_component_colors.clear();
    m_filament_colors.clear();
    m_weights_by_target.clear();
    m_candidates = nullptr;
    if (rotation.size() < 2)
        return false;

    const std::vector<std::string> &colours = config.filament_colour.values;
    m_filament_colors.resize(colours.size());
    for (size_t i = 0; i < colours.size(); ++i)
        if (!parse_filament_color(colours[i], m_filament_colors[i]))
            m_filament_colors[i] = { 0.f, 0.f, 0.f };

    m_component_colors.reserve(rotation.size());
    for (unsigned int filament : rotation) {
        if (size_t(filament) >= m_filament_colors.size())
            return false;
        m_component_colors.emplace_back(m_filament_colors[filament]);
    }

    m_lookup_mode = color_solver_lookup_mode_from_index(config.image_map_generic_solver_lookup_mode.value);
    m_solver_mode = effective_solver_mode_from_config(config.image_map_generic_solver_mode.value);
    m_mix_model   = color_solver_mix_model_from_index(config.image_map_generic_solver_mix_model.value);

    // Port of the candidate-set construction used by the generic solver path
    // of component_weights_for_sample() (TextureMappingOffset.cpp).
    m_candidates = &color_solver_candidates(m_candidate_cache, m_component_colors, m_mix_model);
    if (m_candidates->empty()) {
        m_candidates = nullptr;
        return false;
    }
    m_valid = true;
    return true;
}

bool Solver::in_rotation(unsigned int filament_0based) const
{
    return std::find(m_rotation.begin(), m_rotation.end(), filament_0based) != m_rotation.end();
}

const std::vector<float> &Solver::weights_for_target(unsigned int target_filament)
{
    auto it = m_weights_by_target.find(target_filament);
    if (it != m_weights_by_target.end())
        return it->second;
    if (!m_valid || m_candidates == nullptr || size_t(target_filament) >= m_filament_colors.size())
        return m_empty_weights;

    const std::array<float, 3> &target_rgb = m_filament_colors[target_filament];
    // Port of the generic-solver branch of component_weights_for_sample():
    // solve_color_solver_weights_for_target() over the rotation candidates.
    std::vector<float> weights = solve_color_solver_weights_for_target(*m_candidates,
                                                                       target_rgb,
                                                                       m_lookup_mode,
                                                                       m_solver_mode);
    if (weights.size() != m_component_colors.size())
        weights.clear();
    for (float &w : weights)
        w = std::clamp(w, 0.f, 1.f);
    return m_weights_by_target.emplace(target_filament, std::move(weights)).first->second;
}

float Solver::weight_for(unsigned int target_filament, unsigned int active_filament)
{
    const auto active_it = std::find(m_rotation.begin(), m_rotation.end(), active_filament);
    if (active_it == m_rotation.end())
        return -1.f;
    const std::vector<float> &weights = weights_for_target(target_filament);
    if (weights.size() != m_rotation.size())
        return -1.f;
    return weights[size_t(active_it - m_rotation.begin())];
}

// ---------------------------------------------------------------------------
// Outer-wall width modulation
// ---------------------------------------------------------------------------

float modulated_outer_wall_width(float weight,
                                 float base_outer_width_mm,
                                 float config_min_width_mm,
                                 float config_max_width_mm,
                                 float global_strength_pct,
                                 float layer_height_mm)
{
    // Port of the clamping formulas of the outer-wall gradient path in origin
    // GCode.cpp (lines ~11118-11144, non-vertex "offset gradient" mode where
    // base_outer_width_mm = path_outer_width_mm):
    const float base = std::max(0.01f, base_outer_width_mm);
    const float safe_layer_height = std::max(0.01f, layer_height_mm);
    const float config_min = std::clamp(config_min_width_mm, 0.05f, base);
    const float min_width_for_positive_spacing = safe_layer_height * float(1. - 0.25 * M_PI) + 1e-4f;
    const float safe_min = std::clamp(std::max(config_min, min_width_for_positive_spacing), 0.05f, base);
    const float max_width_delta = std::max(0.f, base - safe_min);
    const float strength = std::clamp(global_strength_pct / 100.f, 0.f, 1.f);
    const float effective_delta = max_width_delta * strength;

    const float w = std::clamp(weight, 0.f, 1.f);
    float width = base - (1.f - w) * effective_delta;
    // texture_mapping_outer_wall_gradient_max_line_width acts as an absolute cap.
    width = std::min(width, std::max(0.05f, config_max_width_mm));
    return std::clamp(width, safe_min, base);
}

double flow_scale_for_width_change(float old_width_mm, float new_width_mm, float height_mm)
{
    // Rounded-rectangle extrusion cross-section, as used by Slic3r::Flow:
    //   area = h * (w - h * (1 - PI/4))
    const double h = double(height_mm);
    if (h <= 0.)
        return 1.;
    auto area = [h](double w) { return h * (w - h * (1. - 0.25 * M_PI)); };
    const double old_area = area(double(old_width_mm));
    const double new_area = area(double(new_width_mm));
    if (!(old_area > 0.) || !(new_area > 0.))
        return 1.;
    return new_area / old_area;
}

// ---------------------------------------------------------------------------
// Phase 2: outer-wall surface offsetting. See ImageMapPerLayerColor.hpp for the
// design note and the one deliberate deviation from the origin.
// ---------------------------------------------------------------------------

// Port of TextureMappingManager::max_component_surface_offset_mm()
// (origin TextureMapping.cpp:2219).
float max_surface_offset_mm(float nozzle_diameter_mm)
{
    const float safe_reference = std::max(0.05f, std::abs(nozzle_diameter_mm));
    return std::clamp(safe_reference, 0.01f, 0.35f);
}

WallModulation compute_wall_modulation(float         weight,
                                       float         path_nominal_width_mm,
                                       float         config_min_width_mm,
                                       float         config_max_width_mm,
                                       float         global_strength_pct,
                                       float         layer_height_mm,
                                       float         nozzle_diameter_mm,
                                       OuterWallMode mode)
{
    WallModulation out;

    const float nominal = std::max(0.01f, path_nominal_width_mm);
    const float layer_height = std::max(0.01f, layer_height_mm);
    const float strength = std::clamp(global_strength_pct / 100.f, 0.f, 1.f);
    const float cap = max_surface_offset_mm(nozzle_diameter_mm);

    // Lower bound on the width: the configured minimum, but never below the
    // width at which the rounded-rectangle cross-section's spacing turns
    // negative. Ported verbatim from origin GCode.cpp:10967
    // (min_width_for_positive_spacing_mm).
    const float min_width_for_positive_spacing = layer_height * float(1. - 0.25 * M_PI) + 1e-4f;
    const float hard_min = std::max(0.05f, min_width_for_positive_spacing);

    // The width sweep [lo, hi]. Both ends are clamped so the resulting surface
    // never moves further than `cap` from nominal, mirroring the origin's
    // max_width_delta_limit_mm = min(effective_delta, 2 * max_allowed_distance)
    // (origin GCode.cpp:11026) -- there the delta is one-sided about the base
    // width, here it is two-sided about the nominal width.
    float lo = std::max({config_min_width_mm, hard_min, nominal - cap});
    float hi = nominal;
    if (mode == OuterWallMode::Centered) {
        // Widening is what buys the outward half of the swing. Cap it both by
        // the user's configured maximum and by the surface-offset cap.
        hi = std::clamp(config_max_width_mm, nominal, nominal + cap);
    } else {
        // Inward mode still honours the configured maximum as an absolute
        // upper bound (origin GCode.cpp:11116 applies it the same way).
        hi = std::min(nominal, std::max(0.05f, config_max_width_mm));
    }
    lo = std::min(lo, hi);

    // weight 1 -> widest (the active filament matches the painted colour, so
    // deposit the full line); weight 0 -> narrowest.
    const float w = std::clamp(weight, 0.f, 1.f);
    const float raw_width = lo + w * (hi - lo);
    // The global strength scales the whole swing about the nominal width, so
    // strength 0 is an exact no-op in both modes.
    // Scaling about the nominal width keeps `width` inside [lo, hi] on its own;
    // the floor is a belt-and-braces guard on the positive-spacing bound.
    float width = std::max(nominal + (raw_width - nominal) * strength, std::min(hard_min, nominal));
    if (!std::isfinite(width) || width <= 0.f)
        return out;

    // Pin the wall's inner edge: shifting the centerline by half the width
    // change keeps (centerline - width/2) constant, so the surface moves by
    // exactly (width - nominal) and the wall behind this one is never
    // encroached on. Positive shift = toward the material = surface recedes.
    const float shift = 0.5f * (nominal - width);
    const float surface_offset = width - nominal;
    if (!std::isfinite(shift) || std::abs(surface_offset) > cap + 1e-4f)
        return out;

    out.width_mm = width;
    out.centerline_shift_mm = shift;
    out.surface_offset_mm = surface_offset;
    out.flow_scale = flow_scale_for_width_change(nominal, width, layer_height);
    if (!std::isfinite(out.flow_scale) || out.flow_scale <= 0.)
        return WallModulation{};
    // Below ~1 um of surface movement there is nothing worth perturbing the
    // toolpath for.
    out.active = std::abs(surface_offset) > 1e-3f;
    return out;
}

bool offset_closed_ring(const Points &ring, double delta, Points &out)
{
    const size_t n = ring.size();
    if (n < 3 || !std::isfinite(delta))
        return false;
    if (std::abs(delta) <= SCALED_EPSILON)
        return false;

    // Signed area (twice) via the shoelace formula, in doubles: coordinates are
    // scaled int64 and their products overflow 32-bit arithmetic easily.
    auto signed_area2 = [](const Points &pts) {
        double acc = 0.;
        for (size_t i = 0, m = pts.size(); i < m; ++i) {
            const Point &a = pts[i];
            const Point &b = pts[(i + 1) % m];
            acc += double(a.x()) * double(b.y()) - double(b.x()) * double(a.y());
        }
        return acc;
    };

    const double area2 = signed_area2(ring);
    if (!std::isfinite(area2) || std::abs(area2) <= EPSILON)
        return false;
    // For a CCW ring (positive area) the interior lies to the left of every
    // directed edge, i.e. along (-dy, dx).
    const double orientation = area2 > 0. ? 1. : -1.;

    // Unit interior normal of the edge leaving vertex i.
    std::vector<double> nx(n, 0.), ny(n, 0.);
    std::vector<bool>   have_normal(n, false);
    for (size_t i = 0; i < n; ++i) {
        const Point &a = ring[i];
        const Point &b = ring[(i + 1) % n];
        const double dx = double(b.x()) - double(a.x());
        const double dy = double(b.y()) - double(a.y());
        const double len = std::hypot(dx, dy);
        if (len <= EPSILON)
            continue; // zero-length edge (duplicate point): filled in below
        nx[i] = orientation * (-dy / len);
        ny[i] = orientation * (dx / len);
        have_normal[i] = true;
    }
    // Duplicate points carry no direction of their own; give them the nearest
    // preceding real edge's normal so the miter below stays well-defined. The
    // ring has at least one real edge, or its area would have been zero.
    {
        size_t last = n;
        for (size_t pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < n; ++i) {
                if (have_normal[i]) {
                    last = i;
                } else if (last < n) {
                    nx[i] = nx[last];
                    ny[i] = ny[last];
                    have_normal[i] = true;
                }
            }
        if (std::find(have_normal.begin(), have_normal.end(), false) != have_normal.end())
            return false;
    }

    // A miter join can run away at a near-reversal corner; the origin clamps
    // its per-segment shift too (clamped_shift_coord_for_gcode, bounded by
    // scale_(max(0.5, base_outer_width))). Cap the displacement at 4x the
    // requested offset, which leaves ordinary corners exact.
    const double max_displacement = 4. * std::abs(delta);

    Points offset;
    offset.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        // Vertex i joins the edge arriving from i-1 and the edge leaving i.
        const size_t prev = (i + n - 1) % n;
        const double pnx = nx[prev], pny = ny[prev];
        const double cnx = nx[i], cny = ny[i];
        const double denom = 1. + (pnx * cnx + pny * cny);

        double vx, vy;
        if (denom <= 1e-6) {
            // Corner folds back on itself; the miter point is at infinity. Fall
            // back to the (bevel-like) average normal.
            const double ax = pnx + cnx;
            const double ay = pny + cny;
            const double len = std::hypot(ax, ay);
            if (len <= EPSILON) {
                vx = delta * cnx;
                vy = delta * cny;
            } else {
                vx = delta * ax / len;
                vy = delta * ay / len;
            }
        } else {
            vx = delta * (pnx + cnx) / denom;
            vy = delta * (pny + cny) / denom;
        }

        const double displacement = std::hypot(vx, vy);
        if (!std::isfinite(displacement))
            return false;
        if (displacement > max_displacement && displacement > EPSILON) {
            const double s = max_displacement / displacement;
            vx *= s;
            vy *= s;
        }

        const double x = double(ring[i].x()) + vx;
        const double y = double(ring[i].y()) + vy;
        if (!std::isfinite(x) || !std::isfinite(y) ||
            std::abs(x) > double(std::numeric_limits<coord_t>::max()) ||
            std::abs(y) > double(std::numeric_limits<coord_t>::max()))
            return false;
        offset.emplace_back(coord_t(std::llround(x)), coord_t(std::llround(y)));
    }

    // Reject the offset if it overran the feature. Leaving the geometry alone
    // is always a safe outcome for the caller.
    const double offset_area2 = signed_area2(offset);
    if (!std::isfinite(offset_area2))
        return false;
    // The winding must survive.
    if (offset_area2 * area2 <= 0.)
        return false;
    // An inward offset has to shrink the ring, an outward one has to grow it.
    // The winding check above is NOT sufficient on its own: mitering a square
    // narrower than twice the offset inward turns it inside out but re-emerges
    // as a *larger* square of the same winding, which passes a sign test.
    const double area_before = std::abs(area2);
    const double area_after  = std::abs(offset_area2);
    if (delta > 0. ? area_after >= area_before : area_after <= area_before)
        return false;
    // ...and an inward offset must leave something behind.
    if (delta > 0. && area_after < 0.1 * area_before)
        return false;

    out = std::move(offset);
    return true;
}

} // namespace ImageMapPerLayer
} // namespace Slic3r
