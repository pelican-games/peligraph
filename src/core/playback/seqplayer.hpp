#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"
#include "../renderer/modelinstance.hpp"

#include <filesystem>
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct SequenceTransform {
    glm::vec3 pos{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct TransformSequenceFrame {
    double time = 0.0;
    std::vector<SequenceTransform> transforms;
    std::vector<uint32_t> hidden;

    bool isHidden(uint32_t object_index) const;
};

class TransformSequence {
    double sequence_fps = 30.0;
    std::vector<std::string> object_names;
    std::vector<TransformSequenceFrame> frame_samples;

  public:
    static TransformSequence fromJsonLines(std::string_view json_lines);

    double fps() const { return sequence_fps; }
    std::span<const std::string> objects() const { return object_names; }
    std::span<const TransformSequenceFrame> frames() const { return frame_samples; }

    size_t sampleIndex(double time, bool loop) const;
    const TransformSequenceFrame &sample(double time, bool loop) const;
};

TransformSequence loadTransformSequenceFile(const std::filesystem::path &path);

DECLARE_MODULE(SeqPlayer) {
    bool enabled = false;
    bool loop = false;
    TransformSequence sequence;
    std::vector<ModelInstanceId> instances;
    std::optional<ModelTemplate> sequence_model;

    void applyCameraOverride();
    void initializeInstances(const std::filesystem::path &mesh_path);

  public:
    SeqPlayer();

    void update(double time);
    void releaseInstancesForSceneLoad();
    bool isEnabled() const { return enabled; }
    size_t instanceCountForTesting() const { return instances.size(); }
    ModelInstanceId instanceForTesting(size_t index) const {
        return instances.at(index);
    }
};

} // namespace Pelican
