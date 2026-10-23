#pragma once

#include "../model/graphcanvasmodel.hpp"
// UX-3, decision 5: GraphCanvasTagPicker (its own file since UX-11).
#include "graphcanvastagpicker.hpp"

#include <QToolButton>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
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

class ViewSelectField;

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

    // Q-17: the fields take an edit only while the model can send one. From
    // the moment an edit (or a read of the document) is in flight until the
    // document is read back, every field is switched off -- a choice made in
    // between would be refused by the model and lost without a word -- and
    // switched on again once. Called by the owner whenever the model moved.
    void followEditable();
    // A refused draft changes the notice without rebuilding its live fields.
    std::function<void()> refused;

    // "1, 4" / "1 4" -> {1, 4}. Nothing when a piece is not an integer.
    static std::optional<std::vector<std::int64_t>> parseIntegers(const QString &text);
    static QString integersText(const std::vector<std::int64_t> &values);
    // LP-4e: "metal, glass" / "metal glass" -> {"metal", "glass"}.
    static std::vector<std::string> parseNames(const QString &text);
    static QString namesText(const std::vector<std::string> &names);
    // LP-4f1: a list of elements is offered as checks, one per element,
    // while the loop's count is known and at most this; past it (or for an
    // expression) the line of integers stays.
    static constexpr std::int64_t CheckLimit = 64;
    // How an element is named on its check: "2", or "2 (+Y)" for a view
    // axis the engine labels.
    static QString elementText(const GraphCanvasLoop &loop, std::int64_t element);

  private:
    void rebuild(const GraphCanvasLoop &loop, bool preserve_count);
    void clearRows();
    void after(bool submitted);
    void commitCount();
    // LP-4f1, decision 1: what the loop runs over (Count / View axis), and
    // the family and axis of a view axis.
    void commitOver();
    void commitViewAxis(bool family_changed);
    void fillViewAxes(const std::string &family, const std::string &axis);
    // Decision 4: mip_levels_of("<target>") / layers_of("<target>") into
    // the count expression.
    void insertCountFunction(const QString &function);
    void rebuildWhereChecks(const GraphCanvasLoop &loop, bool editable);
    void commitWhere();
    void commitLimit();
    void commitCarry();
    void commitOutputs();
    void commitBranchWhen(std::size_t branch);
    // LP-4e: a draw_loop's `where` tags, and one branch's material
    // condition read off its row.
    void commitWhereTags();
    void commitDrawWhen(std::size_t branch);
    // Q-17: a branch's choices the model refused go back to what the
    // document says, so the row never shows a choice that was not sent.
    void restoreBranch(std::size_t branch);
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
    // Q-17: everything that edits the loop -- the form and the rows -- sits
    // in here, switched off while the model cannot take an edit.
    QWidget *fields_ = nullptr;
    QSpinBox *count_ = nullptr;
    QToolButton *count_expression_toggle_ = nullptr;
    QLineEdit *count_expression_ = nullptr;
    // LP-4f1.
    QComboBox *over_ = nullptr;
    QWidget *count_insert_row_ = nullptr;
    QComboBox *count_target_ = nullptr;
    QPushButton *insert_mips_ = nullptr;
    QPushButton *insert_layers_ = nullptr;
    // UX-8, decision 12 (a) (RS-15): log2(<the target's size>), for a
    // fixed-size image of the graph.
    QPushButton *insert_log2_ = nullptr;
    [[nodiscard]] std::optional<long long> countTargetSize() const;
    void insertLog2();
    QWidget *view_row_ = nullptr;
    QComboBox *view_family_ = nullptr;
    QComboBox *view_axis_ = nullptr;
    QLabel *view_count_ = nullptr;
    QLabel *view_note_ = nullptr;
    QWidget *where_checks_ = nullptr;
    std::vector<QCheckBox *> where_boxes_;
    QComboBox *order_ = nullptr;
    QCheckBox *limit_enabled_ = nullptr;
    QSpinBox *limit_ = nullptr;
    QComboBox *tier_ = nullptr;
    // PS-1s, decisions 1 and 4: every pass of the body runs once
    // (`pelican.graphCanvasLoop.bodyOnce`), and Recompute (`.recompute`,
    // rerun_once of the loop: all its copies).
    QCheckBox *body_once_ = nullptr;
    QPushButton *recompute_ = nullptr;
    void syncBodyOnce();
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
    // UX-3, decision 5: the same three as picks among the engine's tags,
    // shown in their place while the engine lists any.
    GraphCanvasTagPicker *pick_include_ = nullptr;
    GraphCanvasTagPicker *pick_exclude_ = nullptr;
    GraphCanvasTagPicker *pick_require_all_ = nullptr;
    bool tags_listed_ = false;
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
        // LP-4f1, decision 4: `element =` is a spin, `element in` / `not in`
        // a check per element while the count is known.
        QSpinBox *value = nullptr;
        std::vector<QCheckBox *> checks;
        QComboBox *body = nullptr;
        // LP-4e: a draw_loop branch's condition, one field per attribute.
        QComboBox *material = nullptr;
        QLineEdit *tags = nullptr;
        // UX-3, decision 5: the tags as picks, while the engine lists any.
        GraphCanvasTagPicker *tag_picker = nullptr;
        QComboBox *contract = nullptr;
        QComboBox *skinned = nullptr;
        QComboBox *has_variant = nullptr;
    };
    std::vector<BranchRow> branch_rows_;
};

