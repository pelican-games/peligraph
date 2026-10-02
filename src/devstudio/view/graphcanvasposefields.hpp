#pragma once

#include "../model/graphcanvasmodel.hpp"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;

namespace PelicanStudio {

// UX-10, decision 5: a pose.clip_sample's fields on the Graph tab, beside the
// Loop, Draw, Image, Pass and value fields. Rig is picked from the project's
// glTF / VRM files (project://...), Clip from the animations that file holds
// (read from its JSON, as the engine resolves a clip by its name), Speed is a
// spin and Loop a check box. Each change is one set_node_parameter (one Undo
// step). Named pelican.graphCanvasPose.rig / .clip / .speed / .loop / .note.
class GraphCanvasPoseFields final : public QWidget {
  public:
    GraphCanvasPoseFields(GraphCanvasModel &model, std::function<void()> submitted,
                          QWidget *parent = nullptr);
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    void followEditable();
    void setProjectRoot(const std::filesystem::path &root) { project_root_ = root; }

  private:
    void load();
    void loadClips(bool no_rigs);
    void commit(const std::string &key, const nlohmann::json &value);

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::filesystem::path project_root_;
    std::string node_;
    std::string scope_;
    nlohmann::json parameters_ = nlohmann::json::object();
    bool loading_ = false;
    QComboBox *rig_ = nullptr;
    QComboBox *clip_ = nullptr;
    QDoubleSpinBox *speed_ = nullptr;
    QCheckBox *loop_ = nullptr;
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
