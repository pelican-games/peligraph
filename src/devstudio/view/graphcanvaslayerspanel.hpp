#pragma once

#include "../model/graphcanvaslayerstack.hpp"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

namespace PelicanStudio {

class GraphCanvasModel;

// SG-1 (design_canvas_2d 2, 4.3): a canvas.stack's layers, the way a paint
// program's Layers panel shows them. SG-2: a dock of its own ("Layers"),
// always there: it shows the stack the first canvas.output of the root reads
// (a combo when there are several) until a canvas node picked in the Graph
// names another, and with no canvas at all only "+ Canvas".
//
// Rows run top to bottom (the reverse of `layers`). Each row: the eye
// (`visible`), the clip mark (`↳`, clipped to the layer below; not where no
// unclipped layer lies below), the kind -- a press picks the row, and its
// node in the Graph -- the mask mark, the name, edited in place (the
// default is Studio's: Rect 1...), pass through for a group, and the opacity
// in percent. Under them + Rect, + Ellipse, + Group, + Stroke, + Image…,
// Import PSD output… (and Import PSD… when PELICAN_IMPORT_TOOLS names the
// converter), Delete, Up and Down; under those, the picked row's fields --
// its shape and paint, an image's offset, size and sampler, the blend, the
// mask and the Transform -- or, with no row picked, the stack's size and
// blend space. Every change is one edit_graph_document (one Undo step).
// Pictures dropped on the dock are + Image.
//
// An imported PSD is a group whose layers are its definition's: Open group
// shows them greyed, and an edit there says it is read only.
//
// Named `pelican.canvasLayers` and, inside it, `.title`, `.stacks`,
// `.addCanvas`, `.back`, `.row<n>` (n from the top) with `.row<n>.visible` /
// `.clip` / `.pick` / `.mask` / `.name` / `.passThrough` / `.opacity`,
// `.addRect`, `.addEllipse`, `.addGroup`, `.addStroke`, `.addImage`,
// `.importPsdOutput`, `.importPsd`, `.delete`, `.up`, `.down`, `.openGroup`,
// `.field.<parameter>[.<component>]` (`size.x`, `center.y`,
// `corner_radius`, `rotation`, `color`, `colorPick`, `width`, `align`,
// `offset.x`, `imageSize.x`, `sizeLock`, `sampler`, `blend`, `addMask`,
// `removeMask`, `transformToggle`, `translate.x`, `rotate`, `scale.x`,
// `stackSize.x`, `blendSpace`), `.note`, `.refusal`, `.imports` and
// `.importList`. The file dialogs are `pelican.canvasImageDialog` and
// `pelican.canvasPsdOutputDialog` (their name fields `.fileName`).
//
// SG-3: the blend combo offers every blend the engine draws, grouped as
// Photoshop's menu groups them; a row whose blend is not normal says it
// beside its name (`.row<n>.blend`), and a row with a filter shows `f`
// (`.row<n>.filters`). + Text… (`.addText`) opens `pelican.canvasTextDialog`
// (`.text`, `.font`, `.importFont`, `.size`, `.ok`; Import font… opens
// `pelican.canvasFontDialog`); + Adjustment (`.addAdjustment`) is a menu of
// the colour filters (`.addAdjustment.<kind>`, kind the op without
// `filter.`), put just above the picked row. A picked text row has
// `.field.text`, `.field.font`, `.field.importFont`, `.field.textSize`,
// `.field.textAlign`, `.field.lineHeight`, `.field.tracking`,
// `.field.boxWidth` and `.field.textStroke`; every picked row has its filters
// (`.field.filter`, `.field.addFilter` with `.addFilter.<kind>`,
// `.field.removeFilter`) and the picked filter's parameters
// (`.field.filter.<parameter>`, `.field.filter.colorPick`; CV-3a: + Filter
// also offers Gaussian Blur, Drop Shadow and Outer Glow, whose fields are
// `.field.filter.<kind>.<parameter>` -- `blur.radius`, `blur.maxRadius`,
// `drop_shadow.offset.x`, `drop_shadow.radius`, `drop_shadow.maxRadius`,
// `drop_shadow.spread`, `drop_shadow.opacity`, `drop_shadow.colorPick`, and
// outer_glow's the same less the offset). With no row
// picked: `.placeTarget` and `.placeInScene` (enabled only when the document
// has a material pass), and for a stack placed in 3D its canvas.place's
// fields (`.field.place.<parameter>[.<component>]`).
class GraphCanvasLayersPanel final : public QWidget {
  public:
    GraphCanvasLayersPanel(GraphCanvasModel &model, std::function<void()> submitted,
                           QWidget *parent = nullptr);
    // The node picked in the Graph (none: nothing picked) in `scope`.
    void showSelection(const std::string &scope, const std::optional<std::string> &node);
    void followEditable();
    // A row was picked: its node is to be picked in the Graph.
    std::function<void(const std::string &scope, const std::string &node)> nodePicked;
    // The project the pictures and imports are copied into.
    std::function<std::filesystem::path()> projectRoot;
    // + Canvas, and an import with no canvas to go on: `ops` place a
    // canvas.output (`output`) and its stack, and the owner sends them and
    // then puts the output's image on present's inputs (when the project has
    // a present). False when nothing was sent.
    std::function<bool(nlohmann::json ops, const std::string &output)> addCanvas;

