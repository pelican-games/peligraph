#include "drawcommands.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace Pelican::ui {

DrawBatch buildDrawBatch(std::vector<QuadCommand> commands, std::string_view document_key) {
    std::stable_sort(commands.begin(), commands.end(), [](const auto &a, const auto &b) {
        return a.layer < b.layer || (a.layer == b.layer && a.decl_seq < b.decl_seq);
    });
    if (commands.size() > maxQuads) {
        const auto widget = commands.empty() ? std::string{} : commands.back().widget_id;
        throw std::length_error("limit_exceeded: UI document '" + std::string{document_key} +
                                "' widget '" + widget + "' has " + std::to_string(commands.size()) +
                                " quads; limit is " + std::to_string(maxQuads));
    }
    std::set<std::pair<std::uint16_t, RectI>> clips;
    for (const auto &command : commands) clips.emplace(command.key.clip_id, command.key.scissor_px);
    if (clips.size() > maxUniqueClips) {
        throw std::length_error("limit_exceeded: UI document '" + std::string{document_key} +
                                "' has " + std::to_string(clips.size()) +
                                " unique clips; limit is " + std::to_string(maxUniqueClips));
    }
    DrawBatch batch;
    batch.quads = std::move(commands);
    batch.vertices.reserve(batch.quads.size() * 4);
    batch.indices.reserve(batch.quads.size() * 6);
    for (const auto &quad : batch.quads) {
        if (batch.runs.empty() || batch.runs.back().key != quad.key) {
            batch.runs.push_back({quad.key, batch.total_index_count, 0});
        }
        batch.runs.back().index_count += 6;
        const auto base = static_cast<std::uint16_t>(batch.vertices.size());
        const auto &r = quad.rect_px;
        const auto &u = quad.uv_rect;
        batch.vertices.push_back({{static_cast<float>(r.left), static_cast<float>(r.top)}, {u[0], u[1]}, quad.color});
        batch.vertices.push_back({{static_cast<float>(r.right), static_cast<float>(r.top)}, {u[2], u[1]}, quad.color});
        batch.vertices.push_back({{static_cast<float>(r.right), static_cast<float>(r.bottom)}, {u[2], u[3]}, quad.color});
        batch.vertices.push_back({{static_cast<float>(r.left), static_cast<float>(r.bottom)}, {u[0], u[3]}, quad.color});
        const std::uint16_t quad_indices[]{base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2),
                                           base, static_cast<std::uint16_t>(base + 2), static_cast<std::uint16_t>(base + 3)};
        batch.indices.insert(batch.indices.end(), std::begin(quad_indices), std::end(quad_indices));
        batch.total_index_count += 6;
    }
    return batch;
}

} // namespace Pelican::ui
