#include "sampletable.hpp"

#include "ktx2.hpp"

#include "../../project/sha256stream.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace Pelican {
namespace {

std::string readText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + path.generic_string());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// The bytes parsed are the bytes hashed: a write between the two cannot leave
// a table whose sha256 names other contents.
std::shared_ptr<const SampleTable> readSampleTable(SampleTable table, const std::string &text,
                                                   const std::string &manifest_text) {
    const auto &manifest_path = table.manifest;
    const auto &file = table.file;
    try {
        const auto manifest = nlohmann::json::parse(manifest_text);
        table.layout = parseSampleTableLayout(manifest.at("source").at("metadata").at("layout"));
    } catch (const std::exception &error) {
        throw std::runtime_error("the manifest " + manifest_path.filename().generic_string() +
                                 " gives no sample table layout (source.metadata.layout): " + error.what());
    }
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte *>(text.data()), text.size()};
    const auto image = parseKtx2(bytes, file.generic_string());
    const auto &layout = table.layout;
    const auto height = static_cast<std::uint64_t>(layout.rows_per_feature) * layout.features.size();
    if (image.format != Ktx2Format::R32Sfloat || image.dimension != Ktx2Dimension::TwoD || image.levels.size() != 1 ||
        image.width != layout.width || image.height != height)
        throw std::runtime_error("the table is not an R32_SFLOAT 2D KTX2 of " + std::to_string(layout.width) + "x" +
                                 std::to_string(height) + " (its manifest's layout)");
    const auto &level = image.levels.front();
    table.values.resize(level.size / sizeof(float));
    std::memcpy(table.values.data(), image.payload.data() + level.offset, table.values.size() * sizeof(float));
    return std::make_shared<const SampleTable>(std::move(table));
}

} // namespace

std::shared_ptr<const SampleTable> loadSampleTable(const std::filesystem::path &file) {
    const auto manifest = derivedAssetManifestPath(file);
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) throw std::runtime_error("the table file is missing");
    if (!std::filesystem::is_regular_file(manifest, error))
        throw std::runtime_error("the table has no manifest beside it (" + manifest.filename().generic_string() + ")");
    // IA-3: the bytes decide, not the write time and size: a table rewritten
    // within the clock's resolution (or with its time put back) is read anew.
    const auto text = readText(file);
    const auto manifest_text = readText(manifest);
    SampleTable read;
    read.file = file;
    read.manifest = manifest;
    read.file_sha256 = sha256Hex(text);
    read.manifest_sha256 = sha256Hex(manifest_text);
    auto stamp = std::pair{read.file_sha256, read.manifest_sha256};

    static std::mutex mutex;
    // Held by the graphs that read it; the cache only finds it again.
    static std::map<std::filesystem::path, std::pair<std::pair<std::string, std::string>,
                                                     std::weak_ptr<const SampleTable>>> cache;
    const auto key = file.lexically_normal();
    {
        std::lock_guard lock{mutex};
        if (const auto found = cache.find(key); found != cache.end() && found->second.first == stamp)
            if (auto held = found->second.second.lock()) return held;
    }
    auto table = readSampleTable(std::move(read), text, manifest_text);
    std::lock_guard lock{mutex};
    cache.insert_or_assign(key, std::pair{std::move(stamp), table});
    return table;
}

} // namespace Pelican
