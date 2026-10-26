#include "computetaskdefinition.hpp"

namespace Pelican {

std::string_view computeTaskScheduleName(ComputeTaskSchedule schedule) {
    return nodeScheduleName(schedule);
}

} // namespace Pelican
