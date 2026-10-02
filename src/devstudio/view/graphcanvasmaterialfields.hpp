#pragma once

#include "../model/graphcanvasmodel.hpp"

#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QLabel;
class QTimer;
class QTreeWidget;

namespace PelicanStudio {

class AttachmentOpsForm;
class GraphCanvasHeldEdits;
class MaterialSettingsForm;

// UX-11, decisions 1, 2 and 4: the rest of the selected render.material_pass's
// settings beside the Graph tab's canvas, below its Contract and Variant (the
// Draw fields'), in the Pass Editor's own forms:
//   - UX-4a's MaterialSettingsForm: Tags (material_filter, the tag picker),
//     Range, Outputs (material_outputs and their states), Resources
//     (screen_inputs, surface_resources, material_resources) and Other
//     (gpu_draw_source as JSON);
//   - UX-4a's AttachmentOpsForm: Load / Clear / Store of each colour socket
//     and of depth -- a graph pass names no output list, so they are written
//     as the pass's color_load_op, color_store_op, clear_color,
//     depth_load_op and depth_store_op, as the fullscreen pass's are (UX-5b);
//   - Materials drawn: the frame plan's routes to the pass and the engine's
//     materials its filter and variant take, with their tags; a row opens
//     the Frame Plan's Materials rows of the pass. UX-12c, decision 1: once
//     the plan says what the pass drew in its last frame (engine MP-1's
//     drawn_materials), the rows are those materials and their draws; the
//     approximation stays for a plan that does not say.
// Each change is one set_node_parameters of the node's whole parameters (one
// Undo step); a value typed (a name, a clear colour, Other) goes when the
// typing stops. The forms' children are named
// `pelican.graphCanvasMaterial.<field>` (the Pass Editor's names under the
// Graph's prefix), the list `pelican.graphCanvasMaterial.drawn`.
class GraphCanvasMaterialPassFields final : public QWidget {
  public:
    GraphCanvasMaterialPassFields(GraphCanvasModel &model, std::function<void()> submitted,
                                  QWidget *parent = nullptr);
    ~GraphCanvasMaterialPassFields() override;

    // The node to show and the scope it stands in, or null to hide.
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    // Off while the model cannot take an edit (Q-18).
    void followEditable();
    // UX-9, decision 2: a value given while the model was busy waits for it.
    [[nodiscard]] bool valuePending() const noexcept;
    void setPendingChanged(std::function<void()> changed);
    // Decision 4: a Materials drawn row was pressed -- the owner brings the
    // Frame Plan's Materials rows of `pass` (the plan's name) forward.
    void setMaterialsAction(std::function<void(const QString &pass)> action);

    // The parameters `parameters` become with the forms' values: the
    // material keys the forms show (not material_contract and
    // material_variant, which are the Draw fields'), and the attachment ops.
    // `loaded_ops` is what the ops were when the forms were loaded, so the
    // row that moved says what the pass's ops become. Throws while a typed
    // value is not one yet (Other, a clear colour) or a
    // screen input or surface resource reads nothing yet (UX-12c).
    [[nodiscard]] static nlohmann::json withForms(const nlohmann::json &parameters,
                                                  const MaterialSettingsForm &settings,
                                                  const AttachmentOpsForm &ops,
                                                  const nlohmann::json &loaded_ops);

    // Decision 4: the plan pass a root material pass node made, if the last
    // plan lists it.
    [[nodiscard]] static const GraphCanvasPlanPass *planPassOf(const GraphCanvasModel &model,
                                                              const std::string &node);

  private:
    void load(const nlohmann::json &parameters);
    void loadOps(const nlohmann::json &parameters);
    void offerInputNames();
    void showDrawn(const nlohmann::json &parameters);
    // UX-12c, decision 1: the rows of what the plan says the pass drew.
    void showDrawnMaterials(const GraphCanvasPlanPass &pass);
    void commit();
    void formChanged();
    void release();

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::function<void(const QString &)> materials_action_;
    std::string node_;
    std::string scope_;
    std::string shown_;
    std::string drawn_shown_;
    bool loading_ = false;
    QWidget *fields_ = nullptr;
    MaterialSettingsForm *settings_ = nullptr;
    AttachmentOpsForm *ops_ = nullptr;
    // The colour sockets the ops were loaded for, and their values then.
    std::vector<std::string> ops_sockets_;
    nlohmann::json loaded_ops_ = nlohmann::json::object();
    // The lists the forms offer, as last given to them.
    std::vector<std::string> variant_candidates_;
    std::vector<std::string> target_candidates_;
    QLabel *drawn_heading_ = nullptr;
    QTreeWidget *drawn_ = nullptr;
    QTimer *typing_ = nullptr;
    std::unique_ptr<GraphCanvasHeldEdits> held_;
};

} // namespace PelicanStudio
