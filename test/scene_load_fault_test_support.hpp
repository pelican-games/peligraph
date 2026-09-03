#pragma once

#include "../src/core/loader/sceneload.hpp"
#include "../src/core/loader/sceneparticipantmanifest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace Pelican {
class VertBufContainer;
class MaterialContainer;
}

namespace Pelican::test_support {

class ManifestSceneLoadFault;

void setSceneLoadFaultInjectorForTesting(
    SceneLoader &loader, SceneLoadFaultInjector *injector) noexcept;
void setDefaultSceneLoadFaultInjectorForTesting(
    SceneLoadFaultInjector *injector) noexcept;
void setMorphMetadataReleaseFailureForTesting(
    VertBufContainer &geometry, bool enabled) noexcept;
void setMaterialReleaseFailureForTesting(MaterialContainer &, bool) noexcept;
void setTextureReleaseFailureForTesting(MaterialContainer &, bool) noexcept;

inline std::string_view manifestMutationId(std::string_view owner,
                                           std::string_view field) {
    const auto rows = sceneParticipantManifest();
    const auto found = std::find_if(
        rows.begin(), rows.end(), [&](const auto &row) {
            return row.owner == owner && row.field == field;
        });
    return found == rows.end() ? std::string_view{} : found->mutation_id;
}

class ManifestSceneLoadFault final : public SceneLoadFaultInjector {
  public:
    struct Call {
        SceneLoadFaultPhase phase = SceneLoadFaultPhase::prepare;
        std::string_view participant;
        std::string_view mutation_id;
    };

  private:
    std::string_view selected_;
    bool enabled_ = false;
    std::array<Call, 128> calls_{};
    std::size_t call_count_ = 0;

  public:
    explicit ManifestSceneLoadFault(std::string_view mutation_id)
        : selected_{mutation_id} {}

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    void resetCalls() noexcept { call_count_ = 0; }

    bool shouldMutate(SceneLoadFaultPhase phase,
                      std::string_view participant_name,
                      std::string_view mutation_id) noexcept override {
        if (call_count_ < calls_.size()) {
            calls_[call_count_++] =
                Call{phase, participant_name, mutation_id};
        }
        return enabled_ && mutation_id == selected_;
    }

    bool saw(SceneLoadFaultPhase phase,
             std::string_view participant_name) const noexcept {
        return std::any_of(
            calls_.begin(), calls_.begin() + call_count_,
            [&](const Call &call) {
                return call.phase == phase &&
                       call.participant == participant_name &&
                       call.mutation_id == selected_;
            });
    }
};

} // namespace Pelican::test_support
