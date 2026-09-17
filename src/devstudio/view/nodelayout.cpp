#include "nodelayout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <utility>

namespace PelicanStudio {
namespace {

// One thing the ordering and the coordinate stages place. A bend is a vertex
// like any other -- that is the whole point of the dummy chain: a long edge
// competes for a lane instead of being drawn over whatever is in its way.
struct Vertex {
    std::string id;   // empty for a bend
    std::string key;  // the tie-break; unique across the layout
    std::size_t seed = 0;
    int column = 0;
    double width = 0.0;
    double height = 0.0;
    double y = 0.0;
    bool bend = false;
};

struct UniqueEdge {
    std::size_t from = 0;
    std::size_t to = 0;
    std::vector<std::size_t> bends;
};

// The relation as the ordering stage reads it: consecutive links of the dummy
// chain, so a long edge is a run of one-column hops.
struct Links {
    std::vector<std::vector<std::size_t>> forward;
    std::vector<std::vector<std::size_t>> backward;
};

std::string bendKey(const Vertex &from, const Vertex &to, int column) {
    return from.key + ">" + to.key + "@" + std::to_string(column);
}

// Which edges a depth-first walk has to climb back up. A graph document is
// acyclic by construction, but a frame plan is a report from the engine and a
// malformed one must still draw: one edge of the cycle is turned around for the
// layering and the caller is told which.
std::vector<bool> backEdges(const std::vector<Vertex> &verts,
                            const std::vector<UniqueEdge> &uniques) {
    std::vector<std::vector<std::size_t>> out_edges(verts.size());
    for (std::size_t edge = 0; edge < uniques.size(); ++edge)
        out_edges[uniques[edge].from].push_back(edge);
    for (auto &list : out_edges)
        std::sort(list.begin(), list.end(),
                  [&](std::size_t left, std::size_t right) {
                      return verts[uniques[left].to].key <
                             verts[uniques[right].to].key;
                  });

    std::vector<std::size_t> roots(verts.size());
    std::iota(roots.begin(), roots.end(), std::size_t{0});
    std::sort(roots.begin(), roots.end(),
              [&](std::size_t left, std::size_t right) {
                  return verts[left].key < verts[right].key;
              });

    enum class Colour { white, grey, black };
    std::vector<Colour> colour(verts.size(), Colour::white);
    std::vector<bool> back(uniques.size(), false);
    struct Frame {
        std::size_t vertex;
        std::size_t next;
    };
    std::vector<Frame> stack;
    for (std::size_t root : roots) {
        if (colour[root] != Colour::white) continue;
        colour[root] = Colour::grey;
        stack.push_back(Frame{root, 0});
        while (!stack.empty()) {
            Frame &top = stack.back();
            if (top.next >= out_edges[top.vertex].size()) {
                colour[top.vertex] = Colour::black;
                stack.pop_back();
                continue;
            }
            const std::size_t edge = out_edges[top.vertex][top.next++];
            const std::size_t next = uniques[edge].to;
            if (colour[next] == Colour::grey) {
                back[edge] = true;
                continue;
            }
            if (colour[next] == Colour::black) continue;
            colour[next] = Colour::grey;
            stack.push_back(Frame{next, 0});
        }
    }
    return back;
}

// The longest path inside the visible set, which is exactly the rule the two
// canvases have always used: every edge then points left to right, and a whole
// graph depth cannot be used because the visible subset may hold one node per
// level. The pass bound keeps a relation that is still cyclic from spinning.
std::vector<int> longestPathLayers(std::size_t count,
                                   const std::vector<UniqueEdge> &uniques,
                                   const std::vector<bool> &back) {
    std::vector<int> column(count, 0);
    for (std::size_t pass = 0; pass < count; ++pass) {
        bool changed = false;
        for (std::size_t edge = 0; edge < uniques.size(); ++edge) {
            const std::size_t from =
                back[edge] ? uniques[edge].to : uniques[edge].from;
            const std::size_t to =
                back[edge] ? uniques[edge].from : uniques[edge].to;
            if (column[to] < column[from] + 1) {
                column[to] = column[from] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return column;
}

std::size_t orderCrossings(const std::vector<Vertex> &verts,
                           const std::map<int, std::vector<std::size_t>> &layers,
                           const Links &links) {
    std::vector<std::size_t> rank(verts.size(), 0);
    for (const auto &[column, order] : layers) {
        (void)column;
        for (std::size_t index = 0; index < order.size(); ++index)
            rank[order[index]] = index;
    }
    std::size_t crossings = 0;
    for (const auto &[column, order] : layers) {
        const auto next = layers.find(column + 1);
        if (next == layers.end()) continue;
        std::vector<std::pair<std::size_t, std::size_t>> pairs;
        for (std::size_t vertex : order)
            for (std::size_t other : links.forward[vertex])
                if (verts[other].column == column + 1)
                    pairs.emplace_back(rank[vertex], rank[other]);
        for (std::size_t left = 0; left + 1 < pairs.size(); ++left)
            for (std::size_t right = left + 1; right < pairs.size(); ++right) {
                const auto &a = pairs[left];
                const auto &b = pairs[right];
                if ((a.first < b.first && a.second > b.second) ||
                    (a.first > b.first && a.second < b.second))
                    ++crossings;
            }
    }
    return crossings;
}

// Isotonic regression by pooling adjacent violators. It is the whole of
// decision 2d's second half: every vertex is pulled to where its neighbours
// are, and the order the crossing stage chose is kept by pushing the ones that
// collide apart by exactly their own height plus the gap. Because the
// constraint chain is what the separation is, the result cannot overlap.
void placeLayer(const std::vector<std::size_t> &order,
                const std::vector<double> &targets,
                const std::vector<double> &weights, double gap, double bend_gap,
                std::vector<Vertex> &verts) {
    const std::size_t count = order.size();
    if (count == 0) return;
    std::vector<double> offset(count, 0.0);
    double run = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        offset[index] = run;
        run += verts[order[index]].height;
        if (index + 1 < count)
            run += verts[order[index]].bend || verts[order[index + 1]].bend
                       ? bend_gap
                       : gap;
    }
    struct Block {
        double weight;
        double value;
        std::size_t count;
    };
    std::vector<Block> blocks;
    blocks.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        blocks.push_back(Block{std::max(weights[index], 1e-6),
                               targets[index] - offset[index], 1});
        while (blocks.size() >= 2 &&
               blocks[blocks.size() - 2].value > blocks.back().value) {
            const Block higher = blocks.back();
            blocks.pop_back();
            const Block lower = blocks.back();
            blocks.pop_back();
            const double weight = lower.weight + higher.weight;
            blocks.push_back(
                Block{weight,
                      (lower.value * lower.weight + higher.value * higher.weight) /
                          weight,
                      lower.count + higher.count});
        }
    }
    std::size_t index = 0;
    for (const Block &block : blocks)
        for (std::size_t step = 0; step < block.count; ++step, ++index)
            verts[order[index]].y = block.value + offset[index];
}

double orientation(const LayoutPoint &a, const LayoutPoint &b,
                   const LayoutPoint &c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool segmentsCross(const LayoutPoint &a, const LayoutPoint &b,
                   const LayoutPoint &c, const LayoutPoint &d) {
    const double one = orientation(c, d, a);
    const double two = orientation(c, d, b);
    const double three = orientation(a, b, c);
    const double four = orientation(a, b, d);
    return ((one > 0.0) != (two > 0.0)) && ((three > 0.0) != (four > 0.0));
}

bool segmentHitsBox(const LayoutPoint &a, const LayoutPoint &b,
                    const LayoutBox &box) {
    const LayoutPoint corners[4] = {{box.x, box.y},
                                    {box.right(), box.y},
                                    {box.right(), box.bottom()},
                                    {box.x, box.bottom()}};
    for (int side = 0; side < 4; ++side)
        if (segmentsCross(a, b, corners[side], corners[(side + 1) % 4]))
            return true;
    const auto inside = [&](const LayoutPoint &point) {
        return point.x >= box.x && point.x <= box.right() && point.y >= box.y &&
               point.y <= box.bottom();
    };
    return inside(a) || inside(b);
}

} // namespace

NodeGraphLayout layoutNodeGraph(const std::vector<LayoutNode> &nodes,
                                const std::vector<LayoutEdge> &edges,
                                const LayoutOptions &options) {
    NodeGraphLayout out;
    out.bends.assign(edges.size(), LayoutRoute{});
    if (nodes.empty()) return out;

    // (1) The boxes the caller named. A repeated id is one node: the layout
    // cannot place the same thing twice.
    std::map<std::string, std::size_t, std::less<>> vertex_of;
    std::vector<Vertex> verts;
    verts.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const LayoutNode &node = nodes[index];
        if (node.id.empty() || vertex_of.contains(node.id)) continue;
        vertex_of.emplace(node.id, verts.size());
        verts.push_back(Vertex{node.id, node.id, index, 0, node.width,
                               node.height, 0.0, false});
    }
    if (verts.empty()) return out;

    // (2) The relation, collapsed. A frame plan holds the same pair once per
    // resource; drawing one line per bundle is what the scene already does, so
    // the layout counts the pair once too.
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> unique_of;
    std::vector<UniqueEdge> uniques;
    std::vector<std::size_t> unique_for_input(edges.size(),
                                              std::numeric_limits<std::size_t>::max());
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto from = vertex_of.find(edges[index].from);
        const auto to = vertex_of.find(edges[index].to);
        if (from == vertex_of.end() || to == vertex_of.end() ||
            from->second == to->second)
            continue;
        const std::pair<std::size_t, std::size_t> key{from->second, to->second};
        auto found = unique_of.find(key);
        if (found == unique_of.end()) {
            found = unique_of.emplace(key, uniques.size()).first;
            uniques.push_back(UniqueEdge{from->second, to->second, {}});
        }
        unique_for_input[index] = found->second;
    }

