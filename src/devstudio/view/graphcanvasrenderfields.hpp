#pragma once

#include "../model/graphcanvasmodel.hpp"

#include <QDialog>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTimer;
class QToolButton;

namespace PelicanStudio {

class AttachmentOpsForm;
class FullscreenPassInterfaceRow;
class RasterStateForm;
class ShaderDefinesForm;

// UX-2, decision 1: the fields of the selected render.image, beside the Graph
// tab's canvas above the details (as the Loop, Draw and View fields). Every
// value is picked, none typed as JSON:
//   - format: the engine's formats (get_render_authoring_context's
//     render_target_vocabulary), the depth ones for a depth type;
//   - type: the catalog's image types by name, and a combo per argument with
//     the values its schema allows;
//   - usage: a check per flag the engine names;
//   - size: a scale of the screen, or a fixed width and height; layers.
// Each change is one set_node_parameters of the node's whole parameters (one
// Undo step), the keys the fields do not show travelling as they were.
// Without the vocabulary (an engine before it) format and usage fall back to
// text. Named `pelican.graphCanvasImage.<field>`.
class GraphCanvasImageFields final : public QWidget {
  public:
    GraphCanvasImageFields(GraphCanvasModel &model, std::function<void()> submitted,
                           QWidget *parent = nullptr);
    // The node to show and the scope it stands in, or null to hide.
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    // Off while the model cannot take an edit, as the Draw fields (Q-18).
    void followEditable();

  private:
    void load(const nlohmann::json &parameters);
    void loadArguments(const nlohmann::json &parameters);
    void loadUsage(const nlohmann::json &parameters);
    // One set_node_parameters; a refused one shows the document again.
    void commit(const nlohmann::json &parameters);
    void commitFormat(const QString &format);
    void commitSemantic();
    void commitArgument(const std::string &name, QComboBox *combo);
    void commitUsage();
    void commitSize();
    [[nodiscard]] nlohmann::json parameters() const;

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string node_;
    std::string scope_;
    std::string shown_;
    QLabel *heading_ = nullptr;
    QWidget *fields_ = nullptr;
    QComboBox *format_ = nullptr;
    QLineEdit *format_line_ = nullptr;
    QComboBox *semantic_ = nullptr;
    QWidget *arguments_ = nullptr;
    QFormLayout *arguments_form_ = nullptr;
    std::vector<QComboBox *> argument_boxes_;
    QWidget *usage_ = nullptr;
    std::vector<std::pair<std::string, QCheckBox *>> usage_boxes_;
    QLineEdit *usage_text_ = nullptr;
    QRadioButton *size_scale_ = nullptr;
    QRadioButton *size_fixed_ = nullptr;
    QDoubleSpinBox *extent_scale_ = nullptr;
    QSpinBox *width_ = nullptr;
    QSpinBox *height_ = nullptr;
    QSpinBox *layers_ = nullptr;
    // UX-8, decision 12 (a) (RS-15): the mip count (0 reads "full"), and
    // behind the f button the layer and mip counts as integer expressions
    // too (a body's image: `6 - element`).
    QSpinBox *mip_levels_ = nullptr;
    QLineEdit *layers_expression_ = nullptr;
    QLineEdit *mip_levels_expression_ = nullptr;
    // UX-7, decision 6: `history: true`, named `pelican.graphCanvasImage.history`.
    QCheckBox *history_ = nullptr;
    QLabel *note_ = nullptr;
    // UX-5d, decision 1: the size as an expression ({"$expr": ...}), behind
    // the f button as the loop's Count is -- extent_scale for a screen-sized
    // image, width and height for a fixed one -- with the usual forms one
    // press away.
    QFormLayout *size_form_ = nullptr;
    QToolButton *size_expression_toggle_ = nullptr;
    QWidget *size_expression_row_ = nullptr;
    QLineEdit *extent_expression_ = nullptr;
    QLineEdit *width_expression_ = nullptr;
    QLineEdit *height_expression_ = nullptr;
    QPushButton *half_each_ = nullptr;
    QPushButton *double_each_ = nullptr;
    void showSizeExpression(bool expression);
    void putSizeExpression(bool half);
};

// UX-2, decision 2: the shaders of the selected render.fullscreen or
// render.raster, picked from the Pass Editor's list (the project's shaders
// and the engine's, fullscreenShaderChoices), and its ports as they are
// declared -- a pass's input sockets follow its shader's declarations (VI-1),
// so they are connected on the canvas, not typed. Each pick is one
// set_node_parameters. Named `pelican.graphCanvasPass.<field>`.
//
// UX-5b: a fullscreen pass is used as the Pass Editor's is.
//   - New... beside the fragment opens the Pass Editor's templates
//     (Pass-through, Deferred lighting, Tone map): Create writes the shader
//     into the project with the Pass Editor's own template functions and puts
//     it on the pass, in one edit with what the template needs (Uses scene
//     lights, Camera position) and -- for a template that reads the G-buffer
//     -- the lines from the pass that writes each image to its input
//     (fullscreenTemplateInputFill, the match UX-5a fixed);
//   - Frame data (uses_light_data, push_constants), the raster state's blend
//     (UX-4a's RasterStateForm) and each colour output's Load / Clear / Store
//     (UX-4a's AttachmentOpsForm, written as the pass's color_load_op,
//     color_store_op and clear_color: a graph pass names no output list).
class GraphCanvasPassFields final : public QWidget {
  public:
    GraphCanvasPassFields(GraphCanvasModel &model, std::function<void()> submitted,
                          QWidget *parent = nullptr);
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    void followEditable();
    // Where New... writes shaders/<name>.frag; none offers no New....
    void setProjectRoot(const std::filesystem::path &root);
    // Called after New... wrote a shader, so the lists hold it.
    void setShadersChanged(std::function<void()> changed);