// LP-4f1, decision 4: which element of another loop's p[] an input reads, per
// element of its own loop -- `element`, `element - 1`, `element + 1`, or,
// behind Custom..., any expression. The combo is named `object_name`, the
// custom line `<object_name>.custom`.
class GraphCanvasElementChoice final : public QWidget {
  public:
    explicit GraphCanvasElementChoice(const QString &object_name, QWidget *parent = nullptr);
    // The expression the choice says; `element` for an empty custom line.
    [[nodiscard]] QString expression() const;
    // A preset when it is one, else Custom... with the text.
    void setExpression(const QString &expression);

  private:
    QComboBox *choice_ = nullptr;
    QLineEdit *custom_ = nullptr;
};

// LP-4f1, decision 2: the View field of the selected render node (a pass, a
// task, a copy), beside the Graph tab's canvas -- LP-4b's ViewSelectField,
// named `pelican.graphCanvasRender.view.<axis>`. In the body of a loop over a
// view axis its axis also offers Each element (loop). Each pick is one
// gesture (set_node_parameters), and `submitted` is called after it.
class GraphCanvasViewFields final : public QWidget {
  public:
    GraphCanvasViewFields(GraphCanvasModel &model, std::function<void()> submitted,
                          QWidget *parent = nullptr);
    // The node to show and the scope it stands in, or null to hide.
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    // Q-18: off while the model cannot take an edit, as the Draw fields.
    void followEditable();

  private:
    // A pick the model refuses goes back to what the document says.
    void commit();
    void load();

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string node_;
    std::string scope_;
    std::string shown_;
    std::optional<GraphCanvasViewField> field_;
    QLabel *heading_ = nullptr;
    ViewSelectField *view_ = nullptr;
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

    // Q-18, as GraphCanvasLoopFields::followEditable(): the fields take an
    // edit only while the model can send one -- off from the moment an edit
    // (or a read of the document) is in flight until the document is read
    // back, on again once after. Called by the owner whenever the model moved.
    void followEditable();

    // "A=1\nB" -> {"A=1", "B"}: one define a line, blank lines dropped.
    static std::vector<std::string> parseDefines(const QString &text);

  private:
    // A choice the model refused goes back to the document's value.
    void after(bool submitted);
    void commitOverride();
    void showParameters(const nlohmann::json &parameters, bool keep_variant, bool keep_defines);
    void restore();

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string node_;
    std::string op_;
    std::string scope_;
    QLabel *heading_ = nullptr;
    QComboBox *contract_ = nullptr;
    // UX-3, decision 5: a material pass's material_variant, from the
    // variants the engine's materials have.
    QComboBox *material_variant_ = nullptr;
    QComboBox *variant_ = nullptr;
    // The line the variant combo types into while it is editable (no list).
    QLineEdit *variant_line_ = nullptr;
    QComboBox *cull_ = nullptr;
    QComboBox *depth_test_ = nullptr;
    QComboBox *depth_write_ = nullptr;
    QComboBox *blend_ = nullptr;
    // UX-3, decision 4.
    QComboBox *depth_compare_ = nullptr;
    QPlainTextEdit *defines_ = nullptr;
    QPushButton *apply_defines_ = nullptr;
    QWidget *contract_row_ = nullptr;
    QWidget *variant_row_ = nullptr;
    QWidget *override_rows_ = nullptr;
};

} // namespace PelicanStudio