    // (3) Layers. Decision 2a: the longest path, unless the caller already has
    // a column rule of its own.
    std::vector<bool> back(uniques.size(), false);
    if (options.columns.empty()) {
        back = backEdges(verts, uniques);
        const std::vector<int> column =
            longestPathLayers(verts.size(), uniques, back);
        for (std::size_t index = 0; index < verts.size(); ++index)
            verts[index].column = column[index];
    } else {
        for (Vertex &vertex : verts) {
            const auto found = options.columns.find(vertex.id);
            if (found != options.columns.end()) vertex.column = found->second;
        }
    }
    const std::size_t real_count = verts.size();
    std::set<int> occupied;
    for (const Vertex &vertex : verts) occupied.insert(vertex.column);

    // (4) Decision 2b: a bend per column an edge flies over, so no edge is
    // drawn across a node it has nothing to do with.
    for (UniqueEdge &edge : uniques) {
        const int first = verts[edge.from].column;
        const int last = verts[edge.to].column;
        if (last - first <= 1) continue;
        for (int column : occupied) {
            if (column <= first) continue;
            if (column >= last) break;
            const std::size_t bend = verts.size();
            verts.push_back(Vertex{std::string{},
                                   bendKey(verts[edge.from], verts[edge.to],
                                           column),
                                   verts[edge.from].seed, column, 0.0,
                                   options.bend_height, 0.0, true});
            edge.bends.push_back(bend);
        }
    }

