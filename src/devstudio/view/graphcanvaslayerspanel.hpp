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
class QPushButton;
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
    // UX-8's rule for the dialogs: the pictures picked or dropped.
    void addImages(const std::vector<std::filesystem::path> &files);
    void importPsdOutput(const std::filesystem::path &graph_json);

  protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    struct RowWidgets {
        QWidget *row = nullptr;
        QCheckBox *visible = nullptr;
        QToolButton *clip = nullptr;
        QPushButton *pick = nullptr;
        QLabel *mask = nullptr;
        QLineEdit *name = nullptr;
        QToolButton *pass_through = nullptr;
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
    void chooseImages(bool mask);
    void chooseFile(const QString &title, const QString &name, const QString &filter,
                    std::function<void(const QStringList &)> picked);
    void addMaskImage(const std::filesystem::path &file);
    void importPsd(const std::filesystem::path &psd);
    // The stack to show when none is picked: the first output's of the root.
    void settleStack();

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
};

} // namespace PelicanStudio
