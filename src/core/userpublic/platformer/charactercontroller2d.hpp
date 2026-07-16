#pragma once

#include "../export.hpp"
#include <phys/physquery.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace Pelican {

class GameContext;

namespace platformer {

enum class ContactKind2D : std::uint8_t {
    ground,
    wall,
    ceiling,
};

struct CharacterContact2D {
    phys::ShapeCastQueryHit hit;
    // The collision normal projected into the side-scroller XY plane and
    // normalized. This is the normal used by the movement policy.
    vec3 normal{0.0F, 1.0F, 0.0F};
    ContactKind2D kind = ContactKind2D::wall;
};

struct MoveAndSlide2DSettings {
    // A contact is walkable when normal.y >= cos(max_slope_degrees).
    float max_slope_degrees = 50.0F;
    // The controller stops this far before a non-overlapping cast contact.
    float skin_width = 1.0e-4F;
    // Motions at or below this length are treated as zero after overlap
    // recovery. It must not be smaller than zero.
    float motion_epsilon = 1.0e-6F;
    // A one-way surface may be this far above the moving shape's starting
    // support point and still count as approached from above.
    float one_way_tolerance = 2.0e-4F;
    std::uint32_t max_iterations = 8;
    bool collide_with_triggers = false;
    bool collide_with_one_way = true;
};

struct MoveAndSlide2DResult {
    // The translated shape is the canonical output. Its orientation and
    // dimensions are unchanged.
    phys::Shape shape;
    vec3 requested_delta{0.0F, 0.0F, 0.0F};
    // Includes deterministic initial-overlap recovery as well as requested
    // motion, so it can be larger than requested_delta.
    vec3 applied_translation{0.0F, 0.0F, 0.0F};
    vec3 remaining_delta{0.0F, 0.0F, 0.0F};
    std::vector<CharacterContact2D> contacts;
    bool grounded = false;
    bool hit_wall = false;
    bool hit_ceiling = false;
    bool iteration_limit_reached = false;
};

// The callback must return the same canonical ordered-all-hit contract as
// GameContext::shapeCastAll. Keeping it injectable makes this standard helper
// usable with Builtin, Jolt, a game DLL provider, or a completely custom query
// source without granting it engine privileges.
using ShapeCastAll2DQuery = std::function<std::vector<phys::ShapeCastQueryHit>(
    const phys::Shape &, vec3, const phys::QueryFilter &)>;

PELICAN_API MoveAndSlide2DResult moveAndSlide(
    const phys::Shape &moving_shape, vec3 delta,
    const ShapeCastAll2DQuery &query,
    const phys::QueryFilter &filter = {},
    const MoveAndSlide2DSettings &settings = {});

// Convenience adapter over the public GameContext query surface. It contains
// no backend-specific behavior; all one-way and slope policy remains above the
// provider boundary in the function above.
PELICAN_API MoveAndSlide2DResult moveAndSlide(
    GameContext &context, const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter = {},
    const MoveAndSlide2DSettings &settings = {});

} // namespace platformer
} // namespace Pelican
