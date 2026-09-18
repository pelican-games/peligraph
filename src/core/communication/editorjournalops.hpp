#pragma once

// The prepare* operation family of the editor journal.
//
// Each entry point turns one raw RPC operation into a PreparedOperation: the
// projection commands that apply it, the forward and inverse journal wire, and
// the stable read/write sets. None of them touches EditorEditCoordinator::Impl
// - the whole family is a function of the raw operation plus the authoring
// document, which is why it can live outside editorjournal.cpp. The dispatcher
// (prepareRpcOperation) stays in editorjournal.cpp next to the prefab
// preparers.

#include "editorjournalinternal.hpp"

#include <functional>
#include <string_view>

namespace Pelican {
namespace EditorJournalInternal {

PreparedOperation prepareSet(const Json &raw,
                             const AuthoringSceneDocument &document);
PreparedOperation prepareInsertField(const Json &raw,
                                     const AuthoringSceneDocument &document);
PreparedOperation prepareRemoveField(const Json &raw,
                                     const AuthoringSceneDocument &document);
PreparedOperation prepareMoveField(const Json &raw,
                                   const AuthoringSceneDocument &document);
PreparedOperation prepareAdd(const Json &raw,
                             const AuthoringSceneDocument &document,
                             const std::function<bool(std::string_view)> &
                                 component_exists);
PreparedOperation prepareRemove(const Json &raw,
                                const AuthoringSceneDocument &document);
PreparedOperation prepareSpawn(const Json &raw,
                               const AuthoringSceneDocument &document,
                               std::string_view default_scene,
                               const std::function<bool(std::string_view)> &
                                   component_exists);
PreparedOperation prepareDestroy(const Json &raw,
                                 const AuthoringSceneDocument &document);
PreparedOperation prepareReparent(const Json &raw,
                                  const AuthoringSceneDocument &document);

} // namespace EditorJournalInternal
} // namespace Pelican
