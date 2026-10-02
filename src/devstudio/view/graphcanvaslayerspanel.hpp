#pragma once

#include "../model/graphcanvaslayerstack.hpp"

#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

namespace PelicanStudio {

class GraphCanvasModel;

// SG-1 (design_canvas_2d 2, 4.3): a canvas.stack's layers, the way a paint
// program's Layers panel shows them, over the Graph tab's node fields. It
// shows the stack of the canvas node picked in the Graph (a stack, the
// output reading one, or a paint or shape on one) and keeps it while the
// pick goes elsewhere.
//
// Rows run top to bottom (the reverse of `layers`). Each row: the eye
// (`visible`), the kind -- a press picks the row, and its node in the Graph
// -- the name, edited in place (the default is Studio's: Rect 1...), and
// the opacity in percent. Under them + Rect, + Ellipse, + Group, + Stroke,
// Delete, Up and Down; under those, the picked row's shape and paint.
// Every change is one edit_graph_document (one Undo step).
//
// Named `pelican.canvasLayers` and, inside it, `.title`, `.row<n>` (n from
// the top) with `.row<n>.visible` / `.pick` / `.name` / `.opacity`,
// `.addRect`, `.addEllipse`, `.addGroup`, `.addStroke`, `.delete`, `.up`,
// `.down`, `.openGroup`, `.field.<parameter>[.<component>]`
// (`size.x`, `center.y`, `corner_radius`, `rotation`, `color`,
// `colorPick`, `width`, `align`) and `.note`.
class GraphCanvasLayersPanel final : public QWidget {
  public:
    GraphCanvasLayersPanel(GraphCanvasModel &model, std::function<void()> submitted,
                           QWidget *parent = nullptr);
    // The node picked in the Graph (none: nothing picked) in `scope`.
    void showSelection(const std::string &scope, const std::optional<std::string> &node);
    void followEditable();
    // A row was picked: its node is to be picked in the Graph.
    std::function<void(const std::string &scope, const std::string &node)> nodePicked;

    [[nodiscard]] const std::string &stack() const noexcept { return stack_; }

  private:
    struct RowWidgets {
        QWidget *row = nullptr;
        QCheckBox *visible = nullptr;
        QPushButton *pick = nullptr;
        QLineEdit *name = nullptr;
        QSpinBox *opacity = nullptr;
    };
    void reload(bool force);
    void rebuildRows();
    void loadFields();
    void pickRow(const std::string &input, bool tell);
    [[nodiscard]] std::optional<GraphCanvasStackRow> pickedRow() const;
    void commit(nlohmann::json ops);
    void commitParameter(const std::string &node, const std::string &key,
                         const nlohmann::json &value);

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string scope_;
    std::string stack_;
    // The picked row, by its input name (it keeps across a reorder).
    std::string picked_;
    std::vector<GraphCanvasStackRow> shown_rows_;
    std::string shown_stack_;
    bool loading_ = false;

    QLabel *title_ = nullptr;
    QWidget *rows_ = nullptr;
    QVBoxLayout *rows_layout_ = nullptr;
    QLabel *empty_ = nullptr;
    std::vector<RowWidgets> row_widgets_;
    QPushButton *add_rect_ = nullptr;
    QPushButton *add_ellipse_ = nullptr;
    QPushButton *add_group_ = nullptr;
    QPushButton *add_stroke_ = nullptr;
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
    QPushButton *open_group_ = nullptr;
    QLabel *note_ = nullptr;
};

} // namespace PelicanStudio
