#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "../model/graphfoldgroups.hpp"
#include "frameplanedit.hpp"
#include "graphcanvasreadcapability.hpp"

#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class QByteArray;
class QCheckBox;
class QComboBox;
class QLabel;
class QTimer;

namespace PelicanStudio {

class AttachmentOpsForm;
class FullscreenPassInterfaceRow;
class MaterialSettingsForm;
class ShaderDefinesForm;

// GL-5a, decisions 1, 2, 5 and 6. A folded node of the root canvas that stands
// for a pass the root's rendering section declares (Blank's present) is edited
// beside the canvas, in the Pass Editor's own parts:
//   - a fullscreen pass: its Vertex and Fragment shaders, picked from the
//     Pass Editor's list (fullscreenShaderChoices, the engine's once it
//     arrives); its shader defines (ShaderDefinesForm); Frame data
//     (FullscreenPassInterfaceRow); each colour output's Load / Clear / Store
//     and clear colour (AttachmentOpsForm). Its inputs are the node's sockets
//     (GL-1e), listed here;
//   - a material pass: its ten settings (MaterialSettingsForm) and its
//     attachments' ops (AttachmentOpsForm, depth too).
// A change is one edit_preset_pass, its params presetPassEditParams' -- the
// Pass Editor's Save's -- handed to the tab (send), which sends it, follows
// its ticket, puts a refusal on the notice in the inspector model's sentences
// and notes the edit for the tab's Undo (RU-1). A value typed (a define, a
// clear colour, a material setting) goes when the typing stops.
//
// Decision 5: Serial and Isolate of the pass, and No alias of each image it
// writes -- the root's target_planning, read with get_render_documents and
// written with edit_target_planning (graphTargetPlanningParams, the Frame
// Plan's right-click's). A box shows what the root holds.
//
// A pass of the preset or a feature shows nothing here (read only; its
// tooltip says Expand to edit…). Children are named
// `pelican.graphCanvasFoldedPass.<field>`: `.vertex`, `.fragment`,
// `.inputs`, `.output.<n>` (GL-5b: colour output n's target, picked from the
// declared ones or made with New target…), the forms' own fields, `.serial`,
// `.isolate`, `.noAlias.<resource>`.
class GraphCanvasFoldedPassFields final : public QWidget {
  public:
    using Send = std::function<void(const FramePlanEditRequest &request,
                                    std::function<FramePlanEditRequest()> rebuild)>;

    GraphCanvasFoldedPassFields(GraphCanvasModel &model, GraphCanvasReadCapability graph,
                                Send send, QWidget *parent = nullptr);
    ~GraphCanvasFoldedPassFields() override;

    // The folded node picked on the root canvas (its id), or empty. Cheap to
    // call again: the forms are built anew only when the pass or what the
    // authoring context says of it changed, never under a value being typed.
    void showPass(const std::string &pass);
    [[nodiscard]] const std::string &pass() const noexcept { return pass_; }
    // Whether the pass shown is one this column edits.
    [[nodiscard]] bool editing() const noexcept { return editable_; }
    // Off while the tab cannot send (an edit in flight, no connection).
    void followEditable(bool can_send);
    // The root document's reply (the planning decisions); false for any
    // other request.
    bool deliverResult(qint64 request_id, const QByteArray &bytes);
    // An edit of this column committed or was refused: the decisions are
    // read again, and a box put back to what the root holds.
    void editFinished();

    // The edit_preset_pass the forms stand for now, against the authoring
    // context the tab holds; a refusal (the one line) when they cannot be
    // sent -- a define the engine would refuse, a clear colour that is not
    // one yet.
    [[nodiscard]] FramePlanEditRequest passEditRequest() const;

    // GL-5b, decision 2 (b): an output's last entry, New target…, hands the
    // tab the pass, the output and the name proposedRenderTargetName gives
    // it; the tab opens New render target on it (presentNewRenderTarget).
    using NewTarget =
        std::function<void(const std::string &pass, std::size_t output, const std::string &name)>;
    void setNewTargetAction(NewTarget action) { new_target_ = std::move(action); }
    // The target New target… asked for is declared now: that output of the
    // pass writes it -- one edit_preset_pass, as a pick from the list is.
    void outputTargetCreated(const std::string &pass, std::size_t output, const std::string &target);
    // The engine refused this column's edit: the forms show what the root
    // holds again (a refused output is not carried into the next edit).
    void editRefused();

  private:
    [[nodiscard]] FramePlanAuthoring authoring() const;
    [[nodiscard]] const FramePlanAuthoringPass *row(const FramePlanAuthoring &authoring) const;
    void load(const FramePlanAuthoringPass &row);
    void fillShaderChoices(const nlohmann::json &declaration);
    void showPlanning();
    void readPlanning();
    void togglePlanning(const std::string &kind, const std::string &name, const std::string &field,
                        bool on);
    [[nodiscard]] nlohmann::json draftJson(const nlohmann::json &original) const;
    void formChanged();
    void commit();
    // GL-5b, decision 3: one combo per colour output.
    void fillOutputs();
    void outputPicked(std::size_t output, int index);
    void retarget(std::size_t output, const std::string &target);

    GraphCanvasModel &model_;
    GraphCanvasReadCapability graph_;
    Send send_;
    std::string pass_;
    std::string type_;
    bool editable_ = false;
    bool loading_ = false;
    bool can_send_ = true;
    // What the forms were built from (the declaration and the lists).
    std::string shown_;
    std::string choices_shown_;
    mutable std::optional<std::pair<std::uint64_t, FramePlanAuthoring>> authoring_;
    nlohmann::json original_ = nlohmann::json::object();
    nlohmann::json loaded_draft_ = nullptr;

    QLabel *heading_ = nullptr;
    QWidget *shaders_ = nullptr;
    QComboBox *vertex_ = nullptr;
    QComboBox *fragment_ = nullptr;
    QLabel *inputs_ = nullptr;
    ShaderDefinesForm *defines_ = nullptr;
    FullscreenPassInterfaceRow *interface_ = nullptr;
    QLabel *ops_caption_ = nullptr;
    AttachmentOpsForm *ops_ = nullptr;
    MaterialSettingsForm *settings_ = nullptr;
    QTimer *typing_ = nullptr;
    // GL-5b, decisions 2 and 3: `.output.<n>`, and what they were filled from.
    QWidget *outputs_ = nullptr;
    std::vector<QComboBox *> output_combos_;
    std::string outputs_shown_;
    NewTarget new_target_;

    // Decision 5.
    QWidget *planning_ = nullptr;
    QCheckBox *serial_ = nullptr;
    QCheckBox *isolate_ = nullptr;
    QWidget *no_alias_box_ = nullptr;
    std::map<std::string, QCheckBox *> no_alias_;
    GraphTargetPlanning planning_state_;
    qint64 planning_request_ = 0;
    // The document digest and pass the decisions were last read for.
    std::string planning_read_;
};

} // namespace PelicanStudio