    Links links;
    links.forward.assign(verts.size(), {});
    links.backward.assign(verts.size(), {});
    for (const UniqueEdge &edge : uniques) {
        std::size_t previous = edge.from;
        for (std::size_t bend : edge.bends) {
            links.forward[previous].push_back(bend);
            links.backward[bend].push_back(previous);
            previous = bend;
        }
        links.forward[previous].push_back(edge.to);
        links.backward[edge.to].push_back(previous);
    }

    // (5) Decision 2c: barycenter sweeps, ties by id, and the best ordering any
    // sweep produced is the one that is kept -- a sweep that made things worse
    // must not be able to leave the drawing worse than it found it.
    std::map<int, std::vector<std::size_t>> layers;
    for (std::size_t index = 0; index < verts.size(); ++index)
        layers[verts[index].column].push_back(index);
    for (auto &[column, order] : layers) {
        (void)column;
        std::sort(order.begin(), order.end(),
                  [&](std::size_t left, std::size_t right) {
                      if (verts[left].seed != verts[right].seed)
                          return verts[left].seed < verts[right].seed;
                      return verts[left].key < verts[right].key;
                  });
    }
    std::vector<int> column_list;
    column_list.reserve(layers.size());
    for (const auto &[column, order] : layers) {
        (void)order;
        column_list.push_back(column);
    }

