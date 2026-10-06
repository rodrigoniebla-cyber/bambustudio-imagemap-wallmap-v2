#include <catch2/catch.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ImageMapPerLayerColor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include "test_data.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

DynamicPrintConfig make_imagemap_config(unsigned num_filaments, bool enable_rotation)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(num_filaments);

    std::vector<double> filament_diameters(num_filaments, 1.75);
    std::vector<std::string> filament_colours;
    std::vector<std::string> filament_types;
    filament_colours.reserve(num_filaments);
    filament_types.reserve(num_filaments);
    static const char *palette[] = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    for (unsigned i = 0; i < num_filaments; ++i) {
        filament_colours.emplace_back(palette[i % 4]);
        filament_types.emplace_back("PLA");
    }

    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("filament_diameter", new ConfigOptionFloats(filament_diameters));
    config.set_key_value("filament_colour", new ConfigOptionStrings(filament_colours));
    config.set_key_value("filament_type", new ConfigOptionStrings(filament_types));
    config.set_key_value("image_map_per_layer_color_rotation", new ConfigOptionBool(enable_rotation));
    return config;
}

void add_cube_object(Model &model, double offset_x, int extruder)
{
    ModelObject *object = model.add_object();
    object->add_volume(mesh(TestMesh::cube_20x20x20));
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(offset_x, 0.0, 0.0));
    object->config.set_key_value("extruder", new ConfigOptionInt(extruder));
}

void build_imagemap_print(Print &print, Model &model, const DynamicPrintConfig &config_in, unsigned num_objects)
{
    model.clear_objects();
    for (unsigned i = 0; i < num_objects; ++i)
        add_cube_object(model, 30.0 * double(i), int(i + 1));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(config_in.option<ConfigOptionFloats>("filament_diameter")->size());
    config.apply(config_in);

    for (ModelObject *mo : model.objects)
        mo->ensure_on_bed();

    print.apply(model, config);
    print.set_status_silent();
}

} // namespace

// print.apply() alone (no process()/slicing) already resolves per-region
// wall_filament / solid_infill_filament / sparse_infill_filament from each
// object's "extruder" override -- rotation_filaments() only reads print
// regions, so this exercises the real port logic against real Print /
// PrintRegion objects without going through mesh slicing, skirt/brim, or
// G-code export (whose validation requirements for a synthetic multi-nozzle
// test print are unrelated to this feature and out of scope here).
TEST_CASE("imagemap: rotation_filaments() finds the distinct filaments used by real print regions", "[ImageMapPerLayerColor]")
{
    Model model;
    Print print;
    build_imagemap_print(print, model, make_imagemap_config(3, true), 3);

    std::vector<unsigned int> rotation = Slic3r::ImageMapPerLayer::rotation_filaments(print);
    REQUIRE(rotation.size() == 3);
    REQUIRE(std::find(rotation.begin(), rotation.end(), 0u) != rotation.end());
    REQUIRE(std::find(rotation.begin(), rotation.end(), 1u) != rotation.end());
    REQUIRE(std::find(rotation.begin(), rotation.end(), 2u) != rotation.end());

    // Balanced rotation sequence must cycle through all three filaments, and
    // active_filament_for_layer() must reproduce that cycle across layers --
    // this is the actual "one tool-change per layer" mechanism.
    std::vector<unsigned int> sequence = Slic3r::ImageMapPerLayer::rotation_sequence(rotation);
    REQUIRE(sequence.size() == rotation.size());
    for (int layer = 0; layer < 9; ++layer) {
        unsigned int active = Slic3r::ImageMapPerLayer::active_filament_for_layer(sequence, layer);
        REQUIRE(active == sequence[size_t(layer) % sequence.size()]);
    }
}

TEST_CASE("imagemap: rotation set is empty when fewer than 2 filaments are used", "[ImageMapPerLayerColor]")
{
    Model model;
    Print print;
    build_imagemap_print(print, model, make_imagemap_config(1, true), 1);
    REQUIRE(Slic3r::ImageMapPerLayer::rotation_filaments(print).empty());
}

// ---------------------------------------------------------------------------
// The wall map: the two composable halves (line-width modulation and surface
// offsetting) and the fixed "combined" preset that runs both.
// ---------------------------------------------------------------------------

namespace {

using Slic3r::ImageMapPerLayer::WallModulation;
using Slic3r::ImageMapPerLayer::WallModulationSettings;
using Slic3r::ImageMapPerLayer::WallOffsetDirection;

constexpr float kNominal     = 0.42f;
constexpr float kMinWidth    = 0.32f;
constexpr float kMaxWidth    = 0.95f;
constexpr float kLayerHeight = 0.20f;
constexpr float kNozzle      = 0.40f;
constexpr float kCap         = 0.35f;   // max_surface_offset_mm(0.4)

WallModulationSettings base_settings()
{
    WallModulationSettings s;
    s.config_min_width_mm = kMinWidth;
    s.config_max_width_mm = kMaxWidth;
    s.width_strength_pct  = 100.f;
    s.offset_distance_mm  = 0.15f;
    s.layer_height_mm     = kLayerHeight;
    s.nozzle_diameter_mm  = kNozzle;
    return s;
}

// Toggle 1 only: vary the line width.
WallModulation width_only(float weight, bool allow_widening, float strength = 100.f,
                          float min_width = kMinWidth, float max_width = kMaxWidth)
{
    WallModulationSettings s = base_settings();
    s.modulate_width      = true;
    s.allow_widening      = allow_widening;
    s.width_strength_pct  = strength;
    s.config_min_width_mm = min_width;
    s.config_max_width_mm = max_width;
    return Slic3r::ImageMapPerLayer::compute_wall_modulation(weight, kNominal, s);
}

// Toggle 2 only: move the surface.
WallModulation offset_only(float weight, float distance, WallOffsetDirection direction)
{
    WallModulationSettings s = base_settings();
    s.offset_surface     = true;
    s.offset_distance_mm = distance;
    s.offset_direction   = direction;
    return Slic3r::ImageMapPerLayer::compute_wall_modulation(weight, kNominal, s);
}

// Toggle 3: the fixed preset.
WallModulation combined(float weight)
{
    return Slic3r::ImageMapPerLayer::compute_wall_modulation(
        weight, kNominal,
        Slic3r::ImageMapPerLayer::combined_preset_settings(kNominal, kLayerHeight, kNozzle));
}

// Where the wall's inner edge ends up, relative to the nominal inner edge.
// The centerline shift is positive toward the material, so the inner edge sits
// at (-shift - width/2) and the nominal one at (-nominal/2).
float inner_edge_error(const WallModulation &m)
{
    return (-m.centerline_shift_mm - 0.5f * m.width_mm) - (-0.5f * kNominal);
}

} // namespace

