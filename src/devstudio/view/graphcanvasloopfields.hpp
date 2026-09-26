#pragma once

#include "../model/graphcanvasmodel.hpp"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGridLayout;
class QLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

namespace PelicanStudio {

// LP-4c, decisions 3 and 4: the Loop fields beside the Graph tab's canvas, for
// the root graph.loop that is selected. The loop's parameters are edited here
// and nowhere else on the canvas: they are not ports, and no JSON is shown.
//
// Every field is one gesture on the model -- one edit_graph_document -- and
// `submitted` is called after each, so the owner posts what the model built.
// The fields are named `pelican.graphCanvasLoop.<field>` (the branch table's
// rows `pelican.graphCanvasLoop.branch<index>.<column>`), which is how a
// scenario reaches them.
class GraphCanvasLoopFields final : public QWidget {
  public:
    // `open_body` descends into the loop's body (decision 5), the same way a
    // double-click on the loop does.
    GraphCanvasLoopFields(GraphCanvasModel &model, std::function<void()> submitted,
                          std::function<void(const std::string &loop)> open_body,
                          QWidget *parent = nullptr);

    // The loop to show, or the empty id to hide the fields. The fields are
    // built again only when what they show changed, so a report arriving
    // four times a second does not take a field away from under the cursor.
    void showLoop(const std::string &id);

    // "1, 4" / "1 4" -> {1, 4}. Nothing when a piece is not an integer.
    static std::optional<std::vector<std::int64_t>> parseIntegers(const QString &text);
    static QString integersText(const std::vector<std::int64_t> &values);
    // LP-4e: "metal, glass" / "metal glass" -> {"metal", "glass"}.
    static std::vector<std::string> parseNames(const QString &text);
    static QString namesText(const std::vector<std::string> &names);

  private:
    void rebuild(const GraphCanvasLoop &loop);
    void clearRows();
    void after(bool submitted);
    void commitCount();
    void commitWhere();
    void commitLimit();
    void commitCarry();
    void commitOutputs();
    void commitBranchWhen(std::size_t branch);
    // LP-4e: a draw_loop's `where` tags, and one branch's material
    // condition read off its row.
    void commitWhereTags();
    void commitDrawWhen(std::size_t branch);
    void addDrawBranchRow(QGridLayout &grid, const GraphCanvasLoop &loop, std::size_t index,
                          int row, bool editable);

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::function<void(const std::string &)> open_body_;
    QPushButton *open_ = nullptr;
    std::string loop_;
    std::string shown_;
    std::optional<GraphCanvasLoop> current_;

    QLabel *summary_ = nullptr;
    QSpinBox *count_ = nullptr;
    QToolButton *count_expression_toggle_ = nullptr;
    QLineEdit *count_expression_ = nullptr;
    QComboBox *order_ = nullptr;
    QCheckBox *limit_enabled_ = nullptr;
    QSpinBox *limit_ = nullptr;
    QComboBox *tier_ = nullptr;
    QComboBox *place_ = nullptr;
    QComboBox *where_kind_ = nullptr;
    QLineEdit *where_values_ = nullptr;
    // LP-4e: the rows only a draw_loop has, and the form they share with the
    // rows it does not take (count, order, limit, place, element where).
    QFormLayout *form_ = nullptr;
    QLayout *count_row_ = nullptr;
    QLayout *limit_row_ = nullptr;
    QLayout *where_row_ = nullptr;
    QLabel *contract_ = nullptr;
    QWidget *tags_row_ = nullptr;
    QLineEdit *tags_include_ = nullptr;
    QLineEdit *tags_exclude_ = nullptr;
    QLineEdit *tags_require_all_ = nullptr;
    QWidget *rows_ = nullptr;
    QVBoxLayout *rows_layout_ = nullptr;
    // The rows of the carry list, the outputs and the branch table, rebuilt
    // with the loop.
    struct CarryRow {
        QComboBox *from = nullptr;
        QComboBox *to = nullptr;
    };
    std::vector<CarryRow> carry_rows_;
    std::vector<std::pair<std::string, QCheckBox *>> output_boxes_;
    struct BranchRow {
        QComboBox *when = nullptr;
        QLineEdit *values = nullptr;
        QComboBox *body = nullptr;
        // LP-4e: a draw_loop branch's condition, one field per attribute.
        QComboBox *material = nullptr;
        QLineEdit *tags = nullptr;
        QComboBox *contract = nullptr;
        QComboBox *skinned = nullptr;
        QComboBox *has_variant = nullptr;
    };
    std::vector<BranchRow> branch_rows_;
};

// LP-4e, decisions 2 and 4: the fields of the selected render.material_pass
// (its contract) or draw step (draw.variant's variant; draw.override's render
// state and defines), beside the Graph tab's canvas. Named
// `pelican.graphCanvasDraw.<field>`; each change is one gesture, and
// `submitted` is called after it.
class GraphCanvasDrawFields final : public QWidget {
  public:
    GraphCanvasDrawFields(GraphCanvasModel &model, std::function<void()> submitted,
                          QWidget *parent = nullptr);

    // The node to show, or null to hide the fields. LP-4d1: `scope` is where
    // the node stands -- a draw step in a zone's body is edited in its
    // definition while the canvas shows the root.
    void showNode(const GraphCanvasNode *node);
    void showNode(const GraphCanvasNode *node, const std::string &scope);

    // "A=1\nB" -> {"A=1", "B"}: one define a line, blank lines dropped.
    static std::vector<std::string> parseDefines(const QString &text);

  private:
    void after(bool submitted);
    void commitOverride();

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string node_;
    std::string op_;
    std::string scope_;
    QLabel *heading_ = nullptr;
    QComboBox *contract_ = nullptr;
    QComboBox *variant_ = nullptr;
    QComboBox *cull_ = nullptr;
    QComboBox *depth_test_ = nullptr;
    QComboBox *depth_write_ = nullptr;
    QComboBox *blend_ = nullptr;
    QPlainTextEdit *defines_ = nullptr;
    QPushButton *apply_defines_ = nullptr;
    QWidget *contract_row_ = nullptr;
    QWidget *variant_row_ = nullptr;
    QWidget *override_rows_ = nullptr;
};

} // namespace PelicanStudio