    std::map<int, std::vector<std::size_t>> best = layers;
    std::size_t best_crossings = orderCrossings(verts, layers, links);
    for (int sweep = 0; sweep < options.ordering_sweeps; ++sweep) {
        for (int direction = 0; direction < 2; ++direction) {
            const bool down = direction == 0;
            for (std::size_t step = 0; step + 1 < column_list.size(); ++step) {
                const int column =
                    down ? column_list[step + 1]
                         : column_list[column_list.size() - 2 - step];
                const int reference =
                    down ? column_list[step]
                         : column_list[column_list.size() - 1 - step];
                std::map<std::size_t, std::size_t> rank;
                for (std::size_t index = 0; index < layers[reference].size();
                     ++index)
                    rank.emplace(layers[reference][index], index);
                std::vector<std::size_t> &order = layers[column];
                std::vector<std::pair<double, std::size_t>> keyed;
                keyed.reserve(order.size());
                for (std::size_t index = 0; index < order.size(); ++index) {
                    const std::size_t vertex = order[index];
                    double total = 0.0;
                    std::size_t seen = 0;
                    for (std::size_t other :
                         down ? links.backward[vertex] : links.forward[vertex]) {
                        const auto found = rank.find(other);
                        if (found == rank.end()) continue;
                        total += static_cast<double>(found->second);
                        ++seen;
                    }
                    keyed.emplace_back(seen == 0
                                           ? static_cast<double>(index)
                                           : total / static_cast<double>(seen),
                                       vertex);
                }
                std::sort(keyed.begin(), keyed.end(),
                          [&](const auto &left, const auto &right) {
                              if (left.first != right.first)
                                  return left.first < right.first;
                              return verts[left.second].key <
                                     verts[right.second].key;
                          });
                for (std::size_t index = 0; index < order.size(); ++index)
                    order[index] = keyed[index].second;
            }
            const std::size_t counted = orderCrossings(verts, layers, links);
            if (counted < best_crossings) {
                best_crossings = counted;
                best = layers;
            }
        }
    }
    layers = best;
    out.crossings = best_crossings;

    // (6) Decision 2d: pull every vertex onto the line of what it is connected
    // to, then push what collides apart. A bend pulls harder than a node so a
    // long edge comes out straight.
    for (const int column : column_list) {
        double cursor = 0.0;
        const std::vector<std::size_t> &order = layers[column];
        for (std::size_t index = 0; index < order.size(); ++index) {
            verts[order[index]].y = cursor;
            cursor += verts[order[index]].height;
            if (index + 1 < order.size())
                cursor += verts[order[index]].bend || verts[order[index + 1]].bend
                              ? options.bend_gap
                              : options.vertical_gap;
        }
    }
    for (int sweep = 0; sweep < options.coordinate_sweeps; ++sweep) {
        for (int direction = 0; direction < 2; ++direction) {
            const bool down = direction == 0;
            for (std::size_t step = 0; step + 1 < column_list.size(); ++step) {
                const int column =
                    down ? column_list[step + 1]
                         : column_list[column_list.size() - 2 - step];
                const int reference =
                    down ? column_list[step]
                         : column_list[column_list.size() - 1 - step];
                const std::vector<std::size_t> &order = layers[column];
                std::vector<double> targets;
                std::vector<double> weights;
                targets.reserve(order.size());
                weights.reserve(order.size());
                for (std::size_t vertex : order) {
                    double total = 0.0;
                    std::size_t seen = 0;
                    for (std::size_t other :
                         down ? links.backward[vertex] : links.forward[vertex]) {
                        if (verts[other].column != reference) continue;
                        total += verts[other].y + verts[other].height / 2.0;
                        ++seen;
                    }
                    if (seen == 0) {
                        // Nothing over there to line up with: stay where the
                        // other direction left it, and yield to those that do.
                        targets.push_back(verts[vertex].y);
                        weights.push_back(0.25);
                        continue;
                    }
                    targets.push_back(total / static_cast<double>(seen) -
                                      verts[vertex].height / 2.0);
                    weights.push_back(verts[vertex].bend ? options.bend_weight
                                                         : 1.0);
                }
                placeLayer(order, targets, weights, options.vertical_gap,
                           options.bend_gap, verts);
            }
        }
    }

    // (7) Decision 2e: one column width per layer, the widest node in it.
    std::map<int, std::pair<double, double>> column_x;
    double pen = options.origin_x;
    for (const int column : column_list) {
        double width = 0.0;
        for (std::size_t vertex : layers[column])
            if (!verts[vertex].bend)
                width = std::max(width, verts[vertex].width);
        column_x.emplace(column, std::pair{pen, width});
        pen += width + options.horizontal_gap;
    }

    double lowest = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < real_count; ++index)
        lowest = std::min(lowest, verts[index].y);
    const double shift =
        lowest == std::numeric_limits<double>::max() ? 0.0
                                                     : options.origin_y - lowest;

    for (std::size_t index = 0; index < real_count; ++index) {
        const Vertex &vertex = verts[index];
        const auto &[x, width] = column_x.at(vertex.column);
        (void)width;
        out.boxes.emplace(vertex.id, LayoutBox{x, vertex.y + shift,
                                               vertex.width, vertex.height});
        out.columns.emplace(vertex.id, vertex.column);
    }

