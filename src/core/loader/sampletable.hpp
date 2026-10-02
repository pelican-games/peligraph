#pragma once

#include "../../project/audioanalysisformat.hpp"

#include <filesystem>
#include <memory>
#include <vector>

// AB-1: a baked table (audioanalysisformat.hpp) as value.sample_table reads
// it on the CPU: the R32_SFLOAT KTX2's values and the layout its
// `<file>.import.json` records.
namespace Pelican {

struct SampleTable {
    SampleTableLayout layout;
    std::vector<float> values;

    // sampleTableValue: the two samples either side of seconds / hop, linearly;
    // the end's value outside.
    double sample(std::uint32_t feature, double seconds) const {
        return sampleTableValue(layout, values, feature, seconds);
    }
};

// The table at `file`, shared by every graph that reads it until the file or
// its manifest changes (their write time and size). Throws std::runtime_error
// saying what is missing: the file, a manifest beside it, the layout in it,
// or a table of that layout (an R32_SFLOAT 2D KTX2 of width x
// rows_per_feature * features rows).
std::shared_ptr<const SampleTable> loadSampleTable(const std::filesystem::path &file);

} // namespace Pelican
