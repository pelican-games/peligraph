#include "seqplayer.hpp"
#include "../loader/sceneload.hpp"

#include "../build_features.hpp"
#include "../launchconfig.hpp"

namespace Pelican {

bool TransformSequenceFrame::isHidden(uint32_t) const { return false; }

TransformSequence TransformSequence::fromJsonLines(std::string_view) {
    throwBuildFeatureDisabled("PELICAN_WITH_SEQPLAYER", "transform_seq parsing is unavailable");
}

size_t TransformSequence::sampleIndex(double, bool) const {
    throwBuildFeatureDisabled("PELICAN_WITH_SEQPLAYER", "transform_seq sampling is unavailable");
}

const TransformSequenceFrame &TransformSequence::sample(double, bool) const {
    throwBuildFeatureDisabled("PELICAN_WITH_SEQPLAYER", "transform_seq sampling is unavailable");
}

TransformSequence loadTransformSequenceFile(const std::filesystem::path &) {
    throwBuildFeatureDisabled("PELICAN_WITH_SEQPLAYER", "transform_seq loading is unavailable");
}

SeqPlayer::SeqPlayer() {
    if (GET_MODULE(EngineLaunchConfig).play_seq) {
        throwBuildFeatureDisabled("PELICAN_WITH_SEQPLAYER", "--play-seq is unavailable");
    }
}

void SeqPlayer::applyCameraOverride() {}

void SeqPlayer::initializeInstances(const std::filesystem::path &) {}

void SeqPlayer::update(double) {}

void SeqPlayer::releaseInstancesForSceneLoad() {
    instances.clear();
    enabled = false;
}

SeqPlayer::PreparedSceneState SeqPlayer::snapshotPreparedScene() const {
    return PreparedSceneState{.enabled = enabled, .instances = instances};
}

SeqPlayer::PreparedSceneState SeqPlayer::prepareSceneLoad() const {
    return PreparedSceneState{};
}

void SeqPlayer::publishPreparedScene(
    PreparedSceneState &&prepared) noexcept {
    std::swap(enabled, prepared.enabled);
    instances.swap(prepared.instances);
}

void SeqPlayer::publishPreparedScene(PreparedSceneState &&prepared,
                                     SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::publish);
    publishPreparedScene(std::move(prepared));
}

} // namespace Pelican
