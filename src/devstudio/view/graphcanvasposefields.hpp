#pragma once

#include "../model/graphcanvasmodel.hpp"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class QLabel;

namespace PelicanStudio {

class GraphCanvasParameterFields;

// UX-10, decision 5 / UX-12a, decision 5: a pose.clip_sample's fields on the
// Graph tab, beside the Loop, Draw, Image, Pass and value fields: the
// parameter fields made from the op's table (engine CT-3) -- rig an asset
// (the project's glTF / VRM files, with Import...), speed, start time, loop,
// root motion, root node -- with one thing the table cannot say: clip, which
// it types as a string, is picked from the animations the rig's file holds
// (read from its JSON, as the engine resolves a clip by its name). Each
// change is one set_node_parameters (one Undo step). Named
// pelican.graphCanvasPose.<parameter> (the parameter fields' names under
// this prefix) and pelican.graphCanvasPose.note.
class GraphCanvasPoseFields final : public QWidget {
  public:
    GraphCanvasPoseFields(GraphCanvasModel &model, std::function<void()> submitted,
                          QWidget *parent = nullptr);
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    void followEditable();
    void setProjectRoot(const std::filesystem::path &root);
    [[nodiscard]] bool valuePending() const noexcept;
    void setPendingChanged(std::function<void()> changed);

  private:
    void showNote(const nlohmann::json &parameters);

    std::filesystem::path project_root_;
    GraphCanvasParameterFields *fields_ = nullptr;
    QLabel *note_ = nullptr;
};

// The project's model files a rig can name (.gltf, .glb, .vrm), as
// project://<path>, sorted; dot directories are skipped.
std::vector<std::string> projectRigReferences(const std::filesystem::path &project_root);
// The animation names a glTF (or .glb / .vrm) file holds, in its order;
// empty when the file cannot be read.
std::vector<std::string> gltfAnimationNames(const std::filesystem::path &file);
// Where a project:// reference lives under the project; empty for another.
std::filesystem::path projectReferenceFile(const std::filesystem::path &project_root,
                                           const std::string &reference);

} // namespace PelicanStudio
