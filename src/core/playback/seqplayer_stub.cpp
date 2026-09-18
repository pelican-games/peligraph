#include "seqplayer.hpp"

#include "../build_features.hpp"
#include "../launchconfig.hpp"

namespace Pelican {

// Everything the disabled build feature actually changes. The prepared-scene
// snapshot/publish protocol is shared with the real build in
// playbackscenestate.cpp rather than copied here.

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

void SeqPlayer::initializeInstances(const std::filesystem::path &) {}

void SeqPlayer::update(double) {}

void SeqPlayer::releaseInstancesForSceneLoad() {
    instances.clear();
    enabled = false;
}

} // namespace Pelican
