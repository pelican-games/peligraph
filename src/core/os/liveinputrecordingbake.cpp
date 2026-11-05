#include "liveinputrecordingbake.hpp"

#include "../../project/filereplace.hpp"
#include "../../project/graphcomposition.hpp"
#include "../../project/sha256stream.hpp"
#include "../loader/ktx2.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

[[noreturn]] void invalid(std::string reason, Json fields = Json::object()) {
    fields["reason"] = std::move(reason);
    throw GraphRegistryError(std::string{liveRecordingBakeInvalidCode}, std::move(fields));
}

[[noreturn]] void failed(const std::string &message, Json fields = Json::object()) {
    fields["message"] = message;
    throw GraphRegistryError(std::string{liveRecordingBakeFailedCode}, std::move(fields));
}

float checkedFloat(double value, const LiveInputSpec &spec) {
    if (!std::isfinite(value) ||
        std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
        invalid("value_outside_float32", {{"input", spec.name}, {"value", value}});
    const auto converted = static_cast<float>(value);
    if (spec.type == LiveInputType::integer && static_cast<double>(converted) != value)
        invalid("integer_not_exact_in_float32", {{"input", spec.name}, {"value", value}});
    return converted;
}

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + path.generic_string());
    std::string result{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (!input.good() && !input.eof()) throw std::runtime_error("cannot read " + path.generic_string());
    return result;
}

std::filesystem::path relativeToTable(const std::filesystem::path &source,
                                      const std::filesystem::path &out) {
    std::error_code error;
    auto result = std::filesystem::relative(source, out.parent_path(), error);
    if (error) result = source;
    return result;
}

void writeStaged(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error("cannot write " + path.generic_string());
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("cannot write " + path.generic_string());
}

std::string hashBytes(std::span<const std::byte> bytes) {
    return sha256Hex(std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}

struct OldFile {
    bool present = false;
    std::string bytes;
};

OldFile readOldFile(const std::filesystem::path &path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return {};
    return {true, readFile(path)};
}

void restoreFile(const std::filesystem::path &path, const OldFile &old) {
    std::error_code error;
    if (old.present) {
        writeFileReplacing(path, std::as_bytes(std::span{old.bytes}));
    } else {
        std::filesystem::remove(path, error);
        if (error) throw std::runtime_error("cannot roll back " + path.generic_string());
    }
}

} // namespace

LiveRecordingTable makeLiveRecordingTable(const LiveInputRecording &recording, double rate,
                                          const std::optional<std::vector<std::string>> &inputs) {
    if (!std::isfinite(rate) || rate < 1.0 || rate > 1000.0)
        invalid("rate_out_of_range", {{"field", "rate"}, {"value", rate}});
    if (recording.inputs.empty()) invalid("no_inputs");
    if (recording.end.time < recording.clock.start_time)
        invalid("negative_duration", {{"t0_seconds", recording.clock.start_time},
                                     {"end_time", recording.end.time}});

    std::set<std::string> selected;
    if (inputs) {
        if (inputs->empty()) invalid("empty_inputs", {{"field", "inputs"}});
        for (const auto &name : *inputs) {
            if (!selected.insert(name).second) invalid("duplicate_input", {{"input", name}});
            const auto *spec = recording.declaration(name);
            if (spec == nullptr) invalid("unknown_input", {{"input", name}});
            if (spec->type != LiveInputType::floating && spec->type != LiveInputType::integer &&
                spec->type != LiveInputType::boolean)
                invalid("unsupported_input", {{"input", name}});
        }
    }

    LiveRecordingTable result;
    result.t0_seconds = recording.clock.start_time;
    auto &layout = result.layout;
    layout.hop_seconds = 1.0 / rate;
    for (const auto &spec : recording.inputs)
        if (!inputs || selected.contains(spec.name)) layout.features.push_back(spec.name);
    if (layout.features.empty()) invalid("empty_inputs", {{"field", "inputs"}});

    const long double duration = static_cast<long double>(recording.end.time) - result.t0_seconds;
    const long double intervals = std::ceil(duration * static_cast<long double>(rate));
    const long double count = intervals + 1.0L;
    if (!std::isfinite(static_cast<double>(count)) || count < 1.0L ||
        count > std::numeric_limits<std::uint32_t>::max())
        invalid("sample_count_limit", {{"sample_count", static_cast<double>(count)}});
    layout.sample_count = static_cast<std::uint32_t>(count);
    layout.rows_per_feature = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(layout.sample_count) + layout.width - 1) / layout.width);
    const std::uint64_t height = static_cast<std::uint64_t>(layout.rows_per_feature) * layout.features.size();
    const long double values_count = static_cast<long double>(layout.width) * height;
    const long double bytes = values_count * sizeof(float);
    if (height > std::numeric_limits<std::uint32_t>::max() ||
        values_count > std::numeric_limits<std::size_t>::max() ||
        bytes > std::numeric_limits<std::uint32_t>::max())
        invalid("table_size_limit", {{"sample_count", layout.sample_count},
                                    {"features", layout.features.size()},
                                    {"height", height},
                                    {"table_bytes", static_cast<double>(bytes)}});

    result.values.assign(static_cast<std::size_t>(values_count), 0.0f);
    std::map<std::string, double, std::less<>> current;
    for (const auto &spec : recording.inputs) current.emplace(spec.name, spec.default_value);
    auto frame = recording.frames.begin();
    for (std::uint32_t sample = 0; sample < layout.sample_count; ++sample) {
        const auto time = result.t0_seconds + static_cast<double>(sample) * layout.hop_seconds;
        while (frame != recording.frames.end() && frame->time <= time) {
            for (const auto &[name, value] : frame->values) current[name] = value;
            ++frame;
        }
        for (std::size_t feature = 0; feature < layout.features.size(); ++feature) {
            const auto &name = layout.features[feature];
            const auto &spec = *recording.declaration(name);
            const auto index = (feature * layout.rows_per_feature + sample / layout.width) * layout.width +
                               sample % layout.width;
            result.values[index] = checkedFloat(current.at(name), spec);
        }
    }
    return result;
}

} // namespace Pelican

