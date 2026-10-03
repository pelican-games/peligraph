#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "graphcanvasrenderfields.hpp"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

class QComboBox;
class QLabel;
class QLineEdit;
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
// CT-6s, decision 3: New... beside Shader writes a starting set into the
// project -- Ray tracing (raygen + miss + closest hit), under a name it
// proposes (rayTracingTemplateName) -- and makes the task trace with it.
// Named `pelican.graphCanvasCompute.<field>`.
class GraphCanvasComputeFields final : public QWidget {
  public:
    GraphCanvasComputeFields(GraphCanvasModel &model, std::function<void()> submitted,
                             QWidget *parent = nullptr);
    // The node to show and the scope it stands in, or null to hide.
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    // Off while the model cannot take an edit.
    void followEditable();
    // CT-6s: where New... writes; none, and it is off.
    void setProjectRoot(const std::filesystem::path &root);
    // CT-6s: New... wrote shaders; the owner reads the project's lists again.
    void setShadersChanged(std::function<void()> changed) { shaders_changed_ = std::move(changed); }

  private:
    void load(const std::string &reference);
    void syncStatus(const std::string &reference);
    void commit();
    void proposeTemplateName();
    void createTemplate();
    void templateNotice(const QString &text, bool failure);

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::function<void()> shaders_changed_;
    std::filesystem::path project_root_;
    std::string node_;
    std::string scope_;
    std::string shown_;
    // The whole reflection error Copy message copies.
    QString message_;
    QComboBox *shader_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *copy_ = nullptr;
    // CT-6s, decision 3: New... and its row (kind, name, Create, a line).
    QPushButton *new_shader_ = nullptr;
    QWidget *template_row_ = nullptr;
    QComboBox *template_kind_ = nullptr;
    QLineEdit *template_name_ = nullptr;
    QPushButton *create_shader_ = nullptr;
    QLabel *template_notice_ = nullptr;
    // The name last proposed: a name typed over it is kept.
    std::string auto_template_name_;
    // PS-1s, decisions 1, 3 and 4: Every frame / Every view / Once, and
    // Recompute (`pelican.graphCanvasCompute.schedule` / `.recompute`).
    GraphCanvasScheduleRow *schedule_ = nullptr;
};

} // namespace PelicanStudio