TEST_CASE("imagemap: all three toggles off is an exact no-op", "[ImageMapPerLayerColor]")
{
    // The wall map's entry point is reached whenever the master rotation toggle
    // is on, even with every wall-map toggle off. That state must modulate
    // nothing at all -- tool-change collapsing without any geometry change.
    WallModulationSettings s = base_settings();
    REQUIRE(!s.any_effect());
    for (int i = 0; i <= 4; ++i) {
        const WallModulation m = Slic3r::ImageMapPerLayer::compute_wall_modulation(float(i) / 4.f, kNominal, s);
        REQUIRE(!m.active);
        REQUIRE(!m.width_changed);
        REQUIRE(!m.ring_moves);
    }
}

TEST_CASE("imagemap: width toggle, narrow-only never grows and never moves the inner edge", "[ImageMapPerLayerColor]")
{
    // weight 1 == the active filament matches the painted target: full width,
    // no shift, nothing to do.
    const WallModulation full = width_only(1.f, /*allow_widening=*/false);
    REQUIRE(!full.active);

    const WallModulation none = width_only(0.f, /*allow_widening=*/false);
    REQUIRE(none.active);
    REQUIRE(none.width_changed);
    REQUIRE(none.ring_moves);
    // Narrowed to the configured minimum...
    REQUIRE(none.width_mm == Approx(kMinWidth).margin(1e-4));
    // ...with the surface pulled in by the *full* width loss, not half of it.
    REQUIRE(none.surface_offset_mm == Approx(kMinWidth - kNominal).margin(1e-4));
    REQUIRE(none.surface_offset_mm < 0.f);
    // The centerline moves toward the material, keeping the inner edge put.
    REQUIRE(none.centerline_shift_mm > 0.f);
    REQUIRE(inner_edge_error(none) == Approx(0.).margin(1e-4));
    // Less material per mm, since the line got thinner.
    REQUIRE(none.flow_scale < 1.0);

    // Monotone in the weight, and never wider than nominal.
    float previous = -1e9f;
    for (int i = 0; i <= 10; ++i) {
        const WallModulation m = width_only(float(i) / 10.f, /*allow_widening=*/false);
        REQUIRE(m.width_mm >= previous);
        REQUIRE(m.width_mm <= kNominal + 1e-4f);
        REQUIRE(inner_edge_error(m) == Approx(0.).margin(1e-4));
        previous = m.width_mm;
    }
}

TEST_CASE("imagemap: width toggle, 'allow widening' swings the surface both ways", "[ImageMapPerLayerColor]")
{
    const WallModulation full = width_only(1.f, /*allow_widening=*/true);
    const WallModulation none = width_only(0.f, /*allow_widening=*/true);

    REQUIRE(full.active);
    REQUIRE(none.active);
    REQUIRE(full.surface_offset_mm > 0.f);
    REQUIRE(none.surface_offset_mm < 0.f);
    // Widening means the centerline shifts away from the material.
    REQUIRE(full.centerline_shift_mm < 0.f);
    REQUIRE(none.centerline_shift_mm > 0.f);
    REQUIRE(full.flow_scale > 1.0);
    REQUIRE(none.flow_scale < 1.0);

    // The inner edge stays pinned in both directions -- this is what keeps the
    // widened wall from over-extruding into the wall behind it.
    REQUIRE(inner_edge_error(full) == Approx(0.).margin(1e-4));
    REQUIRE(inner_edge_error(none) == Approx(0.).margin(1e-4));

    // Strictly more colour range than narrow-only.
    const WallModulation narrow_none = width_only(0.f, /*allow_widening=*/false);
    const float widening_range = full.surface_offset_mm - none.surface_offset_mm;
    const float narrow_range   = 0.f - narrow_none.surface_offset_mm;
    REQUIRE(widening_range > narrow_range);
}

TEST_CASE("imagemap: surface movement is capped at the nozzle-derived limit", "[ImageMapPerLayerColor]")
{
    // Port of TextureMappingManager::max_component_surface_offset_mm().
    REQUIRE(Slic3r::ImageMapPerLayer::max_surface_offset_mm(0.40f) == Approx(kCap));
    REQUIRE(Slic3r::ImageMapPerLayer::max_surface_offset_mm(0.20f) == Approx(0.20f));
    REQUIRE(Slic3r::ImageMapPerLayer::max_surface_offset_mm(1.00f) == Approx(kCap));

    // The origin's bounds: the surface moves out by at most cap and in by at
    // most 2 * cap (a width change of 2 * cap, whose paired centerline shift is
    // cap), and the centerline itself never moves by more than cap.
    auto within_bounds = [](const WallModulation &m) {
        return m.surface_offset_mm <= kCap + 1e-3f && m.surface_offset_mm >= -2.f * kCap - 1e-3f &&
               std::abs(m.centerline_shift_mm) <= kCap + 1e-3f;
    };
    // An absurd configured max must not translate into an absurd excursion,
    // in any combination of the two halves.
    for (int i = 0; i <= 10; ++i) {
        const float w = float(i) / 10.f;
        REQUIRE(within_bounds(width_only(w, true, 100.f, 0.05f, 3.0f)));
        // The offset half alone moves the whole wall, so it is bound by the
        // centerline limit: never more than cap either way.
        REQUIRE(std::abs(offset_only(w, 10.0f, WallOffsetDirection::Both).surface_offset_mm) <= kCap + 1e-3f);
        REQUIRE(std::abs(offset_only(w, 10.0f, WallOffsetDirection::Inward).surface_offset_mm) <= kCap + 1e-3f);
        REQUIRE(std::abs(offset_only(w, 10.0f, WallOffsetDirection::Outward).surface_offset_mm) <= kCap + 1e-3f);
        REQUIRE(std::abs(combined(w).surface_offset_mm) <= kCap + 1e-3f);

        // ...and neither does asking both halves at once for far too much.
        WallModulationSettings s = base_settings();
        s.modulate_width      = true;
        s.allow_widening      = true;
        s.config_min_width_mm = 0.05f;
        s.config_max_width_mm = 3.0f;
        s.offset_surface      = true;
        s.offset_distance_mm  = 10.0f;
        const WallModulation m = Slic3r::ImageMapPerLayer::compute_wall_modulation(w, kNominal, s);
        REQUIRE(within_bounds(m));

        // Same on a wide wall, where the width half alone can reach both limits.
        s.offset_direction = WallOffsetDirection::Inward;
        REQUIRE(within_bounds(Slic3r::ImageMapPerLayer::compute_wall_modulation(w, 0.95f, s)));
    }
}

