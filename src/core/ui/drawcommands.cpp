#include "drawcommands.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Pelican::ui {

DrawBatch buildDrawBatch(std::vector<QuadCommand> commands) {
    std::stable_sort(commands.begin(), commands.end(), [](const auto &a, const auto &b) {
        return a.layer < b.layer || (a.layer == b.layer && a.decl_seq < b.decl_seq);
    });
    if (commands.size() > 16384) throw std::length_error("UI quad limit exceeded");
    DrawBatch batch;
    batch.quads = std::move(commands);
    for (const auto &quad : batch.quads) {
        if (batch.runs.empty() || batch.runs.back().key != quad.key) {
            batch.runs.push_back({quad.key, batch.total_index_count, 0});
        }
        batch.runs.back().index_count += 6;
        batch.total_index_count += 6;
    }
    return batch;
}

} // namespace Pelican::ui