    [[nodiscard]] const std::string &stack() const noexcept { return stack_; }
    // UX-8's rule for the dialogs: the pictures picked or dropped. NB-2b:
    // with `above`, the first goes just above that layer and each next one
    // above the one before (the node's + Image…); without, on top.
    void addImages(const std::vector<std::filesystem::path> &files,
                   std::optional<std::size_t> above = std::nullopt);

    // NB-2b (design_node_bodies 2.3): the row picked on a canvas.stack's
    // node is the dock's picked row, and the other way. `rowShown` says the
    // stack on show (its scope) and the picked row's input (empty: none)
    // each time either changes; pickLayer shows `stack` of `scope` with that
    // row picked (an empty input: none), and says nothing back.
    std::function<void(const std::string &scope, const std::string &stack, const std::string &input)> rowShown;
    void pickLayer(const std::string &scope, const std::string &stack, const std::string &input);
    // The node's + Text… and + Image…: the dock's dialogs, for the stack on
    // show, the layer going just above `above` (on top without one).
    void openTextDialog(std::optional<std::size_t> above = std::nullopt);
    void chooseImages(bool mask, std::optional<std::size_t> above = std::nullopt);
    void importPsdOutput(const std::filesystem::path &graph_json);

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    // SG-3: the text field is sent when it loses the keyboard.
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    struct RowWidgets {
        QWidget *row = nullptr;
        QCheckBox *visible = nullptr;
        QToolButton *clip = nullptr;
        QPushButton *pick = nullptr;
        QLabel *mask = nullptr;
        QLineEdit *name = nullptr;
        QToolButton *pass_through = nullptr;
        // SG-3: the blend when not normal, and `f` for a filtered layer.
        QLabel *blend = nullptr;
        QLabel *filters = nullptr;
        QSpinBox *opacity = nullptr;
    };
    // Where Open group came from, to go Back to.
    struct Place {
        std::string scope;
        std::string stack;
        bool read_only = false;
    };
    void reload(bool force);
    void rebuildRows();
    void loadFields();
    void pickRow(const std::string &input, bool tell);
    [[nodiscard]] std::optional<GraphCanvasStackRow> pickedRow() const;
    // `ops` sent as one edit; a layer they put on the stack is picked next.
    void commit(nlohmann::json ops);
    void commitParameter(const std::string &node, const std::string &key,
                         const nlohmann::json &value);
    void commitTransform();
    void say(const QString &line);
    // NB-2b: rowShown, when what it said last is not what is shown now.
    void tellRow();
    void chooseFile(const QString &title, const QString &name, const QString &filter,
                    std::function<void(const QStringList &)> picked);
    void addMaskImage(const std::filesystem::path &file);
    void importPsd(const std::filesystem::path &psd);
    // The stack to show when none is picked: the first output's of the root.
    void settleStack();
    // SG-3.
    void buildSg3Fields(QVBoxLayout *body);
    void loadSg3Fields(const std::optional<GraphCanvasStackRow> &row);
    void chooseFont(QWidget *over, std::function<void(const std::string &reference)> imported);
    void reloadFonts();
    void fillFontCombo(QComboBox *combo, const std::string &current);
    [[nodiscard]] std::string pickedFilter() const;
    void commitFilterNumber(const char *key, double value);
    void commitFilterJson(const char *key, QLineEdit *field);
    void commitPlace(const char *key, const nlohmann::json &value);
    [[nodiscard]] std::string shownPlace() const;
    QMenu *filterMenu(const char *prefix, const std::vector<GraphCanvasFilterChoice> &choices,
                      std::function<void(const std::string &op)> chosen);
    // CV-3a: a filter's colour from the colour dialog, and its max_radius (0: none).
    void pickFilterColor(const char *title);
    void commitFilterLimit(double value);

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string scope_;
    std::string stack_;
    bool read_only_ = false;
    std::vector<Place> trail_;
    // The picked row, by its input name (it keeps across a reorder).
    std::string picked_;
    std::vector<GraphCanvasStackRow> shown_rows_;
    std::string shown_stack_;
    std::vector<GraphCanvasOutputStack> shown_outputs_;
    // NB-2b: what rowShown said last (scope, stack, input).
    std::optional<std::array<std::string, 3>> told_row_;
    bool loading_ = false;
    bool transform_open_ = false;
    std::string transform_row_;
    double size_ratio_ = 1.0;