// The origin's default for image textures (vertex-colour match) prints the
// outer wall at texture_mapping_outer_wall_gradient_max_line_width, 0.95 mm,
// and narrows it to 0.32 mm on layers whose filament is not wanted: a 0.63 mm
// swing, min(0.95 - 0.32, 2 * 0.35). With the outer wall line width set to
// 0.95 mm (so the perimeter generator reserves the room the origin reserves by
// insetting the slice), narrow-only width modulation must reproduce that.
TEST_CASE("imagemap: a wide outer wall gets the origin's full width swing", "[ImageMapPerLayerColor]")
{
    WallModulationSettings s = base_settings();
    s.modulate_width      = true;
    s.allow_widening      = false;
    s.config_min_width_mm = 0.32f;
    s.config_max_width_mm = 0.95f;
    const WallModulation full = Slic3r::ImageMapPerLayer::compute_wall_modulation(1.f, 0.95f, s);
    const WallModulation none = Slic3r::ImageMapPerLayer::compute_wall_modulation(0.f, 0.95f, s);
    REQUIRE(!full.active);
    REQUIRE(none.active);
    REQUIRE(none.width_mm == Approx(0.32f).margin(1e-4));
    REQUIRE(none.surface_offset_mm == Approx(-0.63f).margin(1e-4));
    REQUIRE(none.centerline_shift_mm == Approx(0.315f).margin(1e-4));
    REQUIRE(none.centerline_shift_mm <= kCap);
}

TEST_CASE("imagemap: zero strength disables the width half without disabling the offset half", "[ImageMapPerLayerColor]")
{
    for (bool allow_widening : {false, true})
        for (int i = 0; i <= 4; ++i)
            REQUIRE(!width_only(float(i) / 4.f, allow_widening, 0.f).active);

    // The strength setting belongs to the width half only: zeroing it must not
    // reach across and kill an independently enabled surface offset.
    WallModulationSettings s = base_settings();
    s.modulate_width     = true;
    s.width_strength_pct = 0.f;
    s.offset_surface     = true;
    s.offset_distance_mm = 0.15f;
    const WallModulation m = Slic3r::ImageMapPerLayer::compute_wall_modulation(0.f, kNominal, s);
    REQUIRE(m.active);
    REQUIRE(!m.width_changed);
    REQUIRE(m.width_mm == Approx(kNominal));
    REQUIRE(m.flow_scale == Approx(1.0));
    REQUIRE(m.surface_offset_mm == Approx(-0.5f * 0.15f).margin(1e-4));

    // A zero offset distance is NOT a no-op: 0 means "derive from the nozzle"
    // (kAutoFromNozzle), because a fixed millimetre default cannot suit every
    // nozzle. Turning the offset half off is the toggle's job, not a zero
    // distance's -- compute_wall_modulation() is never reached with the half
    // enabled and no effect intended.
    const float cap = Slic3r::ImageMapPerLayer::max_surface_offset_mm(kNozzle);
    REQUIRE(offset_only(1.f, Slic3r::ImageMapPerLayer::kAutoFromNozzle, WallOffsetDirection::Both)
                .surface_offset_mm == Approx(+0.5f * cap).margin(1e-3));
    REQUIRE(offset_only(0.f, Slic3r::ImageMapPerLayer::kAutoFromNozzle, WallOffsetDirection::Both)
                .surface_offset_mm == Approx(-0.5f * cap).margin(1e-3));

    // Clearing the toggle is what makes it inert.
    WallModulationSettings off = base_settings();
    off.offset_surface = false;
    for (int i = 0; i <= 4; ++i)
        REQUIRE(!Slic3r::ImageMapPerLayer::compute_wall_modulation(float(i) / 4.f, kNominal, off).active);
}

// This is the regression that produced the voids on a real Benchy: the project
// had texture_mapping_outer_wall_gradient_min_line_width cranked to its 0.05 mm
// floor, and the width half honoured it literally, asking for ~0.07 mm external
// perimeters on a 0.4 mm nozzle. Those do not extrude.
TEST_CASE("imagemap: the width half never requests an unextrudable line", "[ImageMapPerLayerColor]")
{
    REQUIRE(Slic3r::ImageMapPerLayer::min_printable_width_mm(0.40f) == Approx(0.20f));
    REQUIRE(Slic3r::ImageMapPerLayer::min_printable_width_mm(0.20f) == Approx(0.10f));
    REQUIRE(Slic3r::ImageMapPerLayer::min_printable_width_mm(0.06f) == Approx(0.05f)); // 0.05 mm floor

    const float floor_mm = Slic3r::ImageMapPerLayer::min_printable_width_mm(kNozzle);
    for (bool allow_widening : {false, true})
        for (int i = 0; i <= 10; ++i) {
            // The exact configuration off the failing Benchy project: min 0.05,
            // max 3.0, full strength.
            const WallModulation m = width_only(float(i) / 10.f, allow_widening, 100.f, 0.05f, 3.0f);
            REQUIRE(m.width_mm >= floor_mm - 1e-4f);
        }
    for (int i = 0; i <= 10; ++i)
        REQUIRE(combined(float(i) / 10.f).width_mm >= floor_mm - 1e-4f);
}

