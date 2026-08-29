#pragma once

#include "authoringscenedocument.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Pelican {

// The only typed view of the authored JSON tree. Runtime/query targets do not
// include this authority header and therefore cannot acquire this type from an
// AuthoringSceneDocument.
class AuthoringSceneRawView {
    const nlohmann::json *document_ = nullptr;

    explicit AuthoringSceneRawView(const nlohmann::json &document) noexcept
        : document_{&document} {}

    friend class AuthoringSceneAuthority;
    friend class ResolvedSceneResolver;

  public:
    const nlohmann::json &documentJson() const noexcept { return *document_; }
    const nlohmann::json &scenesJson() const {
        return document_->at("scenes");
    }
};

// Authoring-only operations live behind a distinct interface. Editor journal,
// snapshot/save, and projection transaction code may depend on this interface;
// runtime readers must depend on resolvedscene.hpp instead.
class AuthoringSceneAuthority {
  public:
    static AuthoringSceneRawView rawView(
        const AuthoringSceneDocument &document) noexcept {
        return AuthoringSceneRawView{document.raw_document_};
    }

    static std::vector<AuthoringSceneView> query(
        const AuthoringSceneDocument &document) {
        return document.queryAuthoring();
    }

    static std::string encodeSemantic(
        const AuthoringSceneDocument &document) {
        return document.encodeSemanticAuthoring();
    }

    static AuthoringSceneDocument stage(
        const AuthoringSceneDocument &document, nlohmann::json raw_document,
        SceneRevision revision) {
        return document.stageAuthoring(std::move(raw_document), revision);
    }

    static AuthoringSceneDocumentStage structuralStage(
        const AuthoringSceneDocument &document) {
        return document.structuralStageAuthoring();
    }
};

} // namespace Pelican
