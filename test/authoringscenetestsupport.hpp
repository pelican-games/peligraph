#pragma once

#include "../src/core/loader/authoringsceneauthority.hpp"
#include "../src/core/loader/editorprojectiontransaction.hpp"

#include <utility>

namespace Pelican::test_support {

// Tests which exercise authoring persistence use this deliberately named
// authority facade.  Runtime-reader compile-negative fixtures never link or
// include it.
class AuthoringDocumentView {
    const AuthoringSceneDocument *document_ = nullptr;

  public:
    explicit AuthoringDocumentView(
        const AuthoringSceneDocument &document) noexcept
        : document_{&document} {}

    const nlohmann::json &rawJson() const noexcept {
        return AuthoringSceneAuthority::rawView(*document_).documentJson();
    }
    std::vector<AuthoringSceneView> query() const {
        return AuthoringSceneAuthority::query(*document_);
    }
    std::string encodeSemantic() const {
        return AuthoringSceneAuthority::encodeSemantic(*document_);
    }
    AuthoringSceneDocumentStage structuralStage() const {
        return AuthoringSceneAuthority::structuralStage(*document_);
    }
    AuthoringSceneDocument stage(nlohmann::json json,
                                 SceneRevision revision) const {
        return AuthoringSceneAuthority::stage(*document_, std::move(json),
                                              revision);
    }
};

inline AuthoringDocumentView authoring(
    const AuthoringSceneDocument &document) noexcept {
    return AuthoringDocumentView{document};
}

class PublishedAuthoringDocumentFacade {
    SceneProjectionState *state_ = nullptr;

  public:
    explicit PublishedAuthoringDocumentFacade(
        SceneProjectionState &state) noexcept
        : state_{&state} {}

    operator const AuthoringSceneDocument &() const noexcept {
        return state_->authoring();
    }
    const AuthoringSceneDocument &get() const noexcept {
        return state_->authoring();
    }
    SceneRevision revision() const noexcept { return state_->revision(); }
    const nlohmann::json &rawJson() const noexcept {
        return AuthoringSceneAuthority::rawView(state_->authoring())
            .documentJson();
    }
    std::vector<AuthoringSceneView> query() const {
        return AuthoringSceneAuthority::query(state_->authoring());
    }
    std::string encodeSemantic() const {
        return AuthoringSceneAuthority::encodeSemantic(state_->authoring());
    }
    AuthoringSceneDocument stage(nlohmann::json json,
                                 SceneRevision revision) const {
        return AuthoringSceneAuthority::stage(state_->authoring(),
                                              std::move(json), revision);
    }
    void swap(AuthoringSceneDocument &next) {
        auto candidate = ResolvedSceneResolver::prepare(
            std::move(next),
            SceneResolverGeneration{state_->resolverGeneration().value + 1U},
            state_->resolved().defaults());
        state_->swap(candidate);
    }
};

class SceneProjectionTarget : public EditorProjectionDocumentTarget {
  protected:
    SceneProjectionState state_;

  public:
    PublishedAuthoringDocumentFacade document;

    explicit SceneProjectionTarget(
        AuthoringSceneDocument source,
        ResolvedSceneDefaults defaults = {})
        : state_{ResolvedSceneResolver::prepare(
              std::move(source), SceneResolverGeneration{1}, defaults)},
          document{state_} {}

    const SceneProjectionState &projectionState() const override {
        return state_;
    }
    SceneRevision nextProjectionRevision() const override {
        return SceneRevision{state_.revision().value + 1U};
    }
    SceneResolverGeneration nextProjectionResolverGeneration()
        const override {
        return SceneResolverGeneration{
            state_.resolverGeneration().value + 1U};
    }
    void publishProjectionState(
        SceneProjectionState &candidate) noexcept override {
        state_.swap(candidate);
    }
};

} // namespace Pelican::test_support