TEST_CASE("imagemap: offset toggle moves the wall without touching width or flow", "[ImageMapPerLayerColor]")
{
    const float d = 0.15f;

    // "In and out": symmetric swing, width/flow left strictly alone.
    const WallModulation full = offset_only(1.f, d, WallOffsetDirection::Both);
    const WallModulation none = offset_only(0.f, d, WallOffsetDirection::Both);
    const WallModulation mid  = offset_only(0.5f, d, WallOffsetDirection::Both);

    REQUIRE(full.width_mm == Approx(kNominal));
    REQUIRE(none.width_mm == Approx(kNominal));
    REQUIRE(!full.width_changed);
    REQUIRE(!none.width_changed);
    REQUIRE(full.flow_scale == Approx(1.0));
    REQUIRE(none.flow_scale == Approx(1.0));

    REQUIRE(full.surface_offset_mm == Approx(+0.5f * d).margin(1e-4));
    REQUIRE(none.surface_offset_mm == Approx(-0.5f * d).margin(1e-4));
    REQUIRE(!mid.active); // dead centre: nothing to move

    // The shift is the negation of the surface movement: positive shift shoves
    // the centerline toward the material, so the surface recedes.
    REQUIRE(full.centerline_shift_mm == Approx(-full.surface_offset_mm).margin(1e-6));
    REQUIRE(none.centerline_shift_mm == Approx(-none.surface_offset_mm).margin(1e-6));

    // "Inward only": never grows the part, spends the whole distance receding.
    REQUIRE(!offset_only(1.f, d, WallOffsetDirection::Inward).active);  // weight 1 == nominal position
    REQUIRE(offset_only(0.f, d, WallOffsetDirection::Inward).surface_offset_mm == Approx(-d).margin(1e-4));

    // "Outward only": never cuts into the model, stands proud on a match.
    REQUIRE(!offset_only(0.f, d, WallOffsetDirection::Outward).active); // weight 0 == nominal position
    REQUIRE(offset_only(1.f, d, WallOffsetDirection::Outward).surface_offset_mm == Approx(+d).margin(1e-4));

    for (int i = 0; i <= 10; ++i) {
        REQUIRE(offset_only(float(i) / 10.f, d, WallOffsetDirection::Inward).surface_offset_mm <= 1e-4f);
        REQUIRE(offset_only(float(i) / 10.f, d, WallOffsetDirection::Outward).surface_offset_mm >= -1e-4f);
    }

    // Monotone in the weight, all three directions, width never touched.
    for (WallOffsetDirection direction : {WallOffsetDirection::Both, WallOffsetDirection::Inward, WallOffsetDirection::Outward}) {
        float previous = -1e9f;
        for (int i = 0; i <= 10; ++i) {
            const WallModulation m = offset_only(float(i) / 10.f, d, direction);
            REQUIRE(m.surface_offset_mm >= previous);
            REQUIRE(m.width_mm == Approx(kNominal));
            REQUIRE(!m.width_changed);
            previous = m.surface_offset_mm;
        }
    }
}

TEST_CASE("imagemap: the two halves add up", "[ImageMapPerLayerColor]")
{
    const float narrowing = kNominal - kMinWidth;   // 0.10 mm, inside the cap
    const float d         = 0.05f;

    WallModulationSettings s = base_settings();
    s.modulate_width      = true;
    s.allow_widening      = false;
    s.offset_surface      = true;
    s.offset_distance_mm  = d;
    s.offset_direction    = WallOffsetDirection::Outward;

    // weight 0: the width bottoms out at the configured minimum (surface
    // -narrowing) and the outward offset contributes nothing yet.
    const WallModulation none = Slic3r::ImageMapPerLayer::compute_wall_modulation(0.f, kNominal, s);
    REQUIRE(none.active);
    REQUIRE(none.width_mm == Approx(kMinWidth).margin(1e-4));
    REQUIRE(none.surface_offset_mm == Approx(-narrowing).margin(1e-4));

    // Halfway: the width has recovered half the narrowing (surface at
    // -narrowing/2) and the outward offset adds +d/2 on top. The two really do
    // add rather than one overriding the other.
    const WallModulation mid = Slic3r::ImageMapPerLayer::compute_wall_modulation(0.5f, kNominal, s);
    REQUIRE(mid.width_mm == Approx(kNominal - 0.5f * narrowing).margin(1e-4));
    REQUIRE(mid.surface_offset_mm == Approx(-0.5f * narrowing + 0.5f * d).margin(1e-4));
    REQUIRE(mid.width_changed);
    REQUIRE(mid.flow_scale < 1.0);
}

TEST_CASE("imagemap: a re-widthed loop whose ring does not move is still applied", "[ImageMapPerLayerColor]")
{
    // The two halves' *centerline* contributions can cancel exactly: the width
    // half shifts the centerline in by half the width loss to pin the inner
    // edge, and an outward offset of the same size shifts it back out. The ring
    // then stays exactly where it was -- but the extrusion is genuinely
    // narrower, so this is a real modulation, not a no-op. Reporting it as
    // inactive (or making the caller's ring offset a precondition for applying
    // the width) silently drops the width change on the floor.
    //
    // Narrow-only, min 0.32 on a 0.42 nominal, outward offset d = 0.05:
    //   at weight 0.5, width = 0.37, shift_width = +0.025, offset = +0.025.
    WallModulationSettings s = base_settings();
    s.modulate_width      = true;
    s.allow_widening      = false;
    s.offset_surface      = true;
    s.offset_distance_mm  = 0.05f;
    s.offset_direction    = WallOffsetDirection::Outward;

    const WallModulation m = Slic3r::ImageMapPerLayer::compute_wall_modulation(0.5f, kNominal, s);
    REQUIRE(m.centerline_shift_mm == Approx(0.f).margin(1e-4));
    REQUIRE(!m.ring_moves);        // nothing for offset_closed_ring() to do...
    REQUIRE(m.width_changed);      // ...but the width still has to be written
    REQUIRE(m.active);
    REQUIRE(m.width_mm < kNominal - 1e-3f);
    REQUIRE(m.flow_scale < 1.0);
}

