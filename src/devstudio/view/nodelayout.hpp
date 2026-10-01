#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

// Where every node canvas in Studio gets its automatic placement from. Nothing
// here knows about Qt: a placement is arithmetic over boxes and a relation, and
// the two scenes that draw it disagree about everything else. Keeping it out of
// the scene is also what makes the result measurable -- crossings, overlaps and
// the direction of every edge are properties of the numbers, not of a window.
namespace PelicanStudio {

// One box to place. `width`/`height` are what the caller is going to draw: the
// layout never invents a size, because a node's height follows its socket rows
// and only the scene knows how many there are.
struct LayoutNode {
    std::string id;
    double width = 0.0;
    double height = 0.0;
};

// One edge of the relation the columns come from. Duplicates and self-edges are
// allowed and are what a frame plan actually holds; the layout collapses them.
struct LayoutEdge {
    std::string from;
    std::string to;
};

struct LayoutPoint {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const LayoutPoint &, const LayoutPoint &) = default;
};

struct LayoutBox {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] double right() const noexcept { return x + width; }
    [[nodiscard]] double bottom() const noexcept { return y + height; }
    [[nodiscard]] double centerY() const noexcept { return y + height / 2.0; }

    friend bool operator==(const LayoutBox &, const LayoutBox &) = default;
};

using LayoutColumns = std::map<std::string, int, std::less<>>;
using LayoutBoxes = std::map<std::string, LayoutBox, std::less<>>;
using LayoutRoute = std::vector<LayoutPoint>;

struct LayoutOptions {
    double horizontal_gap = 88.0;
    double vertical_gap = 74.0;
    double origin_x = 0.0;
    double origin_y = 0.0;
    // A bend point is not a node, but an edge running through a column still
    // needs a lane of its own: this is the room one keeps between the nodes it
    // passes, which is why an edge never crosses a node rectangle. A lane is
    // much cheaper than a node -- a column that carries ten of them at the node
    // pitch would be taller than the graph it belongs to.
    double bend_height = 10.0;
    double bend_gap = 16.0;
    // How much harder a bend pulls itself onto its neighbours' line than a node
    // does. A long edge that wanders is the thing a reader loses first.
    double bend_weight = 8.0;
    // Barycenter up/down round trips (decision 2c) and the priority passes that
    // choose the vertical coordinate (decision 2d).
    int ordering_sweeps = 4;
    int coordinate_sweeps = 4;
    // When non-empty, the layer of a node is read from here rather than from
    // the longest path. The Frame Plan already has a column rule of its own --
    // a collapsed group sits at the minimum column of its members, a boundary
    // stub one column outside the window -- and that rule is the Frame Plan's
    // to keep. A node missing from the map lands in column 0.
    LayoutColumns columns;
    // UX-1, decision 6: the graph's ends -- what writes to the screen, a
    // sink -- which stand in the last column however short the path to
    // them is. Each named node, and everything that follows it, is moved
    // right as far as it goes: to the last column less the longest path out
    // of it, never left of the column it had, so no edge turns round.
    std::set<std::string, std::less<>> last_column;
};

struct NodeGraphLayout {
    // Top-left of every node the caller named, in scene units.
    LayoutBoxes boxes;
    LayoutColumns columns;
    // One entry per *input* edge, in the caller's order: the bend points the
    // dummy chain produced, empty for an edge that spans a single column.
    // Duplicate edges share the same bends, which is what makes a bundle of
    // parallel dependencies draw as one line.
    std::vector<LayoutRoute> bends;
    // Indices of the input edges the layering had to turn around because the
    // relation held a cycle. Recorded rather than hidden: an edge drawn right
    // to left is a fact about the document, not about the placement.
    std::vector<std::size_t> reversed_edges;
    // What the ordering stage settled on, counted between adjacent layers. It
    // is free (the sweeps count it anyway) and is the number the canvas
    // publishes, so a scenario can ask a live window how tangled it is.
    std::size_t crossings = 0;
};

// The one automatic placement. Deterministic: the same nodes and edges in the
// same order always produce the same boxes, and every tie is broken by id.
NodeGraphLayout layoutNodeGraph(const std::vector<LayoutNode> &nodes,
                                const std::vector<LayoutEdge> &edges,
                                const LayoutOptions &options = {});

// The measurements a test judges a placement by. They read only the numbers, so
// the same three answer for today's placement and for this one.

// The drawn line of every edge: out of the right side of its source, through
// the bends, into the left side of its target. An edge whose ends are missing,
// or that starts and finishes on the same node, gets an empty route.
std::vector<LayoutRoute> layoutRoutes(const LayoutBoxes &boxes,
                                      const std::vector<LayoutEdge> &edges,
                                      const std::vector<LayoutRoute> &bends);

// Pairs of routes that properly cross. Two edges sharing an endpoint are not a
// crossing -- they meet, which is what the socket rows are for.
std::size_t layoutCrossings(const std::vector<LayoutEdge> &edges,
                            const std::vector<LayoutRoute> &routes);

// Pairs of node boxes that share any area at all.
std::size_t layoutOverlaps(const LayoutBoxes &boxes);

// Edges whose drawn line runs through the rectangle of a node that is not one
// of its own two ends. This is what the dummy chain exists to make zero.
std::size_t layoutEdgeNodeHits(const LayoutBoxes &boxes,
                               const std::vector<LayoutEdge> &edges,
                               const std::vector<LayoutRoute> &routes);

// The total vertical travel of the relation: how far, added up, an edge has to
// climb or fall between its two ends.
double layoutEdgeSpan(const LayoutBoxes &boxes,
                      const std::vector<LayoutEdge> &edges);

} // namespace PelicanStudio
