#include "renderingpassconfigloader.hpp"
#include "../loader/fileio.hpp"
#include "../profiler.hpp"
#include <stdexcept>

namespace Pelican {

nlohmann::json loadRenderingPassConfigJson(const std::string &json_path) {
    ScopedLogTimer timer{"load rendering pass config json"};

    const auto rendering_pass_data = nlohmann::json::parse(readBinaryFile(json_path));
    if (!rendering_pass_data.is_object()) {
        throw std::runtime_error("Rendering config must be an object: " + json_path);
    }

    return rendering_pass_data;
}

} // namespace Pelican