  private:
    void load(const nlohmann::json &parameters);
    void loadPassSettings(const nlohmann::json &parameters);
    // UX-6a, decision 5: a row per image the pass reads, with the socket's
    // Filter, Address and Mip level (the right-click entries, which stay).
    void loadInputSettings(const nlohmann::json &parameters);
    void commitInputSetting(const std::string &port, const char *field, QComboBox *combo);
    void commit(const char *stage, QComboBox *combo);
    void commitSettings();
    void showTemplateRow(bool shown);
    void proposeTemplateName();
    void createTemplate();
    void templateNotice(const QString &text, bool failure);
    // The colour attachment ports, by location.
    [[nodiscard]] std::vector<std::string> colorPorts(const nlohmann::json &parameters) const;

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::function<void()> shaders_changed_;
    std::filesystem::path project_root_;
    std::string node_;
    std::string scope_;
    std::string op_;
    std::string shown_;
    bool loading_ = false;
    QLabel *heading_ = nullptr;
    QWidget *fields_ = nullptr;
    QFormLayout *form_ = nullptr;
    QComboBox *vertex_ = nullptr;
    QComboBox *fragment_ = nullptr;
    QPushButton *new_shader_ = nullptr;
    QWidget *template_row_ = nullptr;
    QComboBox *template_kind_ = nullptr;
    QLineEdit *template_name_ = nullptr;
    QPushButton *create_shader_ = nullptr;
    QLabel *template_notice_ = nullptr;
    std::string auto_template_name_;
    // The pass New... was last shown for (scope, node).
    std::string template_for_;
    QLabel *ports_ = nullptr;
    // LC-1s, decision 2: one line when the frame plan says the pass's shader
    // reads the loop element (`pelican.graphCanvasPass.loopElement`).
    QLabel *loop_element_ = nullptr;
    void showLoopElementRead();
    // Named `pelican.graphCanvasPass.input.<port>.filter` / `.address` /
    // `.mip`; a setting the socket cannot take is there, off, saying why.
    QWidget *inputs_ = nullptr;
    QFormLayout *inputs_form_ = nullptr;
    QWidget *settings_ = nullptr;
    FullscreenPassInterfaceRow *interface_ = nullptr;
    RasterStateForm *raster_ = nullptr;
    AttachmentOpsForm *outputs_ = nullptr;
    QTimer *settings_timer_ = nullptr;
    // The colour attachment settings as last loaded, to tell which row moved.
    nlohmann::json loaded_settings_ = nlohmann::json::object();

