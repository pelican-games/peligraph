#pragma once

#include "../model/graphcanvasmodel.hpp"

#include <QWidget>

#include <functional>
#include <string>

class QComboBox;
class QLabel;
class QPushButton;

namespace PelicanStudio {

// EL-5s, decisions 2 and 3: the fields of the selected render.compute,
// beside the Graph tab's canvas (as the Pass fields are a render pass's).
//   - Shader: the project's compute shaders (shaders/<name>.comp) and ray
//     tracing sets (.rgen / .rmiss / .rchit). Picking another one makes the
//     node's ports the new shader's (GraphCanvasModel::setComputeShader):
//     the ones it declares under the same name keep their lines.
//   - a line saying what the engine could not read (the shader's
//     reflection_error, its machine paths already project://), with Copy
//     message for the whole of it.
// Named `pelican.graphCanvasCompute.<field>`.
class GraphCanvasComputeFields final : public QWidget {
  public:
    GraphCanvasComputeFields(GraphCanvasModel &model, std::function<void()> submitted,
                             QWidget *parent = nullptr);
    // The node to show and the scope it stands in, or null to hide.
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    // Off while the model cannot take an edit.
    void followEditable();

  private:
    void load(const std::string &reference);
    void syncStatus(const std::string &reference);
    void commit();

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string node_;
    std::string scope_;
    std::string shown_;
    // The whole reflection error Copy message copies.
    QString message_;
    QComboBox *shader_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *copy_ = nullptr;
};

} // namespace PelicanStudio
