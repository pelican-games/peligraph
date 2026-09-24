#pragma once

#include "../src/project/projectmigration.hpp"

#include <string_view>

namespace Pelican {

// MG-1: the test-only migration step. It exists in test binaries only; the
// production gate (wp380c_production_has_no_fault_setter_symbol) proves no
// shipped archive carries it, by symbol or by source.
//
// pelican.scene 2 -> 3: the top-level `scenes` member is renamed to
// `scene_table`. A document that already has a `scene_table` member keeps it
// and reports `pelican.scene.test_scene_table_taken@1` at /scene_table as a
// manual item, leaving `scenes` where it is.
inline constexpr std::string_view testSceneKeyRenameStepId =
    "pelican.test.scene_key_rename";

void registerTestSceneKeyRenameMigrationStep(DocumentMigrationRegistry &registry);

} // namespace Pelican