// A 0.2 mm nozzle prints 0.22 mm walls (BBL's fdm_process_single_*_nozzle_0.2
// profiles), against 0.42 mm on a 0.4 mm nozzle. The wall map's three millimetre
// settings used to default to fixed values picked for the 0.4: a 0.32 mm minimum
// line width is *wider* than the whole 0.22 mm wall, which collapsed the sweep to
// a single point (narrow-only: the toggle did nothing whatsoever) or pinned the
// wall permanently 45-91% over its nominal width and never let it recede
// (widening). Auto derives from the nozzle instead.
TEST_CASE("imagemap: the wall map works on a 0.2 mm nozzle, not just a 0.4", "[ImageMapPerLayerColor]")
{
    struct Machine { const char *name; float nozzle; float nominal; float layer; };
    const Machine machines[] = {
        { "0.4 mm nozzle", 0.40f, 0.42f, 0.20f },
        { "0.2 mm nozzle", 0.20f, 0.22f, 0.14f },
        { "0.6 mm nozzle", 0.60f, 0.62f, 0.30f },
    };

    for (const Machine &m : machines) {
        CAPTURE(m.name);
        const float cap = Slic3r::ImageMapPerLayer::max_surface_offset_mm(m.nozzle);
        const float floor_mm = Slic3r::ImageMapPerLayer::min_printable_width_mm(m.nozzle);

        WallModulationSettings s;                 // all three mm settings default to auto
        s.layer_height_mm    = m.layer;
        s.nozzle_diameter_mm = m.nozzle;

        SECTION(std::string(m.name) + ": the width half actually modulates") {
            s.modulate_width = true;
            const WallModulation none = Slic3r::ImageMapPerLayer::compute_wall_modulation(0.f, m.nominal, s);
            const WallModulation full = Slic3r::ImageMapPerLayer::compute_wall_modulation(1.f, m.nominal, s);
            // The regression: on a 0.2 mm nozzle this used to be inactive.
            REQUIRE(none.active);
            REQUIRE(none.width_changed);
            REQUIRE(none.width_mm < m.nominal - 1e-3f);   // it recedes...
            REQUIRE(none.width_mm >= floor_mm - 1e-4f);   // ...but stays extrudable
            REQUIRE(!full.active);                        // weight 1 == nominal, nothing to do
            REQUIRE(std::abs(none.surface_offset_mm) <= cap + 1e-3f);
        }

        SECTION(std::string(m.name) + ": widening never pins the wall above nominal") {
            s.modulate_width = true;
            s.allow_widening = true;
            // The other 0.2 mm failure: the sweep sat entirely above the nominal
            // width, so every layer over-extruded and none ever receded.
            const WallModulation none = Slic3r::ImageMapPerLayer::compute_wall_modulation(0.f, m.nominal, s);
            const WallModulation full = Slic3r::ImageMapPerLayer::compute_wall_modulation(1.f, m.nominal, s);
            REQUIRE(none.width_mm < m.nominal);
            REQUIRE(full.width_mm > m.nominal);
            REQUIRE(none.surface_offset_mm < 0.f);
            REQUIRE(full.surface_offset_mm > 0.f);
            REQUIRE(none.width_mm >= floor_mm - 1e-4f);
        }

        SECTION(std::string(m.name) + ": auto offset distance scales to the nozzle") {
            s.offset_surface = true;              // distance auto == cap, "in and out"
            const WallModulation full = Slic3r::ImageMapPerLayer::compute_wall_modulation(1.f, m.nominal, s);
            const WallModulation none = Slic3r::ImageMapPerLayer::compute_wall_modulation(0.f, m.nominal, s);
            REQUIRE(full.surface_offset_mm == Approx(+0.5f * cap).margin(1e-3));
            REQUIRE(none.surface_offset_mm == Approx(-0.5f * cap).margin(1e-3));
            REQUIRE(!full.width_changed);
        }

        SECTION(std::string(m.name) + ": the combined preset stays inside the cap") {
            const WallModulationSettings preset =
                Slic3r::ImageMapPerLayer::combined_preset_settings(m.nominal, m.layer, m.nozzle);
            for (int i = 0; i <= 10; ++i) {
                const WallModulation w =
                    Slic3r::ImageMapPerLayer::compute_wall_modulation(float(i) / 10.f, m.nominal, preset);
                REQUIRE(std::abs(w.surface_offset_mm) <= cap + 1e-3f);
                REQUIRE(w.width_mm >= floor_mm - 1e-4f);
            }
        }
    }
}

TEST_CASE("imagemap: an explicit millimetre setting still overrides auto", "[ImageMapPerLayerColor]")
{
    // Auto is only the default; a user who types a number gets that number.
    WallModulationSettings s = base_settings();   // explicit 0.32 / 0.95 / 0.15
    s.modulate_width = true;
    REQUIRE(Slic3r::ImageMapPerLayer::compute_wall_modulation(0.f, kNominal, s).width_mm
            == Approx(kMinWidth).margin(1e-4));

    WallModulationSettings o;                     // auto everywhere
    o.layer_height_mm = kLayerHeight;
    o.nozzle_diameter_mm = kNozzle;
    o.offset_surface = true;
    o.offset_distance_mm = 0.15f;                 // explicit
    REQUIRE(Slic3r::ImageMapPerLayer::compute_wall_modulation(1.f, kNominal, o).surface_offset_mm
            == Approx(0.075f).margin(1e-4));
}