    std::vector<LayoutRoute> unique_bends(uniques.size());
    for (std::size_t edge = 0; edge < uniques.size(); ++edge) {
        LayoutRoute route;
        for (std::size_t bend : uniques[edge].bends) {
            const auto &[x, width] = column_x.at(verts[bend].column);
            const double y = verts[bend].y + shift + verts[bend].height / 2.0;
            // Two points, not one: inside the column the edge runs level, so
            // the only place it can slope is the gap, where no node lives.
            route.push_back(LayoutPoint{x, y});
            route.push_back(LayoutPoint{x + width, y});
        }
        unique_bends[edge] = std::move(route);
    }
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const std::size_t edge = unique_for_input[index];
        if (edge == std::numeric_limits<std::size_t>::max()) continue;
        out.bends[index] = unique_bends[edge];
        if (back[edge]) out.reversed_edges.push_back(index);
    }
    return out;
}

std::vector<LayoutRoute> layoutRoutes(const LayoutBoxes &boxes,
                                      const std::vector<LayoutEdge> &edges,
                                      const std::vector<LayoutRoute> &bends) {
    std::vector<LayoutRoute> routes(edges.size());
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto from = boxes.find(edges[index].from);
        const auto to = boxes.find(edges[index].to);
        if (from == boxes.end() || to == boxes.end() ||
            edges[index].from == edges[index].to)
            continue;
        LayoutRoute route;
        route.push_back(
            LayoutPoint{from->second.right(), from->second.centerY()});
        if (index < bends.size())
            route.insert(route.end(), bends[index].begin(), bends[index].end());
        route.push_back(LayoutPoint{to->second.x, to->second.centerY()});
        routes[index] = std::move(route);
    }
    return routes;
}

std::size_t layoutCrossings(const std::vector<LayoutEdge> &edges,
                            const std::vector<LayoutRoute> &routes) {
    std::size_t crossings = 0;
    for (std::size_t left = 0; left < routes.size(); ++left) {
        if (routes[left].size() < 2) continue;
        for (std::size_t right = left + 1; right < routes.size(); ++right) {
            if (routes[right].size() < 2) continue;
            if (left < edges.size() && right < edges.size()) {
                const LayoutEdge &a = edges[left];
                const LayoutEdge &b = edges[right];
                if (a.from == b.from || a.from == b.to || a.to == b.from ||
                    a.to == b.to)
                    continue;
            }
            bool hit = false;
            for (std::size_t i = 0; i + 1 < routes[left].size() && !hit; ++i)
                for (std::size_t j = 0; j + 1 < routes[right].size() && !hit; ++j)
                    hit = segmentsCross(routes[left][i], routes[left][i + 1],
                                        routes[right][j], routes[right][j + 1]);
            if (hit) ++crossings;
        }
    }
    return crossings;
}

std::size_t layoutOverlaps(const LayoutBoxes &boxes) {
    std::vector<const LayoutBox *> all;
    all.reserve(boxes.size());
    for (const auto &[id, box] : boxes) {
        (void)id;
        all.push_back(&box);
    }
    std::size_t overlaps = 0;
    for (std::size_t left = 0; left + 1 < all.size(); ++left)
        for (std::size_t right = left + 1; right < all.size(); ++right) {
            const LayoutBox &a = *all[left];
            const LayoutBox &b = *all[right];
            if (a.x < b.right() && b.x < a.right() && a.y < b.bottom() &&
                b.y < a.bottom())
                ++overlaps;
        }
    return overlaps;
}

std::size_t layoutEdgeNodeHits(const LayoutBoxes &boxes,
                               const std::vector<LayoutEdge> &edges,
                               const std::vector<LayoutRoute> &routes) {
    std::size_t hits = 0;
    for (std::size_t index = 0; index < routes.size(); ++index) {
        if (routes[index].size() < 2 || index >= edges.size()) continue;
        const LayoutEdge &edge = edges[index];
        bool hit = false;
        for (const auto &[id, box] : boxes) {
            if (hit) break;
            if (id == edge.from || id == edge.to) continue;
            for (std::size_t point = 0; point + 1 < routes[index].size();
                 ++point)
                if (segmentHitsBox(routes[index][point],
                                   routes[index][point + 1], box)) {
                    hit = true;
                    break;
                }
        }
        if (hit) ++hits;
    }
    return hits;
}

double layoutEdgeSpan(const LayoutBoxes &boxes,
                      const std::vector<LayoutEdge> &edges) {
    double span = 0.0;
    for (const LayoutEdge &edge : edges) {
        const auto from = boxes.find(edge.from);
        const auto to = boxes.find(edge.to);
        if (from == boxes.end() || to == boxes.end()) continue;
        span += std::abs(to->second.centerY() - from->second.centerY());
    }
    return span;
}

} // namespace PelicanStudio
