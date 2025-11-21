#pragma once

#include "core.hpp"

namespace Pelican {

void ECSCore::update() {
    std::vector<SystemId> sorted_systems;

    // topological sort
    std::queue<SystemId> que;
    std::unordered_map<SystemId, size_t> dependency_graph;

    for (const auto &[id, sys] : systems) {
        dependency_graph.insert({id, sys.depends_list.size()});
        if (sys.depends_list.empty())
            que.push(id);
    }
    while (!que.empty()) {
        auto next = que.front();
        que.pop();
        sorted_systems.push_back(next);

        for (const auto depended : systems.at(next).depended_by) {
            auto &dep_count = dependency_graph.at(depended);
            dep_count--;
            if (dep_count == 0)
                que.push(depended);
        }
    }

    // invoke
    for (auto sys_id : sorted_systems) {
        auto &sys = systems.at(sys_id);
        sys.p_func(*this, sys.system_ref);
    }
}

} // namespace Pelican