    QLabel *title_ = nullptr;
    QComboBox *stacks_ = nullptr;
    QPushButton *back_ = nullptr;
    QPushButton *add_canvas_ = nullptr;
    QWidget *body_ = nullptr;
    QWidget *rows_ = nullptr;
    QVBoxLayout *rows_layout_ = nullptr;
    QLabel *empty_ = nullptr;
    std::vector<RowWidgets> row_widgets_;
    QPushButton *add_rect_ = nullptr;
    QPushButton *add_ellipse_ = nullptr;
    QPushButton *add_group_ = nullptr;
    QPushButton *add_stroke_ = nullptr;
    QPushButton *add_image_ = nullptr;
    QPushButton *import_output_ = nullptr;
    QPushButton *import_psd_ = nullptr;
    QPushButton *delete_ = nullptr;
    QPushButton *up_ = nullptr;
    QPushButton *down_ = nullptr;

    QWidget *shape_fields_ = nullptr;
    QLabel *shape_caption_ = nullptr;
    QDoubleSpinBox *size_x_ = nullptr;
    QDoubleSpinBox *size_y_ = nullptr;
    QDoubleSpinBox *center_x_ = nullptr;
    QDoubleSpinBox *center_y_ = nullptr;
    QWidget *corner_row_ = nullptr;
    QDoubleSpinBox *corner_radius_ = nullptr;
    QDoubleSpinBox *rotation_ = nullptr;
    QWidget *paint_fields_ = nullptr;
    QComboBox *color_ = nullptr;
    QPushButton *color_pick_ = nullptr;
    QWidget *stroke_row_ = nullptr;
    QDoubleSpinBox *width_ = nullptr;
    QComboBox *align_ = nullptr;
    // SG-2.
    QWidget *image_fields_ = nullptr;
    QDoubleSpinBox *offset_x_ = nullptr;
    QDoubleSpinBox *offset_y_ = nullptr;
    QDoubleSpinBox *image_w_ = nullptr;
    QDoubleSpinBox *image_h_ = nullptr;
    QToolButton *size_lock_ = nullptr;
    QComboBox *sampler_ = nullptr;
    QWidget *layer_fields_ = nullptr;
    QComboBox *blend_ = nullptr;
    QLabel *mask_caption_ = nullptr;
    QPushButton *add_mask_ = nullptr;
    QPushButton *remove_mask_ = nullptr;
    QPushButton *transform_toggle_ = nullptr;
    QWidget *transform_fields_ = nullptr;
    QDoubleSpinBox *translate_x_ = nullptr;
    QDoubleSpinBox *translate_y_ = nullptr;
    QDoubleSpinBox *rotate_ = nullptr;
    QDoubleSpinBox *scale_x_ = nullptr;
    QDoubleSpinBox *scale_y_ = nullptr;
    QWidget *stack_fields_ = nullptr;
    QDoubleSpinBox *stack_w_ = nullptr;
    QDoubleSpinBox *stack_h_ = nullptr;
    QComboBox *blend_space_ = nullptr;
    QPushButton *open_group_ = nullptr;
    QLabel *note_ = nullptr;
    QLabel *refusal_ = nullptr;
    QPushButton *imports_ = nullptr;
    QLabel *import_list_ = nullptr;