TEST_CASE("imagemap: the combined preset runs both halves and beats either alone", "[ImageMapPerLayerColor]")
{
    const WallModulationSettings preset =
        Slic3r::ImageMapPerLayer::combined_preset_settings(kNominal, kLayerHeight, kNozzle);
    REQUIRE(preset.modulate_width);
    REQUIRE(preset.allow_widening);
    REQUIRE(preset.offset_surface);

    const WallModulation full = combined(1.f);
    const WallModulation none = combined(0.f);
    REQUIRE(full.active);
    REQUIRE(none.active);
    REQUIRE(full.width_changed);
    REQUIRE(none.width_changed);

    // Full scale in both directions: the budget is the surface-travel cap.
    REQUIRE(full.surface_offset_mm == Approx(+kCap).margin(1e-3));
    REQUIRE(none.surface_offset_mm == Approx(-kCap).margin(1e-3));
    REQUIRE(full.flow_scale > 1.0);
    REQUIRE(none.flow_scale < 1.0);

    // Strictly wider colour range than either half on its own at its default
    // settings -- that is the whole point of the preset.
    const float combined_range = full.surface_offset_mm - none.surface_offset_mm;
    const float width_range    = width_only(1.f, true).surface_offset_mm - width_only(0.f, true).surface_offset_mm;
    const float offset_range   = offset_only(1.f, 0.15f, WallOffsetDirection::Both).surface_offset_mm -
                                 offset_only(0.f, 0.15f, WallOffsetDirection::Both).surface_offset_mm;
    REQUIRE(combined_range > width_range);
    REQUIRE(combined_range > offset_range);

    // Monotone, and the preset ignores every user setting by construction: the
    // same nominal/layer/nozzle triple always yields the same numbers.
    float previous = -1e9f;
    for (int i = 0; i <= 10; ++i) {
        const WallModulation m = combined(float(i) / 10.f);
        REQUIRE(m.surface_offset_mm >= previous - 1e-6f);
        previous = m.surface_offset_mm;
    }
}

TEST_CASE("imagemap: offset_closed_ring() miters a square inward and preserves vertices", "[ImageMapPerLayerColor]")
{
    // 10 mm CCW square.
    const coord_t s = coord_t(scale_(10.));
    Points square{Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)};

    const double delta = scale_(0.5);
    Points offset;
    REQUIRE(Slic3r::ImageMapPerLayer::offset_closed_ring(square, delta, offset));
    REQUIRE(offset.size() == square.size());

    // A square offset inward by 0.5 mm is a square inset 0.5 mm on every side --
    // the miter must land exactly on the corner, not on the edge normal.
    const coord_t d = coord_t(scale_(0.5));
    REQUIRE(offset[0].x() == Approx(double(d)).margin(2.));
    REQUIRE(offset[0].y() == Approx(double(d)).margin(2.));
    REQUIRE(offset[2].x() == Approx(double(s - d)).margin(2.));
    REQUIRE(offset[2].y() == Approx(double(s - d)).margin(2.));

    // Negative delta grows the ring back outward.
    Points grown;
    REQUIRE(Slic3r::ImageMapPerLayer::offset_closed_ring(square, -delta, grown));
    REQUIRE(grown[0].x() == Approx(-double(d)).margin(2.));
    REQUIRE(grown[0].y() == Approx(-double(d)).margin(2.));

    // Same square wound clockwise: "interior" must still mean interior, so the
    // inward offset lands in the same place.
    Points cw(square.rbegin(), square.rend());
    Points cw_offset;
    REQUIRE(Slic3r::ImageMapPerLayer::offset_closed_ring(cw, delta, cw_offset));
    for (const Point &p : cw_offset) {
        REQUIRE(p.x() >= d - 2);
        REQUIRE(p.y() >= d - 2);
        REQUIRE(p.x() <= s - d + 2);
        REQUIRE(p.y() <= s - d + 2);
    }
}

TEST_CASE("imagemap: offset_closed_ring() bounds how far a sharp corner may travel", "[ImageMapPerLayerColor]")
{
    // A miter join displaces a vertex by |delta| / cos(turn/2), which runs away
    // at a near-reversal. This was clamped at 4x |delta|, so a needle-thin spike
    // in the toolpath -- and Arachne and painted-region boundaries both produce
    // them -- pushed the wall up to four times the requested offset outside the
    // model. On a real Benchy that was 110 of 599 layers past the surface-travel
    // cap, 1.34 mm out on a 0.35 mm budget, visible as loops standing outside
    // the wall path.
    const double delta = scale_(0.2);
    auto mm = [](double v) { return coord_t(scale_(v)); };

    // A 20 mm CCW square with a needle spike standing off its top edge: a body
    // big enough that the ring survives the offset (a bare wedge would be
    // refused by the collapse guard, which is separately correct), plus one
    // vertex whose turn angle is sharp enough to make the miter run away.
    Points spike{Point(0, 0),       Point(mm(20.), 0),    Point(mm(20.), mm(20.)),
                 Point(mm(10.1), mm(20.)), Point(mm(10.), mm(24.)), Point(mm(9.9), mm(20.)),
                 Point(0, mm(20.))};

    auto max_travel = [&](const Points &before, const Points &after) {
        double worst = 0.;
        for (size_t i = 0; i < before.size(); ++i)
            worst = std::max(worst, (after[i] - before[i]).cast<double>().norm());
        return worst;
    };

    SECTION("the miter limit alone bounds it") {
        Points out;
        REQUIRE(Slic3r::ImageMapPerLayer::offset_closed_ring(spike, delta, out));
        REQUIRE(out.size() == spike.size());
        REQUIRE(max_travel(spike, out) <= Slic3r::ImageMapPerLayer::kOffsetRingMiterLimit * delta + 2.);
    }

    SECTION("an absolute ceiling bounds it further") {
        const double ceiling = scale_(0.25);   // tighter than 2 * delta
        Points out;
        REQUIRE(Slic3r::ImageMapPerLayer::offset_closed_ring(spike, delta, out, ceiling));
        REQUIRE(max_travel(spike, out) <= ceiling + 2.);
    }

    SECTION("ordinary corners are still exactly mitered") {
        // 90 degrees wants 1.41x |delta|, inside the miter limit, so a square
        // must still inset exactly -- the fix must not round off normal corners.
        const coord_t s = coord_t(scale_(10.));
        Points square{Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)};
        const double ceiling = scale_(0.35);
        Points out;
        REQUIRE(Slic3r::ImageMapPerLayer::offset_closed_ring(square, delta, out, ceiling));
        REQUIRE(out[0].x() == Approx(delta).margin(2.));
        REQUIRE(out[0].y() == Approx(delta).margin(2.));
        REQUIRE(out[2].x() == Approx(double(s) - delta).margin(2.));
        REQUIRE(out[2].y() == Approx(double(s) - delta).margin(2.));
    }
}

