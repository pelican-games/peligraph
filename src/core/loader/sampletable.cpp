#include "sampletable.hpp"

#include "ktx2.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <tuple>

namespace Pelican {
namespace {

using Stamp = std::tuple<std::int64_t, std::uintmax_t, std::int64_t, std::uintmax_t>;

std::optional<std::pair<std::int64_t, std::uintmax_t>> fileStamp(const std::filesystem::path &path) {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) return std::nullopt;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return std::nullopt;
    return std::pair{static_cast<std::int64_t>(time.time_since_epoch().count()), size};
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + path.generic_string());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::shared_ptr<const SampleTable> readSampleTable(const std::filesystem::path &file,
                                                   const std::filesystem::path &manifest_path) {
    SampleTable table;
    try {
        const auto manifest = nlohmann::json::parse(readText(manifest_path));
        table.layout = parseSampleTableLayout(manifest.at("source").at("metadata").at("layout"));
    } catch (const std::exception &error) {
        throw std::runtime_error("the manifest " + manifest_path.filename().generic_string() +
                                 " gives no sample table layout (source.metadata.layout): " + error.what());
    }
    const auto text = readText(file);
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
    const auto file_stamp = fileStamp(file);
    if (!file_stamp) throw std::runtime_error("the table file is missing");
    const auto manifest_stamp = fileStamp(manifest);
    if (!manifest_stamp)
        throw std::runtime_error("the table has no manifest beside it (" + manifest.filename().generic_string() + ")");
    const Stamp stamp{file_stamp->first, file_stamp->second, manifest_stamp->first, manifest_stamp->second};

    static std::mutex mutex;
    // Held by the graphs that read it; the cache only finds it again.
    static std::map<std::filesystem::path, std::pair<Stamp, std::weak_ptr<const SampleTable>>> cache;
    const auto key = file.lexically_normal();
    {
        std::lock_guard lock{mutex};
        if (const auto found = cache.find(key); found != cache.end() && found->second.first == stamp)
            if (auto held = found->second.second.lock()) return held;
    }
    auto table = readSampleTable(file, manifest);
    std::lock_guard lock{mutex};
    cache.insert_or_assign(key, std::pair{stamp, table});
    return table;
}

} // namespace Pelican
