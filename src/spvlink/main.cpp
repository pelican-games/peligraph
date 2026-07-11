#include "../core/shader/spvlink.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::uint32_t> readSpirv(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file.is_open()) throw std::runtime_error("failed to open " + path.string());
    const auto bytes = static_cast<std::size_t>(file.tellg());
    if (bytes == 0 || bytes % sizeof(std::uint32_t) != 0) {
        throw std::runtime_error(path.string() + " is not an aligned SPIR-V binary");
    }
    std::vector<std::uint32_t> words(bytes / sizeof(std::uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(bytes));
    if (!file) throw std::runtime_error("failed to read " + path.string());
    return words;
}

void writeSpirv(const std::filesystem::path &path, const std::vector<std::uint32_t> &words) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file.is_open()) throw std::runtime_error("failed to create " + path.string());
    file.write(reinterpret_cast<const char *>(words.data()),
               static_cast<std::streamsize>(words.size() * sizeof(std::uint32_t)));
}

void usage() {
    std::cerr << "usage: pelican-spv-link --template template.spv --user user.spv "
                 "--user-export symbol [--template-export symbol ...] --output final.spv "
                 "[--bindings bindings.json] [--cache-salt value]\n";
}

} // namespace

int main(int argc, char **argv) {
    try {
        std::filesystem::path template_path;
        std::filesystem::path user_path;
        std::filesystem::path output_path;
        std::filesystem::path bindings_path;
        Pelican::SpvLinkRequest request;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option{argv[i]};
            const auto value = [&]() -> std::string {
                if (++i >= argc) throw std::runtime_error("missing value after " + std::string{option});
                return argv[i];
            };
            if (option == "--template") template_path = value();
            else if (option == "--user") user_path = value();
            else if (option == "--output") output_path = value();
            else if (option == "--bindings") bindings_path = value();
            else if (option == "--user-export") request.user_exports.push_back(value());
            else if (option == "--template-export") request.template_exports.push_back(value());
            else if (option == "--preserve-descriptor") request.preserved_descriptor_names.push_back(value());
            else if (option == "--cache-salt") request.cache_salts.push_back(value());
            else if (option == "--material-set") request.material_set = std::stoul(value());
            else if (option == "--first-binding") request.first_free_material_binding = std::stoul(value());
            else if (option == "--toolchain") {
                std::cout << Pelican::spvLinkToolchainManifest() << '\n';
                return 0;
            } else {
                throw std::runtime_error("unknown option: " + std::string{option});
            }
        }
        if (template_path.empty() || user_path.empty() || output_path.empty() ||
            request.user_exports.empty()) {
            usage();
            return 2;
        }
        const auto template_words = readSpirv(template_path);
        const auto user_words = readSpirv(user_path);
        request.template_module = template_words;
        request.user_module = user_words;
        const auto result = Pelican::linkSpirvModules(request);
        writeSpirv(output_path, result.spirv);
        if (!bindings_path.empty()) {
            nlohmann::ordered_json document;
            document["cache_key"] = result.cache_key;
            document["bindings"] = nlohmann::ordered_json::array();
            for (const auto &binding : result.bindings) {
                document["bindings"].push_back({
                    {"logical_name", binding.logical_name},
                    {"descriptor_type", binding.descriptor_type},
                    {"original", {{"set", binding.original_set}, {"binding", binding.original_binding}}},
                    {"remapped", {{"set", binding.set}, {"binding", binding.binding}}},
                });
            }
            std::ofstream file{bindings_path, std::ios::binary | std::ios::trunc};
            if (!file.is_open()) throw std::runtime_error("failed to create " + bindings_path.string());
            file << document.dump(2) << '\n';
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "pelican-spv-link: " << error.what() << '\n';
        return 1;
    }
}