TEST_CASE("imagemap: offset_closed_ring() refuses to collapse or invert a ring", "[ImageMapPerLayerColor]")
{
    // A 0.4 mm square cannot survive a 0.5 mm inward offset; the caller must be
    // told to leave the geometry alone rather than handed an inverted ring.
    //
    // Note this case turns inside out *without* reversing its winding -- the
    // miter carries each corner past the opposite side, so the result is a
    // larger square of the same orientation. A sign-of-area check alone passes
    // it; only requiring an inward offset to actually shrink the ring rejects it.
    const coord_t tiny = coord_t(scale_(0.4));
    Points small{Point(0, 0), Point(tiny, 0), Point(tiny, tiny), Point(0, tiny)};
    Points offset;
    REQUIRE(!Slic3r::ImageMapPerLayer::offset_closed_ring(small, scale_(0.5), offset));
    // Just past the limit in the other direction: a 0.4 mm square survives a
    // 0.1 mm inward offset, and the result really is smaller.
    REQUIRE(Slic3r::ImageMapPerLayer::offset_closed_ring(small, scale_(0.1), offset));
    REQUIRE(offset[2].x() - offset[0].x() < tiny);

    // Degenerate inputs.
    Points two{Point(0, 0), Point(coord_t(scale_(1.)), 0)};
    REQUIRE(!Slic3r::ImageMapPerLayer::offset_closed_ring(two, scale_(0.1), offset));
    Points empty;
    REQUIRE(!Slic3r::ImageMapPerLayer::offset_closed_ring(empty, scale_(0.1), offset));

    // A zero offset is not worth perturbing the toolpath for.
    const coord_t s = coord_t(scale_(10.));
    Points square{Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)};
    REQUIRE(!Slic3r::ImageMapPerLayer::offset_closed_ring(square, 0., offset));
}

// ---------------------------------------------------------------------------
// Rotation filaments (the origin's zone components) and colour targets that
// are not themselves in the rotation.
// ---------------------------------------------------------------------------

TEST_CASE("imagemap: parse_component_filaments() reads 1-based lists in order", "[ImageMapPerLayerColor]")
{
    using Slic3r::ImageMapPerLayer::parse_component_filaments;
    using V = std::vector<unsigned int>;
    REQUIRE(parse_component_filaments("", 4).empty());
    REQUIRE(parse_component_filaments("   ", 4).empty());
    // Order is kept: it is the layer rotation order.
    REQUIRE(parse_component_filaments("1,2,3,4", 4) == V{0, 1, 2, 3});
    REQUIRE(parse_component_filaments("4 2;1", 4) == V{3, 1, 0});
    REQUIRE(parse_component_filaments(" 3 , 1 ", 4) == V{2, 0});
    // Duplicates, out-of-range numbers and non-numeric tokens are dropped
    // whole -- "F2" must not turn into a stray filament 2, nor "-1" into 1.
    REQUIRE(parse_component_filaments("1,1,2", 4) == V{0, 1});
    REQUIRE(parse_component_filaments("0,5,2", 4) == V{1});
    REQUIRE(parse_component_filaments("F2,-1,3x,abc,4", 4) == V{3});
    REQUIRE(parse_component_filaments("99999999999999999999,1", 4) == V{0});
}

TEST_CASE("imagemap: a painted colour outside the rotation is mixed from the rotation filaments", "[ImageMapPerLayerColor]")
{
    // Slots 1-4 are loaded with red, green, blue and yellow; slot 5 is orange,
    // painted onto the model but never loaded -- a colour target only.
    DynamicPrintConfig dyn = DynamicPrintConfig::full_print_config();
    dyn.set_num_filaments(5);
    dyn.set_key_value("filament_colour",
                      new ConfigOptionStrings({"#FF0000", "#00FF00", "#0000FF", "#FFFF00", "#FF8000"}));
    PrintConfig config;
    config.apply(dyn, true);

    Slic3r::ImageMapPerLayer::Solver solver;
    const std::vector<unsigned int> rotation = {0, 1, 2, 3};
    REQUIRE(solver.init(config, rotation));
    REQUIRE_FALSE(solver.in_rotation(4));

    float sum = 0.f;
    std::vector<float> w;
    for (unsigned int active : rotation) {
        const float weight = solver.weight_for(4, active);
        REQUIRE(weight >= 0.f);
        REQUIRE(weight <= 1.f);
        w.push_back(weight);
        sum += weight;
    }
    REQUIRE(sum == Approx(1.f).margin(1e-3));
    // Orange is red and yellow, not green or blue.
    REQUIRE(w[0] + w[3] > 0.6f);
    REQUIRE(w[2] < 0.2f);

    // A rotation filament still solves to (nearly) itself.
    REQUIRE(solver.weight_for(2, 2) > 0.8f);
    // A filament that is not in the rotation can never be the active one.
    REQUIRE(solver.weight_for(0, 4) < 0.f);
}

// ---------------------------------------------------------------------------
// End to end: slice and export real G-code. Two 20 mm cubes, one per filament,
// with the feature on. Every layer must collapse to one filament, and turning
// the wall map on must actually change the G-code -- in a by-layer print *and*
// in a by-object one, where each object has its own ToolOrdering and the wall
// map used to find no rotation at all (collapsed tool changes, no modulation:
// plain stripes).
// ---------------------------------------------------------------------------

namespace {

DynamicPrintConfig make_e2e_config(bool by_object, bool wall_map, const std::string &components = "")
{
    DynamicPrintConfig config = make_imagemap_config(4, true);
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(by_object ? PrintSequence::ByObject : PrintSequence::ByLayer));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("image_map_wall_offset_enable", new ConfigOptionBool(wall_map));
    config.set_key_value("image_map_wall_width_enable", new ConfigOptionBool(false));
    config.set_key_value("image_map_wall_combined_preset", new ConfigOptionBool(false));
    config.set_key_value("image_map_component_filaments", new ConfigOptionString(components));
    // Per-filament settings with extruder variants are indexed through
    // filament_self_index / filament_extruder_variant; set_num_filaments()
    // leaves those at one entry, which silently gives filaments 2+ no
    // settings at all (and no perimeters or infill).
    const std::string variant = config.option<ConfigOptionStrings>("filament_extruder_variant")->values.front();
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(4, variant)));
    config.set_key_value("filament_self_index", new ConfigOptionInts({1, 2, 3, 4}));
    config.set_key_value("filament_map", new ConfigOptionInts({1, 1, 1, 1}));
    return config;
}

