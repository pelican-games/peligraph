#include "projectmigrationteststep.hpp"

#include <string>
#include <utility>

namespace Pelican {

void registerTestSceneKeyRenameMigrationStep(DocumentMigrationRegistry &registry) {
    registry.registerStep(DocumentMigrationStep{
        .id = std::string{testSceneKeyRenameStepId},
        .schema = "pelican.scene",
        .from_version = 2,
        .to_version = 3,
        .description = "test only: renames the top-level `scenes` to `scene_table`",
        .plan =
            [](const nlohmann::ordered_json &document, const ProjectContext &) {
                StepResult result;
                if (document.contains("scene_table")) {
                    result.document = document;
                    result.manual_items.push_back(MigrationManualItem{
                        .code = "pelican.scene.test_scene_table_taken@1",
                        .pointer = "/scene_table",
                        .message = "scene_table already exists; move `scenes` "
                                   "into it by hand",
                    });
                    return result;
                }
                result.document = nlohmann::ordered_json::object();
                for (const auto &[key, value] : document.items()) {
                    result.document[key == "scenes" ? "scene_table" : key] = value;
                }
                return result;
            },
    });
}

} // namespace Pelican
