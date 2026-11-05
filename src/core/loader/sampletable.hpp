#pragma once

#include "../../project/audioanalysisformat.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// AB-1: a baked table (audioanalysisformat.hpp) as value.sample_table reads
// it on the CPU: the R32_SFLOAT KTX2's values and the layout its
// `<file>.import.json` records.
namespace Pelican {

struct SampleTable {
    SampleTableLayout layout;
    std::vector<float> values;
    // IA-3: the two files the table was read from and the sha256 (lowercase
    // hex) of the bytes that were parsed. reload_render_documents hashes the
    // files again; the file watcher watches them.
    std::filesystem::path file;
    std::filesystem::path manifest;
    std::string file_sha256;
    std::string manifest_sha256;
    // Absolute scene time of sample zero. Audio manifests omit it and mean 0.
    double t0_seconds = 0.0;

    double sample(std::uint32_t feature, double seconds,
                  SampleTableInterpolation interpolation = SampleTableInterpolation::linear) const {
        return sampleTableValue(layout, values, feature, seconds - t0_seconds, interpolation);
    }
};

// The table at `file`, shared by every graph that reads it until the file or
// its manifest changes (IA-3: their bytes' sha256; a rewrite that keeps the
// write time and size is still a change). Throws std::runtime_error
// saying what is missing: the file, a manifest beside it, the layout in it,
// or a table of that layout (an R32_SFLOAT 2D KTX2 of width x
// rows_per_feature * features rows).
std::shared_ptr<const SampleTable> loadSampleTable(const std::filesystem::path &file);

} // namespace Pelican
