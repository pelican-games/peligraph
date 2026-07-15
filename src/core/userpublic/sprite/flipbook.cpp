#include "flipbook.hpp"

#include "../gamecontext.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace Pelican::sprite {

struct FlipbookClip::State {
    std::vector<FlipbookFrame> frames;
    std::vector<double> frame_end_times;
    double duration_seconds = 0.0;
    FlipbookPlayback playback = FlipbookPlayback::loop;
};

FlipbookClip::State *FlipbookClip::createState(std::vector<FlipbookFrame> frames,
                                               FlipbookPlayback playback) {
    auto state = std::make_unique<State>();
    state->frames = std::move(frames);
    state->playback = playback;
    if (state->frames.empty()) throw std::invalid_argument("flipbook requires at least one frame");
    state->frame_end_times.reserve(state->frames.size());
    for (const auto &frame : state->frames) {
        if (frame.texture.empty()) throw std::invalid_argument("flipbook frame texture must not be empty");
        if (!std::isfinite(frame.duration_seconds) || frame.duration_seconds <= 0.0)
            throw std::invalid_argument("flipbook frame duration must be finite and positive");
        state->duration_seconds += frame.duration_seconds;
        if (!std::isfinite(state->duration_seconds))
            throw std::overflow_error("flipbook duration overflow");
        state->frame_end_times.push_back(state->duration_seconds);
    }
    return state.release();
}

FlipbookClip::FlipbookClip(std::vector<FlipbookFrame> frames,
                           FlipbookPlayback playback)
    : state_{createState(std::move(frames), playback)} {}

FlipbookClip::~FlipbookClip() { delete state_; }

FlipbookClip::FlipbookClip(FlipbookClip &&other) noexcept
    : state_{std::exchange(other.state_, nullptr)} {}

FlipbookClip &FlipbookClip::operator=(FlipbookClip &&other) noexcept {
    if (this == &other) return *this;
    delete state_;
    state_ = std::exchange(other.state_, nullptr);
    return *this;
}

std::size_t FlipbookClip::frameIndex(double local_time_seconds) const {
    if (state_ == nullptr) throw std::logic_error("cannot sample a moved-from flipbook");
    if (!std::isfinite(local_time_seconds))
        throw std::invalid_argument("flipbook sample time must be finite");
    double sample_time = std::max(0.0, local_time_seconds);
    if (state_->playback == FlipbookPlayback::loop) {
        sample_time = std::fmod(sample_time, state_->duration_seconds);
    } else if (sample_time >= state_->duration_seconds) {
        return state_->frames.size() - 1;
    }
    // Advance a single representable step so decimal authoring boundaries such
    // as 0.1 + 0.2 select the same frame as the mathematical boundary 0.3.
    // This avoids a broad epsilon that could swallow a legitimately short frame.
    const auto boundary_time =
        std::nextafter(sample_time, std::numeric_limits<double>::infinity());
    const auto found = std::upper_bound(state_->frame_end_times.begin(),
                                        state_->frame_end_times.end(), boundary_time);
    if (found == state_->frame_end_times.end())
        return state_->playback == FlipbookPlayback::loop ? 0 : state_->frames.size() - 1;
    return static_cast<std::size_t>(std::distance(state_->frame_end_times.begin(), found));
}

std::string_view FlipbookClip::texture(double local_time_seconds) const {
    const auto index = frameIndex(local_time_seconds);
    return state_->frames[index].texture;
}

double FlipbookClip::duration() const noexcept { return state_ != nullptr ? state_->duration_seconds : 0.0; }
std::size_t FlipbookClip::frameCount() const noexcept { return state_ != nullptr ? state_->frames.size() : 0; }
FlipbookPlayback FlipbookClip::playback() const noexcept {
    return state_ != nullptr ? state_->playback : FlipbookPlayback::loop;
}

bool FlipbookClip::apply(GameContext &context, GameObjectId object,
                         double local_time_seconds) const {
    return context.setSpriteTexture(object, texture(local_time_seconds));
}

} // namespace Pelican::sprite
