#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "fullscreenshadertemplate.hpp"
#include "graphcanvasrenderfields.hpp"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QGridLayout;
class QFormLayout;

namespace PelicanStudio {

class GraphParameterEditor;

// --- RTL-2s (engine RTL-1a / RTL-1b): a render.compute's ray tracing in a
// project with a ray group layout.

// Whether a render.compute with these parameters shows the ray tracing
// section (a ray tracing task, a layout) instead of the generic row.
[[nodiscard]] bool rayLayoutSectionShown(const GraphCanvasModel &model, const nlohmann::json &parameters);
// What a shader written for the layout needs to know of it: the layout
// document as get_ray_layout answers it (GraphCanvasModel::rayLayoutRead,
// RTL-2s-c -- Studio reads no layout file of its own). Nullopt without a
// layout.
[[nodiscard]] std::optional<RayLayoutTemplateNames> rayLayoutTemplateNamesFor(const GraphCanvasModel &model);

// RTL-2s, decision 1: the section of the compute fields a ray tracing task
// of a project with a ray group layout shows, in the layout's terms, beside
// the Shader row (`pelican.graphCanvasRayTracing.<field>`):
//   - raygen: its shaders (.rgen and the engine's) and New...;
//   - traced.<ray type>: a check per ray type of the layout; a ray type
//     checked takes the cells of the first traced one (the shaders it had,
//     so the edit is whole), unchecked loses its row;
//   - miss.<miss>: one row per miss of the layout, its shader and New...;
//   - hit.<ray type>.<group>.<closesthit|anyhit|intersection>: the hit
//     shaders' grid -- a row per traced ray type, a column per group;
//     intersection in a procedural group's column only; a cell without its
//     closest hit (or a procedural one without its intersection) is red;
//   - callable.<index> and callable.add / .new / .<index>.remove: the
//     callable shaders, in index order;
//   - callable_record, callable_records, max_recursion,
//     acceleration_structure: the catalog's editors (UX-12e);
//   - problem: the sentence of the engine's last refusal of the task's ray
//     tracing (CT-6s's table), the field it names red.
// New... writes one shader of that stage for that place (the layout's
// form, writeRayLayoutTemplate) under a free name and puts it there. Each
// change is one set_node_parameters of the node's whole parameters; the
// ray tracing written is the layout's form (no miss / closesthit).
// Without a layout, a ray tracing task shows `noLayout`, one line, and
// `enableHitGroups`, which opens the Ray layout (setOpenRayLayout).
class GraphCanvasRayTracingFields final : public QWidget {
  public:
    GraphCanvasRayTracingFields(GraphCanvasModel &model, std::function<void()> submitted,
                                QWidget *parent = nullptr);
    ~GraphCanvasRayTracingFields() override;
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    void followEditable();
    void setProjectRoot(const std::filesystem::path &root);
    void setShadersChanged(std::function<void()> changed) { shaders_changed_ = std::move(changed); }
    void setOpenRayLayout(std::function<void()> open);
    void setOpenCallableRecords(std::function<void()> open) { open_callable_records_ = std::move(open); }
    // RTL-3s, decision 4: a raygen switch's file write goes on the render
    // history (Undo writes `before` back, Redo `after`).
    using FileHistory = std::function<void(const std::string &label, const std::string &path, const std::string &before, const std::string &after)>;
    void setFileHistoryAction(FileHistory record) { file_history_ = std::move(record); }

  private:
    struct Parts;
    void rebuild(const RayLayoutRead &read, const nlohmann::json &ray_tracing);
    // RTL-2s-c: the raygen's two switches (Trace a second ray type, Call
    // callable 0), written into the raygen file.
    void setRaygenSwitch(std::string_view name, bool on);
    void showProblem();
    void send(nlohmann::json ray_tracing);
    [[nodiscard]] nlohmann::json current() const;
    void createShader(const RayLayoutTemplatePlace &place, const std::function<void(const std::string &)> &put);

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::function<void()> shaders_changed_;
    std::function<void()> open_ray_layout_;
    std::function<void()> open_callable_records_;
    FileHistory file_history_;
    std::filesystem::path project_root_;
    std::string node_;
    std::string scope_;
    std::string shown_;
    std::unique_ptr<Parts> parts_;
};

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
    void setShadersChanged(std::function<void()> changed) {
        shaders_changed_ = std::move(changed);
        if (ray_tracing_ != nullptr) ray_tracing_->setShadersChanged(shaders_changed_);
    }

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
    // RTL-2s, decision 1: the ray tracing in the layout's terms.
    GraphCanvasRayTracingFields *ray_tracing_ = nullptr;

  public:
    // RTL-2s: Enable hit groups (no layout yet) opens the Ray layout.
    void setOpenRayLayout(std::function<void()> open);
    // RTL-2s-c: Callable records... opens the callable records' values.
    void setOpenCallableRecords(std::function<void()> open) {
        ray_tracing_->setOpenCallableRecords(std::move(open));
    }
    void setFileHistoryAction(GraphCanvasRayTracingFields::FileHistory record) {
        ray_tracing_->setFileHistoryAction(std::move(record));
    }
};

} // namespace PelicanStudio