    // UX-7. The rows GB-3 had to type as JSON or could not write at all.
    // Decision 2: shader.defines, a row each (the Pass Editor's form, named
    // `pelican.graphCanvasPass.shaderDefines.*`); a row the engine would
    // refuse stays red and is not sent.
    ShaderDefinesForm *defines_ = nullptr;
    void loadDefines(const nlohmann::json &parameters);
    void commitDefines();
    // Decision 3: a raster pass's depth attachment -- none, a depth image of
    // the scope, or a new one (`pelican.graphCanvasPass.depth`) -- and its
    // raster_state with the depth rows (the Raster form, shown for a raster
    // pass too).
    QComboBox *depth_ = nullptr;
    QFormLayout *settings_form_ = nullptr;
    void loadDepth();
    void commitDepth();
    // Decision 4: a raster pass's draw: its vertex and instance counts, and
    // Grid N×N, which writes the 6·(N−1)² vertices of a grid of triangles
    // (`pelican.graphCanvasPass.vertexCount` / `.instanceCount` /
    // `.gridSize` / `.grid`).
    QWidget *draw_ = nullptr;
    QSpinBox *vertex_count_ = nullptr;
    QSpinBox *instance_count_ = nullptr;
    QSpinBox *grid_size_ = nullptr;
    QPushButton *grid_ = nullptr;
    void loadDraw(const nlohmann::json &parameters);
    void commitDraw();
    // A value given while the model cannot take an edit (a save or a read in
    // flight -- a pick's placement save, say) is not dropped: it is sent
    // again once the model can (followEditable), rather than the field going
    // back to the document's value under the hand that typed it.
    std::function<void()> retry_;
    QTimer *retry_timer_ = nullptr;
    bool deferWhileBusy(std::function<void()> again);
    void retryWhenEditable();
    // A count typed into a draw spin and not yet confirmed (no Return, no
    // focus out) is confirmed before the fields let go of the pass: a redraw
    // that shows no node for a moment hides them, and the hidden spin's
    // focus out found no pass to send it to.
    void flushDraw();
    std::function<void()> pending_changed_;

  public:
    // Whether a value given while the model was busy waits to be sent; the
    // owner's notice says "Saving" meanwhile, not the last save's words.
    [[nodiscard]] bool valuePending() const noexcept { return static_cast<bool>(retry_); }
    // Called when that changes.
    void setPendingChanged(std::function<void()> changed) {
        pending_changed_ = std::move(changed);
    }
};

// UX-3, decision 2: where the selected render node (fullscreen, raster,
// material pass, shadow_depth, snapshot_copy, compute) lands among the
// passes -- automatic, before or after a pass of the frame plan, or after a
// canonical anchor -- and the family (pass_set) it runs in. Each pick is one
// set_node_parameters. A compute task takes no place (graph.insert_on_compute)
// and says so. Named `pelican.graphCanvasPlace.<field>`.
class GraphCanvasPlaceFields final : public QWidget {
  public:
    GraphCanvasPlaceFields(GraphCanvasModel &model, std::function<void()> submitted,
                           QWidget *parent = nullptr);
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    void followEditable();
    // "before|<pass>", "after|<pass>", "anchor|<anchor>", "automatic|": the
    // data a Place item carries.
    [[nodiscard]] static QString placeData(const GraphCanvasPlace &place);

  private:
    void load();
    void commit();

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string node_;
    std::string scope_;
    std::string op_;
    std::string shown_;
    QWidget *fields_ = nullptr;
    QComboBox *place_ = nullptr;
    QComboBox *pass_set_ = nullptr;
    QLabel *note_ = nullptr;
};

// UX-7, decision 5: the small dialog the palette shows before it places an
// op the engine refuses without a parameter its catalog does not list
// (GraphCanvasModel::requiredParameters): one field each -- a buffer picked
// from the graph's or named anew, a number, one of the engine's clocks --
// and OK / Cancel. Named `pelican.graphCanvasRequired` and
// `pelican.graphCanvasRequired.<parameter>` (`.ok`, `.cancel`). What is
// written here can be changed afterwards in the node's details.
class GraphCanvasRequiredDialog final : public QDialog {
  public:
    GraphCanvasRequiredDialog(const GraphCanvasModel &model, const std::string &scope,
                              const std::string &op, QWidget *parent = nullptr);
    // Whether the op asks for anything at all.
    [[nodiscard]] bool asks() const noexcept { return !fields_.empty(); }
    // The parameters as the fields hold them now.
    [[nodiscard]] nlohmann::json parameters() const;
    // UX-8, decision 7: what the palette adds the node with --
    // {"parameters": ..., "inputs": {"voice": {node, port}}} (the voice a
    // sound sink drives is an edge).
    [[nodiscard]] nlohmann::json seed() const;

  private:
    void follow();
    std::vector<std::pair<GraphCanvasModel::RequiredParameter, QWidget *>> fields_;
    QPushButton *ok_ = nullptr;
};

// UX-3, decision 3: the entries a render node's input socket offers on a
// right-click -- Previous frame, the sampling filter and address, the mip and
// layer it reads, a colour attachment's location -- added to `menu` for the
// port. What the port cannot take is there, switched off, and says why. Each
// change is one set_node_parameters; `submitted` is called after it. Nothing
// is added for a port the node's `ports` does not declare. The entries are
// named `pelican.graphCanvasSocket.<field>`.
void addGraphCanvasSocketEntries(QMenu &menu, GraphCanvasModel &model, const std::string &scope,
                                 const std::string &node, const std::string &port,
                                 std::function<void()> submitted);

} // namespace PelicanStudio
