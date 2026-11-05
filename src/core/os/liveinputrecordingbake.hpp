#pragma once

#include "liveinputrecording.hpp"
#include "../../project/audioanalysisformat.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

inline constexpr std::string_view liveRecordingBakeToolName = "pelican.bake_live_recording";
inline constexpr std::string_view liveRecordingBakeToolVersion = "1";
inline constexpr std::string_view liveRecordingTableSchema = "pelican.live_recording_table";
inline constexpr int liveRecordingTableSchemaVersion = 1;
inline constexpr std::string_view liveRecordingBakeInvalidCode = "live_input.bake_invalid@1";
inline constexpr std::string_view liveRecordingBakeFailedCode = "live_input.bake_failed@1";

struct LiveRecordingTable {
    SampleTableLayout layout;
    double t0_seconds = 0.0;
    std::vector<float> values;
};

// This is a fixed-grid approximation, not a curve conversion. A change between
// grid points is visible up to one hop late; points past the recording's end
// hold its last value. Feature order is the recording's declaration order.
LiveRecordingTable makeLiveRecordingTable(
    const LiveInputRecording &recording, double rate,
    const std::optional<std::vector<std::string>> &inputs = std::nullopt);

nlohmann::ordered_json liveRecordingManifest(
    const std::filesystem::path &source, const std::string &source_sha256,
    const LiveRecordingTable &table, const std::filesystem::path &out,
    const std::string &table_sha256);

struct LiveRecordingBakeResult {
    SampleTableLayout layout;
    double t0_seconds = 0.0;
    // One entry per feature, in table order.
    std::vector<std::uint32_t> rows;
};

enum class LiveRecordingCommitFault : std::uint8_t {
    none,
    after_table,
    after_manifest,
};

// Reads and hashes the same recording bytes, stages and verifies both outputs,
// then replaces them under the derived-asset I/O lock. A failure restores both
// previous files (or leaves no files when there were none). The fault argument
// is deterministic test coverage for the two rollback points.
LiveRecordingBakeResult bakeLiveRecording(
    const std::filesystem::path &source, const std::filesystem::path &out,
    double rate, const std::optional<std::vector<std::string>> &inputs = std::nullopt,
    LiveRecordingCommitFault fault = LiveRecordingCommitFault::none);

} // namespace Pelican