    // SG-3.
    std::vector<std::string> fonts_;
    std::string place_;
    std::string filter_row_;
    QPushButton *add_text_ = nullptr;
    QToolButton *add_adjustment_ = nullptr;
    QWidget *text_fields_ = nullptr;
    QPlainTextEdit *text_ = nullptr;
    QComboBox *font_ = nullptr;
    QPushButton *import_font_ = nullptr;
    QComboBox *text_size_ = nullptr;
    QComboBox *text_align_ = nullptr;
    QDoubleSpinBox *line_height_ = nullptr;
    QDoubleSpinBox *tracking_ = nullptr;
    QDoubleSpinBox *box_width_ = nullptr;
    QCheckBox *text_stroke_ = nullptr;
    QWidget *filter_fields_ = nullptr;
    QComboBox *filter_ = nullptr;
    QToolButton *add_filter_ = nullptr;
    QPushButton *remove_filter_ = nullptr;
    QStackedWidget *filter_pages_ = nullptr;
    QDoubleSpinBox *brightness_ = nullptr;
    QDoubleSpinBox *contrast_ = nullptr;
    QDoubleSpinBox *input_floor_ = nullptr;
    QDoubleSpinBox *input_ceiling_ = nullptr;
    QDoubleSpinBox *gamma_ = nullptr;
    QDoubleSpinBox *output_floor_ = nullptr;
    QDoubleSpinBox *output_ceiling_ = nullptr;
    QLineEdit *channels_ = nullptr;
    QDoubleSpinBox *hue_ = nullptr;
    QDoubleSpinBox *saturation_ = nullptr;
    QDoubleSpinBox *lightness_ = nullptr;
    QLineEdit *colorize_ = nullptr;
    QLineEdit *stops_ = nullptr;
    QCheckBox *reversed_ = nullptr;
    QPushButton *overlay_pick_ = nullptr;
    // CV-3a: Gaussian Blur, Drop Shadow and Outer Glow; a max radius of 0 is
    // none (written by leaving it out).
    QDoubleSpinBox *blur_radius_ = nullptr;
    QDoubleSpinBox *blur_max_radius_ = nullptr;
    QDoubleSpinBox *shadow_offset_[2] = {};
    QDoubleSpinBox *shadow_radius_ = nullptr;
    QDoubleSpinBox *shadow_max_radius_ = nullptr;
    QDoubleSpinBox *shadow_spread_ = nullptr;
    QDoubleSpinBox *shadow_opacity_ = nullptr;
    QPushButton *shadow_color_ = nullptr;
    QDoubleSpinBox *glow_radius_ = nullptr;
    QDoubleSpinBox *glow_max_radius_ = nullptr;
    QDoubleSpinBox *glow_spread_ = nullptr;
    QDoubleSpinBox *glow_opacity_ = nullptr;
    QPushButton *glow_color_ = nullptr;
    QWidget *place_row_ = nullptr;
    QComboBox *place_target_ = nullptr;
    QPushButton *place_in_scene_ = nullptr;
    QWidget *place_fields_ = nullptr;
    QLabel *place_caption_ = nullptr;
    QDoubleSpinBox *position_[3] = {};
    QDoubleSpinBox *place_rotation_[3] = {};
    QDoubleSpinBox *place_scale_ = nullptr;
    QDoubleSpinBox *pixels_per_unit_ = nullptr;
    QDoubleSpinBox *anchor_[2] = {};
    QCheckBox *depth_test_ = nullptr;
    QCheckBox *depth_write_ = nullptr;
    QCheckBox *double_sided_ = nullptr;
};

} // namespace PelicanStudio
