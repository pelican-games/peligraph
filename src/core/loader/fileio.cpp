#include "fileio.hpp"
#include <fstream>
#include <stdexcept>

namespace Pelican {

std::string readBinaryFile(const std::string &path) {
    std::ifstream file{path, std::ios_base::binary | std::ios_base::ate};
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    const auto sz = file.tellg();
    if (sz < 0) {
        throw std::runtime_error("Failed to get file size: " + path);
    }

    const auto file_size = static_cast<std::streamsize>(sz);

    std::string data;
    data.resize(static_cast<size_t>(file_size), '\0');
    file.seekg(0);
    file.read(data.data(), file_size);
    if (!file && file_size > 0) {
        throw std::runtime_error("Failed to read file: " + path);
    }
    return data;
}

} // namespace Pelican
