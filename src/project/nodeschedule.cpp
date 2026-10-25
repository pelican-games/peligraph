#include "nodeschedule.hpp"

#include "graphcomposition.hpp"

namespace Pelican {

std::string_view nodeScheduleName(NodeSchedule schedule) {
    switch (schedule) {
    case NodeSchedule::per_frame:
        return "per_frame";
    case NodeSchedule::per_view:
        return "per_view";
    case NodeSchedule::once:
        return "once";
    }
    throw GraphRegistryError("render_graph.node_schedule_unknown@1",
                             {{"value", static_cast<int>(schedule)}});
}

} // namespace Pelican