namespace Pelican {

OrderedJson liveRecordingManifest(const std::filesystem::path &source,
                                  const std::string &source_sha256,
                                  const LiveRecordingTable &table,
                                  const std::filesystem::path &out,
                                  const std::string &table_sha256) {
    return {{"schema", "pelican.import"},
            {"version", 1},
            {"tool", {{"name", liveRecordingBakeToolName},
                      {"version", liveRecordingBakeToolVersion}}},
            {"source",
             {{"file", relativeToTable(source, out).generic_string()},
              {"metadata",
               {{"sha256", source_sha256},
                {"t0_seconds", table.t0_seconds},
                {"rate", 1.0 / table.layout.hop_seconds},
                {"layout", sampleTableLayoutJson(table.layout)}}}}},
            {"outputs", OrderedJson::array({{{"file", out.filename().generic_string()},
                                             {"schema", liveRecordingTableSchema},
                                             {"version", liveRecordingTableSchemaVersion},
                                             {"sha256", table_sha256}}})}};
}

LiveRecordingBakeResult bakeLiveRecording(
    const std::filesystem::path &source, const std::filesystem::path &out,
    double rate, const std::optional<std::vector<std::string>> &inputs,
    LiveRecordingCommitFault fault) {
    LiveRecordingTable table;
    std::string source_sha256;
    try {
        const auto bytes = readFile(source);
        source_sha256 = sha256Hex(bytes);
        const auto recording = LiveInputRecording::fromJsonLines(bytes);
        const double effective_rate = rate == 0.0
            ? recording.clock.mode == LiveRecordingClockMode::fixed ? 1.0 / recording.clock.fixed_step : 60.0
            : rate;
        table = makeLiveRecordingTable(recording, effective_rate, inputs);
    } catch (const GraphRegistryError &) {
        throw;
    } catch (const std::exception &error) {
        const std::string message = error.what();
        if (message.starts_with("live_input.recording_incomplete@1"))
            throw GraphRegistryError("live_input.recording_incomplete@1",
                                     {{"recording", source.generic_string()},
                                      {"reason", "missing_end"}});
        failed(message, {{"recording", source.generic_string()},
                         {"out", out.generic_string()}});
    }

    const auto manifest_path = derivedAssetManifestPath(out);
    const auto height = table.layout.rows_per_feature *
                        static_cast<std::uint32_t>(table.layout.features.size());
    const auto ktx = writeKtx2(Ktx2Format::R32Sfloat, table.layout.width, height,
                               std::as_bytes(std::span{table.values}));
    const auto ktx_sha256 = hashBytes(ktx);
    const auto manifest = liveRecordingManifest(source, source_sha256, table, out,
                                                ktx_sha256).dump(2) + "\n";
    auto staged_table = out;
    staged_table += ".live-bake.tmp";
    auto staged_manifest = manifest_path;
    staged_manifest += ".live-bake.tmp";
    std::error_code cleanup_error;
    try {
        writeStaged(staged_table, ktx);
        writeStaged(staged_manifest, std::as_bytes(std::span{manifest}));
        if (sha256Hex(readFile(staged_table)) != ktx_sha256 ||
            sha256Hex(readFile(staged_manifest)) != sha256Hex(manifest))
            throw std::runtime_error("staged live recording table failed hash verification");
    } catch (...) {
        std::filesystem::remove(staged_table, cleanup_error);
        std::filesystem::remove(staged_manifest, cleanup_error);
        throw;
    }

    try {
        std::lock_guard lock{derivedAssetIoMutex()};
        const auto old_table = readOldFile(out);
        const auto old_manifest = readOldFile(manifest_path);
        bool table_replaced = false;
        bool manifest_replaced = false;
        try {
            replaceFile(staged_table, out);
            table_replaced = true;
            if (fault == LiveRecordingCommitFault::after_table)
                throw std::runtime_error("injected failure after live table commit");
            replaceFile(staged_manifest, manifest_path);
            manifest_replaced = true;
            if (fault == LiveRecordingCommitFault::after_manifest)
                throw std::runtime_error("injected failure after live manifest commit");
        } catch (...) {
            std::exception_ptr rollback_error;
            try {
                if (manifest_replaced) restoreFile(manifest_path, old_manifest);
                if (table_replaced) restoreFile(out, old_table);
            } catch (...) {
                rollback_error = std::current_exception();
            }
            std::filesystem::remove(staged_table, cleanup_error);
            std::filesystem::remove(staged_manifest, cleanup_error);
            if (rollback_error) std::rethrow_exception(rollback_error);
            throw;
        }
    } catch (const FileReplaceLocked &error) {
        failed(error.what(), {{"recording", source.generic_string()},
                              {"out", out.generic_string()},
                              {"reason", "output_locked"},
                              {"path", error.path().generic_string()}});
    } catch (const GraphRegistryError &) {
        throw;
    } catch (const std::exception &error) {
        failed(error.what(), {{"recording", source.generic_string()},
                              {"out", out.generic_string()}});
    }

    LiveRecordingBakeResult result;
    result.layout = table.layout;
    result.t0_seconds = table.t0_seconds;
    result.rows.reserve(table.layout.features.size());
    for (std::uint32_t row = 0; row < table.layout.features.size(); ++row)
        result.rows.push_back(row);
    return result;
}

} // namespace Pelican