void build_e2e_print(Print &print, Model &model, const DynamicPrintConfig &config_in, const std::vector<int> &extruders)
{
    model.clear_objects();
    for (size_t i = 0; i < extruders.size(); ++i) {
        add_cube_object(model, 40.0 + 120.0 * double(i), extruders[i]);
        model.objects.back()->instances.front()->set_offset(Vec3d(40.0 + 120.0 * double(i), 100.0, 0.0));
    }
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(config_in.option<ConfigOptionFloats>("filament_diameter")->size());
    config.apply(config_in);
    // Upstream ConfigOptionEnumsGeneric(std::initializer_list<int>) initialises
    // keys_map from itself, so a vector-enum option holding its definition's
    // default carries a garbage keys map and crashes the first time G-code
    // export serialises it. The app never sees this (it builds options from
    // presets via the definition); a test using raw defaults does. Restore
    // their keys maps from the definitions.
    for (const std::string &key : config.keys()) {
        const ConfigOptionDef *def = config.def()->get(key);
        if (def == nullptr || def->type != coEnums || def->enum_keys_map == nullptr)
            continue;
        ConfigOption *opt = config.option(key);
        if (def->nullable)
            static_cast<ConfigOptionEnumsGenericNullable *>(opt)->keys_map = def->enum_keys_map;
        else
            static_cast<ConfigOptionEnumsGeneric *>(opt)->keys_map = def->enum_keys_map;
    }
    for (ModelObject *mo : model.objects)
        mo->ensure_on_bed();
    print.apply(model, config);
    print.set_status_silent();
}

// Slice and export, reporting the slicer's own error messages on failure.
std::string slice_to_gcode(Print &print)
{
    // G-code export writes temporaries under data_dir(), which a test binary
    // does not otherwise have.
    if (Slic3r::data_dir().empty() || !boost::filesystem::exists(Slic3r::data_dir())) {
        const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "imagemap_tests_data";
        boost::filesystem::create_directories(dir);
        Slic3r::set_data_dir(dir.string());
    }
    try {
        // Not Slic3r::Test::gcode(): it exports to a bare relative file name,
        // whose empty parent directory GCode::do_export() then fails to create.
        const boost::filesystem::path path = boost::filesystem::temp_directory_path() /
                                             boost::filesystem::unique_path("imagemap-%%%%-%%%%.gcode");
        print.set_status_silent();
        print.process();
        // do_export() dereferences the processor result unconditionally.
        GCodeProcessorResult result;
        print.export_gcode(path.string(), &result, nullptr);
        std::ifstream in(path.string());
        std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        boost::filesystem::remove(path);
        return gcode;
    } catch (const Slic3r::SlicingErrors &errors) {
        std::string msg;
        for (const Slic3r::SlicingError &e : errors.errors_)
            msg += std::string(e.what()) + "; ";
        FAIL("slicing failed: " << msg);
    }
    return {};
}

// Count the G-code tool selections ("T<n>" at the start of a line).
size_t count_tool_changes(const std::string &gcode)
{
    size_t count = 0;
    std::istringstream in(gcode);
    std::string line;
    while (std::getline(in, line))
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])))
            ++count;
    return count;
}

// Every layer of every tool ordering the print uses extrudes exactly one body filament.
void require_one_filament_per_layer(const Print &print, const std::vector<unsigned int> &expected_rotation)
{
    std::vector<const ToolOrdering *> orderings;
    if (print.is_sequential_print()) {
        REQUIRE(print.sequential_print_data().has_value());
        for (const auto &kv : print.sequential_print_data()->object_tool_ordering_map)
            orderings.push_back(&kv.second);
    } else {
        orderings.push_back(&print.tool_ordering());
    }
    REQUIRE(!orderings.empty());
    for (const ToolOrdering *ordering : orderings) {
        REQUIRE(ordering->image_map_rotation_filaments() == expected_rotation);
        for (const LayerTools &lt : ordering->layer_tools()) {
            if (lt.extruders.empty())
                continue;
            REQUIRE(lt.extruders.size() == 1);
            REQUIRE(lt.image_map_rotation == expected_rotation);
            REQUIRE(std::find(expected_rotation.begin(), expected_rotation.end(), lt.extruders.front()) != expected_rotation.end());
        }
    }
}

} // namespace

TEST_CASE("imagemap: end to end, rotation filaments drive collapsing and the wall map, by layer and by object", "[ImageMapPerLayerColor]")
{
    for (bool by_object : {false, true}) {
        SECTION(by_object ? "by object" : "by layer") {
            // Two cubes painted (assigned) filament 1; the rotation is 1, 2, 3.
            // Every layer must be printed in exactly one of 1-3, cycling, and
            // turning the wall map on must change the G-code -- by object as
            // well, where each cube has its own ToolOrdering and the wall map
            // used to find no rotation at all (collapsed tool changes, no
            // modulation: plain stripes of filament colour).
            Model model_off, model_on;
            Print print_off, print_on;
            build_e2e_print(print_off, model_off, make_e2e_config(by_object, false, "1,2,3"), {1, 1});
            build_e2e_print(print_on, model_on, make_e2e_config(by_object, true, "1,2,3"), {1, 1});
            const std::string gcode_off = slice_to_gcode(print_off);
            const std::string gcode_on  = slice_to_gcode(print_on);
            REQUIRE(!gcode_off.empty());
            require_one_filament_per_layer(print_on, {0, 1, 2});
            REQUIRE(gcode_on.find("\nT3\n") == std::string::npos);
            REQUIRE(gcode_on != gcode_off);
            // The wall map moves walls; it never adds or removes tool changes.
            REQUIRE(count_tool_changes(gcode_on) == count_tool_changes(gcode_off));
            REQUIRE(count_tool_changes(gcode_on) > 0);
        }
    }
}
