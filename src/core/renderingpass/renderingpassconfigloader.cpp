#include "renderingpassconfigloader.hpp"
#include "../loader/fileio.hpp"
#include "../profiler.hpp"
#include <stdexcept>
#include <string_view>

namespace Pelican {

nlohmann::json loadRenderingPassConfigJson(const std::string &json_path) {
    ScopedLogTimer timer{"load rendering pass config json"};

    return loadRenderingPassConfigJsonFromString(readBinaryFile(json_path), json_path);
}

nlohmann::json loadRenderingPassConfigJsonFromString(std::string_view json_data, std::string_view source_name) {
    const auto rendering_pass_data = nlohmann::json::parse(json_data);
    if (!rendering_pass_data.is_object()) {
        throw std::runtime_error("Rendering config must be an object: " + std::string{source_name});
    }

    return rendering_pass_data;
}

} // namespace Pelican
