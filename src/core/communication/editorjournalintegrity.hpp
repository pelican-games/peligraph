#pragma once

// Unpack journal integrity validation.
//
// The unpack/restore journal pair carries its own consistency contract: the
// two wires must be exact inverses, the dependency witness must still match
// the live prefab closure, and the origin transition must cover every expanded
// component. These checks read only the wire plus the SceneProjectionState, so
// they live outside editorjournal.cpp and outside EditorEditCoordinator::Impl.

#include "editorjournalinternal.hpp"

#include "../loader/resolvedscene.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Pelican {
namespace EditorJournalInternal {

// Parses the `expanded_components` array of an unpack operation.
std::vector<OriginTransitionAuthority> unpackAuthorityFromJson(const Json &value);

bool isCanonicalUnpackOperation(const Json &operation);

// Returns the name of the first inconsistency between an unpack forward wire
// and its inverse, or nullopt when the pair is internally consistent.
std::optional<std::string> unpackJournalIntegrityFailureImpl(
    const OrderedJson &forward, const OrderedJson &inverse);

// Rejects an unpack operation whose recorded dependency witness no longer
// matches the live prefab closure.
void requireUnpackDependencies(const OrderedJson &operation,
                               const SceneProjectionState &state);

bool journalUsesUnpack(const EditorJournalRecord &record);

void requireUnpackJournalRecord(const EditorJournalRecord &record,
                                const SceneProjectionState &state);

} // namespace EditorJournalInternal
} // namespace Pelican
