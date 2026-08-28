#include <catch2/catch.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ImageMapPerLayerColor.hpp"

#include "test_data.hpp"

#include <algorithm>

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
// Phase 2: outer-wall surface offsetting.
// ---------------------------------------------------------------------------

namespace {

using Slic3r::ImageMapPerLayer::OuterWallMode;
using Slic3r::ImageMapPerLayer::WallModulation;

constexpr float kNominal     = 0.42f;
constexpr float kMinWidth    = 0.32f;
constexpr float kMaxWidth    = 0.95f;
constexpr float kLayerHeight = 0.20f;
constexpr float kNozzle      = 0.40f;

WallModulation modulate(float weight, OuterWallMode mode, float strength = 100.f)
{
    return Slic3r::ImageMapPerLayer::compute_wall_modulation(
        weight, kNominal, kMinWidth, kMaxWidth, strength, kLayerHeight, kNozzle, mode);
}

// Where the wall's inner edge ends up, relative to the nominal inner edge.
// The centerline shift is positive toward the material, so the inner edge sits
// at (-shift - width/2) and the nominal one at (-nominal/2).
float inner_edge_error(const WallModulation &m)
{
    return (-m.centerline_shift_mm - 0.5f * m.width_mm) - (-0.5f * kNominal);
}

} // namespace

TEST_CASE("imagemap: Inward mode only ever recedes, and never moves the wall's inner edge", "[ImageMapPerLayerColor]")
{
    // weight 1 == the active filament matches the painted target: full width,
    // no shift, nothing to do.
    const WallModulation full = modulate(1.f, OuterWallMode::Inward);
    REQUIRE(!full.active);

    const WallModulation none = modulate(0.f, OuterWallMode::Inward);
    REQUIRE(none.active);
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
        const WallModulation m = modulate(float(i) / 10.f, OuterWallMode::Inward);
        REQUIRE(m.width_mm >= previous);
        REQUIRE(m.width_mm <= kNominal + 1e-4f);
        REQUIRE(inner_edge_error(m) == Approx(0.).margin(1e-4));
        previous = m.width_mm;
    }
}

TEST_CASE("imagemap: Centered mode swings the surface both outward and inward", "[ImageMapPerLayerColor]")
{
    const WallModulation full = modulate(1.f, OuterWallMode::Centered);
    const WallModulation none = modulate(0.f, OuterWallMode::Centered);

    REQUIRE(full.active);
    REQUIRE(none.active);
    // This is the Phase 2 point: the surface can now go *out* as well as in.
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

    // Strictly more colour range than Inward mode.
    const WallModulation inward_none = modulate(0.f, OuterWallMode::Inward);
    const float centered_range = full.surface_offset_mm - none.surface_offset_mm;
    const float inward_range   = 0.f - inward_none.surface_offset_mm;
    REQUIRE(centered_range > inward_range);
}

TEST_CASE("imagemap: surface movement is capped at the nozzle-derived limit", "[ImageMapPerLayerColor]")
{
    // Port of TextureMappingManager::max_component_surface_offset_mm().
    REQUIRE(Slic3r::ImageMapPerLayer::max_surface_offset_mm(0.40f) == Approx(0.35f));
    REQUIRE(Slic3r::ImageMapPerLayer::max_surface_offset_mm(0.20f) == Approx(0.20f));
    REQUIRE(Slic3r::ImageMapPerLayer::max_surface_offset_mm(1.00f) == Approx(0.35f));

    // An absurd configured max must not translate into an absurd excursion.
    for (int i = 0; i <= 10; ++i) {
        const WallModulation m = Slic3r::ImageMapPerLayer::compute_wall_modulation(
            float(i) / 10.f, kNominal, 0.05f, 3.0f, 100.f, kLayerHeight, kNozzle, OuterWallMode::Centered);
        REQUIRE(std::abs(m.surface_offset_mm) <= 0.35f + 1e-3f);
    }
}

TEST_CASE("imagemap: zero strength is an exact no-op in both modes", "[ImageMapPerLayerColor]")
{
    for (OuterWallMode mode : {OuterWallMode::Inward, OuterWallMode::Centered}) {
        for (int i = 0; i <= 4; ++i) {
            const WallModulation m = modulate(float(i) / 4.f, mode, 0.f);
            REQUIRE(!m.active);
        }
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
