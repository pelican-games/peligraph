#include "graphcanvasloopfields.hpp"

#include "frameplanedit.hpp"
#include "studiotheme.hpp"
#include "viewselectfield.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace PelicanStudio {
namespace {

QString qtext(const std::string &value) { return QString::fromStdString(value); }

// The names a scenario reaches the fields by.
QString fieldName(const QString &field) {
    return QStringLiteral("pelican.graphCanvasLoop.") + field;
}

QString branchFieldName(std::size_t branch, const char *column) {
    return fieldName(QStringLiteral("branch%1.%2").arg(branch).arg(QLatin1String{column}));
}

// The branch table's condition column, in the order the combo lists it. The
// data is the key the document spells the condition with.
struct WhenChoice {
    const char *label;
    const char *key;
};
constexpr WhenChoice WhenChoices[] = {
    {"element =", "element"},
    {"element in", "element_in"},
    {"element not in", "element_not_in"},
};

// What one loop's fields show, as one string: when it changes, the rows are
// built again; when it does not, nothing under the cursor is replaced.
QString signature(const GraphCanvasModel &model, const GraphCanvasLoop &loop,
                  const std::vector<std::string> &choices, bool editable) {
    QStringList parts{qtext(loop.id), QString::number(loop.count),
                      loop.count_expression ? qtext(loop.count_text) : QString{},
                      loop.descending ? QStringLiteral("d") : QStringLiteral("a"),
                      loop.limit ? QString::number(*loop.limit) : QStringLiteral("-"),
                      qtext(loop.tier), qtext(loop.where), qtext(loop.anchor),
                      qtext(loop.insert), loop.body_draws ? QStringLiteral("g") : QString{},
                      GraphCanvasLoopFields::integersText(loop.where_values),
                      qtext(loop.body), QString::number(loop.reported ? loop.reported_count : -1),
                      editable ? QStringLiteral("e") : QStringLiteral("r")};
    for (const auto &branch : loop.branches) {
        parts << qtext(branch.when) << GraphCanvasLoopFields::integersText(branch.values)
              << qtext(branch.body) << (branch.nothing ? QStringLiteral("0") : QStringLiteral("1"))
              << GraphCanvasLoopFields::integersText(branch.matched)
              << (branch.unmatched ? QStringLiteral("u") : QString{});
        // LP-4e: a draw_loop branch's condition and the materials it took.
        const auto &draw = branch.draw_when;
        parts << qtext(draw.material.value_or("\x1e")) << qtext(draw.contract.value_or("\x1e"))
              << (draw.skinned ? (*draw.skinned ? QStringLiteral("s1") : QStringLiteral("s0"))
                               : QString{})
              << (draw.has_variant ? (*draw.has_variant ? QStringLiteral("v1") : QStringLiteral("v0"))
                                   : QString{});
        if (draw.tags)
            parts << GraphCanvasLoopFields::namesText(draw.tags->include)
                  << GraphCanvasLoopFields::namesText(draw.tags->exclude)
                  << GraphCanvasLoopFields::namesText(draw.tags->require_all);
        for (const auto &material : branch.matched_materials) parts << qtext(material);
    }
    parts << (loop.draw_items ? QStringLiteral("draw_items") : QString{}) << qtext(loop.contract)
          << GraphCanvasLoopFields::namesText(loop.where_tags.include)
          << GraphCanvasLoopFields::namesText(loop.where_tags.exclude)
          << GraphCanvasLoopFields::namesText(loop.where_tags.require_all) << qtext(loop.pass)
          << QString::number(loop.reported_total);
    for (const auto &material : loop.materials) parts << qtext(material);
    for (const auto &carry : loop.carry)
        parts << qtext(carry.from) << qtext(carry.to)
              << (carry.to_unbound ? QStringLiteral("!") : QString{});
    for (const auto &output : loop.outputs) parts << qtext(output);
    for (const auto &input : loop.body_inputs) parts << qtext(input);
    for (const auto &output : loop.body_outputs) parts << qtext(output);
    for (const auto &choice : choices) parts << qtext(choice);
    // UX-3, decision 5: the engine's materials, tags and variants.
    parts << QStringLiteral("\x1ematerials");
    for (const auto &material : model.materials())
        parts << qtext(material.id) << qtext(material.name)
              << GraphCanvasLoopFields::namesText(material.variants);
    parts << GraphCanvasLoopFields::namesText(model.materialTags());
    for (const auto &branch : loop.branches)
        parts << qtext(branch.draw_when.has_variant_named.value_or("\x1e"));
    // LP-4f1: what the loop runs over, and what the frame plan offers.
    parts << qtext(loop.view_family) << qtext(loop.view_axis)
          << (loop.known_count ? QString::number(*loop.known_count) : QStringLiteral("?"));
    return parts.join(QChar{0x1f});
}

QString viewSignature(const GraphCanvasModel &model) {
    QStringList parts;
    for (const auto &family : model.viewFamilies()) {
        parts << qtext(family.family);
        for (const auto &axis : family.axes) {
            parts << qtext(axis.axis)
                  << (axis.length ? QString::number(*axis.length) : QStringLiteral("-"))
                  << (axis.runtime ? QStringLiteral("r") : QString{});
            for (const auto &label : axis.labels) parts << qtext(label);
        }
    }
    parts << QStringLiteral("\x1etargets");
    for (const auto &target : model.planTargets()) parts << qtext(target);
    return parts.join(QChar{0x1f});
}

bool selectableAxis(const FramePlanViewAxis &axis) {
    return !axis.runtime && axis.length.has_value();
}

} // namespace

// --- UX-3, decision 5: the engine's material tags, picked.

GraphCanvasTagPicker::GraphCanvasTagPicker(const QString &object_name, const QString &empty_text,
                                           QWidget *parent)
    : QToolButton{parent}, empty_text_{empty_text} {
    setObjectName(object_name);
    setPopupMode(QToolButton::InstantPopup);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *list = new QMenu(this);
    list->setObjectName(object_name + QStringLiteral(".menu"));
    setMenu(list);
    showSelection();
}

void GraphCanvasTagPicker::setTags(const std::vector<std::string> &all,
                                   const std::vector<std::string> &selected) {
    menu()->clear();
    checks_.clear();
    auto tags = all;
    // What the document names stays visible, even a tag no material has.
    for (const auto &tag : selected)
        if (std::ranges::find(tags, tag) == tags.end()) tags.push_back(tag);
    for (const auto &tag : tags) {
        auto *check = menu()->addAction(qtext(tag));
        check->setObjectName(objectName() + QLatin1Char{'.'} + qtext(tag));
        check->setCheckable(true);
        check->setChecked(std::ranges::find(selected, tag) != selected.end());
        connect(check, &QAction::triggered, this, [this] {
            showSelection();
            if (changed) changed();
        });
        checks_.emplace_back(tag, check);
    }
    showSelection();
}

std::vector<std::string> GraphCanvasTagPicker::selected() const {
    std::vector<std::string> tags;
    for (const auto &[tag, check] : checks_)
        if (check->isChecked()) tags.push_back(tag);
    return tags;
}

void GraphCanvasTagPicker::showSelection() {
    QStringList names;
    for (const auto &tag : selected()) names << qtext(tag);
    setText(names.isEmpty() ? empty_text_ : names.join(QStringLiteral(", ")));
}

QString GraphCanvasLoopFields::elementText(const GraphCanvasLoop &loop, std::int64_t element) {
    if (loop.view_axis_info && element >= 0)
        return qtext(framePlanViewValueText({*loop.view_axis_info}, loop.view_axis,
                                            static_cast<std::uint64_t>(element)));
    return QString::number(element);
}

std::optional<std::vector<std::int64_t>> GraphCanvasLoopFields::parseIntegers(const QString &text) {
    std::vector<std::int64_t> values;
    const auto pieces = text.split(QRegularExpression{QStringLiteral("[,\\s]+")},
                                   Qt::SkipEmptyParts);
    for (const auto &piece : pieces) {
        bool ok = false;
        const auto value = piece.toLongLong(&ok);
        if (!ok) return std::nullopt;
        values.push_back(value);
    }
    return values;
}

QString GraphCanvasLoopFields::integersText(const std::vector<std::int64_t> &values) {
    QStringList pieces;
    for (const auto value : values) pieces << QString::number(value);
    return pieces.join(QStringLiteral(", "));
}

std::vector<std::string> GraphCanvasLoopFields::parseNames(const QString &text) {
    std::vector<std::string> names;
    for (const auto &piece :
         text.split(QRegularExpression{QStringLiteral("[,\\s]+")}, Qt::SkipEmptyParts)) {
        auto name = piece.toStdString();
        if (std::ranges::find(names, name) == names.end()) names.push_back(std::move(name));
    }
    return names;
}

QString GraphCanvasLoopFields::namesText(const std::vector<std::string> &names) {
    QStringList pieces;
    for (const auto &name : names) pieces << qtext(name);
    return pieces.join(QStringLiteral(", "));
}

GraphCanvasLoopFields::GraphCanvasLoopFields(
    GraphCanvasModel &model, std::function<void()> submitted,
    std::function<void(const std::string &loop)> open_body, QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)},
      open_body_{std::move(open_body)} {
    setObjectName(QStringLiteral("pelican.graphCanvasLoop"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);
    auto *summary_row = new QHBoxLayout;
    summary_ = new QLabel(this);
    summary_->setObjectName(fieldName(QStringLiteral("summary")));
    summary_->setFont(studioSmallFont());
    summary_->setWordWrap(true);
    summary_row->addWidget(summary_, 1);
    // Decision 5: into the body the chosen branch runs -- what a double-click
    // on the loop does, as a button a person can find.
    open_ = new QPushButton(tr("Open body"), this);
    open_->setObjectName(fieldName(QStringLiteral("openBody")));
    open_->setToolTip(tr("Edit the body of the branch chosen for the loop"));
    summary_row->addWidget(open_);
    layout->addLayout(summary_row);
    connect(open_, &QPushButton::clicked, this, [this] {
        if (!loop_.empty() && open_body_) open_body_(loop_);
    });

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    const auto caption = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setFont(studioSmallFont());
        return label;
    };

    // LP-4f1, decision 1: what the loop runs over -- 0 up to a count, or the
    // views along one axis of a view family.
    over_ = new QComboBox(this);
    over_->setObjectName(fieldName(QStringLiteral("over")));
    over_->addItem(tr("Count"), QStringLiteral("range"));
    over_->addItem(tr("View axis"), QStringLiteral("view_axis"));
    // GL-1d, decision 2: or over the materials of the pass its draws feed.
    over_->addItem(tr("Draw items"), QStringLiteral("draw_items"));
    over_->setToolTip(tr("Run over 0 up to a count, over the views along one axis of a view "
                         "family (a cube's faces, a shadow's cascades), or over the materials "
                         "of the material pass that reads the loop's draws."));
    form->addRow(caption(tr("Over")), over_);
    connect(over_, &QComboBox::currentIndexChanged, this, [this] { commitOver(); });

    // How many elements: an integer, or -- behind the f button -- an
    // expression over the constants known when the pipeline is composed.
    auto *count_row = new QHBoxLayout;
    count_ = new QSpinBox(this);
    count_->setObjectName(fieldName(QStringLiteral("count")));
    count_->setRange(0, 4096);
    count_->setKeyboardTracking(false);
    count_->setToolTip(tr("How many elements the loop runs over (0 up to its count)."));
    count_row->addWidget(count_, 1);
    count_expression_ = new QLineEdit(this);
    count_expression_->setObjectName(fieldName(QStringLiteral("countExpression")));
    count_expression_->setPlaceholderText(tr("mip_levels_of(\"target\") - 1"));
    count_expression_->setToolTip(
        tr("An expression for the count, evaluated when the pipeline is composed."));
    count_expression_->hide();
    count_row->addWidget(count_expression_, 1);
    count_expression_toggle_ = new QToolButton(this);
    count_expression_toggle_->setObjectName(fieldName(QStringLiteral("countExpressionToggle")));
    count_expression_toggle_->setText(QStringLiteral("ƒ"));
    count_expression_toggle_->setCheckable(true);
    count_expression_toggle_->setToolTip(tr("Write the count as an expression"));
    count_row->addWidget(count_expression_toggle_);
    form->addRow(caption(tr("Count")), count_row);
    count_row_ = count_row;
    // LP-4f1, decision 4: a target's mip levels or layers into the
    // expression, the target chosen from the frame plan's.
    count_insert_row_ = new QWidget(this);
    auto *insert_layout = new QHBoxLayout(count_insert_row_);
    insert_layout->setContentsMargins(0, 0, 0, 0);
    count_target_ = new QComboBox(count_insert_row_);
    count_target_->setObjectName(fieldName(QStringLiteral("countTarget")));
    count_target_->setToolTip(tr("A render target of the frame plan"));
    insert_layout->addWidget(count_target_, 1);
    insert_mips_ = new QPushButton(QStringLiteral("mip_levels_of"), count_insert_row_);
    insert_mips_->setObjectName(fieldName(QStringLiteral("insertMipLevels")));
    insert_mips_->setToolTip(tr("Put the target's mip level count into the expression"));
    insert_layout->addWidget(insert_mips_);
    insert_layers_ = new QPushButton(QStringLiteral("layers_of"), count_insert_row_);
    insert_layers_->setObjectName(fieldName(QStringLiteral("insertLayers")));
    insert_layers_->setToolTip(tr("Put the target's layer count into the expression"));
    insert_layout->addWidget(insert_layers_);
    form->addRow(QString{}, count_insert_row_);
    connect(insert_mips_, &QPushButton::clicked, this,
            [this] { insertCountFunction(QStringLiteral("mip_levels_of")); });
    connect(insert_layers_, &QPushButton::clicked, this,
            [this] { insertCountFunction(QStringLiteral("layers_of")); });
    connect(count_, &QSpinBox::editingFinished, this, [this] { commitCount(); });
    connect(count_expression_, &QLineEdit::returnPressed, this, [this] { commitCount(); });
    connect(count_expression_toggle_, &QToolButton::toggled, this, [this](bool expression) {
        count_->setVisible(!expression);
        count_expression_->setVisible(expression);
        if (form_ != nullptr) form_->setRowVisible(count_insert_row_, expression);
        if (expression && count_expression_->text().isEmpty())
            count_expression_->setText(QString::number(count_->value()));
    });

    // LP-4f1, decision 1: the family and the axis a view-axis loop runs
    // over, and its count, which is the axis's length (read only).
    view_row_ = new QWidget(this);
    auto *view_layout = new QHBoxLayout(view_row_);
    view_layout->setContentsMargins(0, 0, 0, 0);
    view_family_ = new QComboBox(view_row_);
    view_family_->setObjectName(fieldName(QStringLiteral("viewFamily")));
    view_family_->setToolTip(tr("The view family whose views the loop runs over"));
    view_layout->addWidget(view_family_, 1);
    view_axis_ = new QComboBox(view_row_);
    view_axis_->setObjectName(fieldName(QStringLiteral("viewAxis")));
    view_axis_->setToolTip(tr("The axis of the family: one element per view along it"));
    view_layout->addWidget(view_axis_, 1);
    view_count_ = new QLabel(view_row_);
    view_count_->setObjectName(fieldName(QStringLiteral("viewCount")));
    view_count_->setFont(studioSmallFont());
    view_count_->setToolTip(tr("How many elements: the axis's length, known when the pipeline "
                               "is composed"));
    view_layout->addWidget(view_count_);
    form->addRow(caption(tr("View")), view_row_);
    view_note_ = new QLabel(this);
    view_note_->setObjectName(fieldName(QStringLiteral("viewAxisNote")));
    view_note_->setFont(studioSmallFont());
    view_note_->setWordWrap(true);
    form->addRow(QString{}, view_note_);
    connect(view_family_, &QComboBox::currentIndexChanged, this,
            [this] { commitViewAxis(true); });
    connect(view_axis_, &QComboBox::currentIndexChanged, this,
            [this] { commitViewAxis(false); });

    order_ = new QComboBox(this);
    order_->setObjectName(fieldName(QStringLiteral("order")));
    order_->addItem(tr("ascending"), false);
    order_->addItem(tr("descending"), true);
    form->addRow(caption(tr("Order")), order_);
    connect(order_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || loop_.empty()) return;
        after(model_.setLoopDescending(loop_, order_->itemData(index).toBool()));
    });

    auto *limit_row = new QHBoxLayout;
    limit_enabled_ = new QCheckBox(tr("first"), this);
    limit_enabled_->setObjectName(fieldName(QStringLiteral("limitEnabled")));
    limit_enabled_->setToolTip(tr("Keep only the first N elements, after the order"));
    limit_row->addWidget(limit_enabled_);
    limit_ = new QSpinBox(this);
    limit_->setObjectName(fieldName(QStringLiteral("limit")));
    limit_->setRange(0, 4096);
    limit_->setKeyboardTracking(false);
    limit_row->addWidget(limit_, 1);
    form->addRow(caption(tr("Limit")), limit_row);
    limit_row_ = limit_row;
    connect(limit_enabled_, &QCheckBox::toggled, this, [this] { commitLimit(); });
    connect(limit_, &QSpinBox::editingFinished, this, [this] { commitLimit(); });

    tier_ = new QComboBox(this);
    tier_->setObjectName(fieldName(QStringLiteral("tier")));
    tier_->addItem(tr("automatic"), QString{});
    tier_->addItem(QStringLiteral("expand"), QStringLiteral("expand"));
    tier_->addItem(QStringLiteral("draw_loop"), QStringLiteral("draw_loop"));
    tier_->setToolTip(tr("Fix the stage the loop runs in; automatic lets the engine choose."));
    form->addRow(caption(tr("Stage")), tier_);
    connect(tier_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || loop_.empty()) return;
        after(model_.setLoopTier(loop_, tier_->itemData(index).toString().toStdString()));
    });

    // Where the copies land among the passes (LP-0b): a canonical anchor, or
    // the end of the pass family. Only a body that draws takes a place.
    place_ = new QComboBox(this);
    place_->setObjectName(fieldName(QStringLiteral("place")));
    place_->addItem(tr("end of its pass family"), QString{});
    for (const auto &anchor : GraphCanvasModel::loopAnchors())
        place_->addItem(tr("after %1").arg(qtext(anchor)), qtext(anchor));
    form->addRow(caption(tr("Place")), place_);
    connect(place_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || loop_.empty()) return;
        const auto data = place_->itemData(index).toString();
        if (data == QLatin1String{"\x1finsert"}) return;
        after(model_.setLoopAnchor(loop_, data.toStdString()));
    });

    auto *where_row = new QHBoxLayout;
    where_kind_ = new QComboBox(this);
    where_kind_->setObjectName(fieldName(QStringLiteral("whereKind")));
    where_kind_->addItem(tr("every element"), QString{});
    where_kind_->addItem(tr("element in"), QStringLiteral("element_in"));
    where_kind_->addItem(tr("element not in"), QStringLiteral("element_not_in"));
    where_row->addWidget(where_kind_);
    where_values_ = new QLineEdit(this);
    where_values_->setObjectName(fieldName(QStringLiteral("whereValues")));
    where_values_->setPlaceholderText(tr("1, 4"));
    where_row->addWidget(where_values_, 1);
    // LP-4f1, decision 4: one check per element while the count is known.
    where_checks_ = new QWidget(this);
    where_checks_->setObjectName(fieldName(QStringLiteral("whereChecks")));
    auto *checks_layout = new QGridLayout(where_checks_);
    checks_layout->setContentsMargins(0, 0, 0, 0);
    checks_layout->setHorizontalSpacing(6);
    checks_layout->setVerticalSpacing(1);
    where_row->addWidget(where_checks_, 1);
    form->addRow(caption(tr("Where")), where_row);
    where_row_ = where_row;
    connect(where_kind_, &QComboBox::currentIndexChanged, this, [this] { commitWhere(); });
    connect(where_values_, &QLineEdit::returnPressed, this, [this] { commitWhere(); });

    // LP-4e, decision 3: a draw_loop runs over the materials of one pass. Its
    // contract is the pass's (read only here: it is decided by the pass the
    // draws feed); its `where` tests the materials' tags.
    contract_ = new QLabel(this);
    contract_->setObjectName(fieldName(QStringLiteral("contract")));
    contract_->setFont(studioSmallFont());
    contract_->setToolTip(tr("The material contract of the pass this loop draws in."));
    form->addRow(caption(tr("Contract")), contract_);
    tags_row_ = new QWidget(this);
    auto *tags_layout = new QHBoxLayout(tags_row_);
    tags_layout->setContentsMargins(0, 0, 0, 0);
    const auto tags_field = [this, tags_layout](const char *name, const QString &placeholder,
                                                const QString &tip) {
        auto *field = new QLineEdit(tags_row_);
        field->setObjectName(fieldName(QString::fromLatin1(name)));
        field->setPlaceholderText(placeholder);
        field->setToolTip(tip);
        tags_layout->addWidget(field, 1);
        connect(field, &QLineEdit::returnPressed, this, [this] { commitWhereTags(); });
        return field;
    };
    tags_include_ = tags_field("whereTagsInclude", tr("any of"),
                               tr("Materials with any of these tags (commas between); empty: every material"));
    tags_exclude_ = tags_field("whereTagsExclude", tr("none of"),
                               tr("Leave out materials with any of these tags"));
    tags_require_all_ = tags_field("whereTagsRequireAll", tr("all of"),
                                   tr("Only materials with every one of these tags"));
    // UX-3, decision 5: the same three, picked among the engine's tags.
    const auto tags_picker = [this, tags_layout](const char *name, const QString &empty,
                                                 const QString &tip) {
        auto *picker =
            new GraphCanvasTagPicker(fieldName(QString::fromLatin1(name)), empty, tags_row_);
        picker->setToolTip(tip);
        picker->changed = [this] { commitWhereTags(); };
        picker->hide();
        tags_layout->addWidget(picker, 1);
        return picker;
    };
    pick_include_ = tags_picker("whereTagsIncludePick", tr("any of: (every material)"),
                                tr("Materials with any of these tags; none checked: every material"));
    pick_exclude_ = tags_picker("whereTagsExcludePick", tr("none of: -"),
                                tr("Leave out materials with any of these tags"));
    pick_require_all_ = tags_picker("whereTagsRequireAllPick", tr("all of: -"),
                                    tr("Only materials with every one of these tags"));
    form->addRow(caption(tr("Tags")), tags_row_);
    form_ = form;
    // Q-17: the form and the rows in one widget, switched off together while
    // the model cannot take an edit. The form's fields move into it with the
    // layout.
    fields_ = new QWidget(this);
    fields_->setObjectName(fieldName(QStringLiteral("fields")));
    auto *fields_layout = new QVBoxLayout(fields_);
    fields_layout->setContentsMargins(0, 0, 0, 0);
    fields_layout->addLayout(form);
    layout->addWidget(fields_);

    rows_ = new QWidget(fields_);
    rows_layout_ = new QVBoxLayout(rows_);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    fields_layout->addWidget(rows_);
    hide();
}

void GraphCanvasLoopFields::after(bool submitted) {
    if (submitted && submitted_) submitted_();
}

void GraphCanvasLoopFields::showLoop(const std::string &id) {
    const auto loop = id.empty() ? std::nullopt : model_.loop(id);
    if (!loop) {
        loop_.clear();
        shown_.clear();
        current_.reset();
        hide();
        return;
    }
    const auto choices = model_.loopBodyChoices(id);
    // UX-3, decision 5: a draw_loop's tags and materials come with the
    // authoring context.
    if (loop->draw_items) model_.askRenderTargetVocabulary();
    const auto shown = (signature(model_, *loop, choices, model_.canEdit()) + QChar{0x1d} +
                        viewSignature(model_))
                           .toStdString();
    loop_ = id;
    current_ = loop;
    if (shown != shown_) {
        shown_ = shown;
        rebuild(*loop);
    }
    followEditable();
    show();
}

void GraphCanvasLoopFields::followEditable() {
    // Q-17. The rows are built for a model that could edit when they were
    // built; an edit in flight since then does not rebuild them (nothing they
    // show has changed yet), so they said "go ahead" while the model could
    // only refuse. A switched-off parent keeps each field's own state: a
    // field off for its own reason stays off when this comes back on.
    fields_->setEnabled(model_.canEdit());
}

void GraphCanvasLoopFields::clearRows() {
    carry_rows_.clear();
    output_boxes_.clear();
    branch_rows_.clear();
    // The rows may be what is emitting right now (a Remove press that sends
    // an edit, which makes the fields read-only until it lands): they go
    // when the event loop comes back, unnamed and out of sight until then,
    // so nothing finds the old row by its name.
    for (QWidget *child : rows_->findChildren<QWidget *>()) child->setObjectName(QString{});
    rows_->hide();
    rows_->deleteLater();
    rows_ = new QWidget(fields_);
    rows_layout_ = new QVBoxLayout(rows_);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    static_cast<QVBoxLayout *>(fields_->layout())->addWidget(rows_);
}

void GraphCanvasLoopFields::rebuild(const GraphCanvasLoop &loop) {
    const bool editable = model_.canEdit();
    // The summary: what the report says the loop became.
    QString summary = tr("loop %1").arg(qtext(loop.id));
    if (loop.reported)
        summary += tr(" · %1 ×%2").arg(qtext(loop.reported_tier)).arg(loop.reported_count);
    else
        summary += tr(" · not reported yet");
    summary += loop.body.empty() ? tr(" · no body") : tr(" · body %1").arg(qtext(loop.body));
    summary_->setText(summary);
    open_->setEnabled(model_.canEnterInstance(loop.id));

    {
        const QSignalBlocker block_count{count_};
        const QSignalBlocker block_toggle{count_expression_toggle_};
        count_->setValue(static_cast<int>(std::clamp<std::int64_t>(loop.count, 0, 4096)));
        count_expression_->setText(loop.count_expression ? qtext(loop.count_text) : QString{});
        count_expression_toggle_->setChecked(loop.count_expression);
        count_->setVisible(!loop.count_expression);
        count_expression_->setVisible(loop.count_expression);
    }
    {
        const QSignalBlocker block{order_};
        order_->setCurrentIndex(loop.descending ? 1 : 0);
    }
    {
        const QSignalBlocker block_enabled{limit_enabled_};
        const QSignalBlocker block_limit{limit_};
        limit_enabled_->setChecked(loop.limit.has_value());
        limit_->setValue(static_cast<int>(std::clamp<std::int64_t>(loop.limit.value_or(1), 0, 4096)));
        limit_->setEnabled(editable && loop.limit.has_value());
    }
    {
        const QSignalBlocker block{tier_};
        const int index = tier_->findData(qtext(loop.tier));
        tier_->setCurrentIndex(index < 0 ? 0 : index);
    }
    {
        const QSignalBlocker block{place_};
        // An insert naming a pass is shown as it is, and stays until another
        // place is picked.
        if (const int shown = place_->findData(QStringLiteral("\x1finsert")); shown >= 0)
            place_->removeItem(shown);
        if (!loop.insert.empty())
            place_->addItem(tr("insert %1").arg(qtext(loop.insert)), QStringLiteral("\x1finsert"));
        const int index = place_->findData(loop.insert.empty() ? qtext(loop.anchor)
                                                               : QStringLiteral("\x1finsert"));
        place_->setCurrentIndex(index < 0 ? 0 : index);
        place_->setEnabled(editable && loop.body_draws);
        place_->setToolTip(loop.body_draws
                               ? tr("Where the loop's copies run among the passes")
                               : tr("Only a body with a fullscreen or raster pass takes a place."));
    }
    {
        const QSignalBlocker block_kind{where_kind_};
        const int index = where_kind_->findData(qtext(loop.where));
        where_kind_->setCurrentIndex(index < 0 ? 0 : index);
        where_values_->setText(integersText(loop.where_values));
        where_values_->setEnabled(editable && !loop.where.empty());
    }
    // LP-4f1: over a count or a view axis.
    const bool view = !loop.view_family.empty();
    {
        const QSignalBlocker block{over_};
        over_->setCurrentIndex(view ? 1 : 0);
        // GL-1d, decision 2: draw items only for a loop a material pass
        // reads, whose contract they are.
        const int draw_index = over_->findData(QStringLiteral("draw_items"));
        const auto contract = model_.loopDrawItemsContract(loop.id);
        if (auto *items = qobject_cast<QStandardItemModel *>(over_->model());
            items != nullptr && draw_index >= 0)
            items->item(draw_index)->setEnabled(!contract.empty());
        over_->setItemData(
            draw_index,
            contract.empty()
                ? tr("No material pass reads this loop's draws: connect its draws "
                     "to a material pass first.")
                : tr("Run over the materials of the %1 pass that reads the draws.")
                      .arg(qtext(contract)),
            Qt::ToolTipRole);
    }
    {
        const QSignalBlocker block{count_target_};
        const auto kept = count_target_->currentText();
        count_target_->clear();
        for (const auto &target : model_.planTargets()) count_target_->addItem(qtext(target));
        count_target_->setCurrentIndex(std::max(0, count_target_->findText(kept)));
    }
    insert_mips_->setEnabled(editable && count_target_->count() > 0);
    insert_layers_->setEnabled(editable && count_target_->count() > 0);
    count_target_->setEnabled(editable);
    if (view) {
        {
            const QSignalBlocker block{view_family_};
            view_family_->clear();
            for (const auto &family : model_.viewFamilies())
                view_family_->addItem(qtext(family.family), qtext(family.family));
            // A family the frame plan does not list stays, as written.
            if (view_family_->findData(qtext(loop.view_family)) < 0)
                view_family_->addItem(tr("%1 (not in the frame plan)").arg(qtext(loop.view_family)),
                                      qtext(loop.view_family));
            view_family_->setCurrentIndex(view_family_->findData(qtext(loop.view_family)));
        }
        fillViewAxes(loop.view_family, loop.view_axis);
        view_count_->setText(loop.known_count
                                 ? tr("×%1 (%2)").arg(*loop.known_count).arg(qtext(loop.view_axis))
                                 : tr("×? (%1)").arg(qtext(loop.view_axis)));
    }
    view_note_->setText(model_.viewFamilies().empty()
                            ? tr("No pass of the frame plan runs in a view family with axes.")
                            : tr("Families no pass of the frame plan runs in are not listed."));
    for (QWidget *field : std::initializer_list<QWidget *>{
             count_, count_expression_, count_expression_toggle_, order_, limit_enabled_,
             tier_, where_kind_, over_, view_family_, view_axis_})
        field->setEnabled(editable);
    // LP-4e: a draw_loop takes no count, order, limit, place or element
    // `where` (the engine refuses a limit or a carry there, and its order is
    // the phase's); it has a contract and a tag `where` instead.
    const bool draw = loop.draw_items;
    rebuildWhereChecks(loop, editable);
    form_->setRowVisible(over_, !draw);
    form_->setRowVisible(count_row_, !draw && !view);
    form_->setRowVisible(count_insert_row_, !draw && !view && loop.count_expression);
    form_->setRowVisible(view_row_, !draw && view);
    form_->setRowVisible(view_note_, !draw && view);
    form_->setRowVisible(order_, !draw);
    form_->setRowVisible(limit_row_, !draw);
    form_->setRowVisible(place_, !draw);
    form_->setRowVisible(where_row_, !draw);
    form_->setRowVisible(contract_, draw);
    form_->setRowVisible(tags_row_, draw);
    // A row shown again shows every widget in it: the one of each pair the
    // loop does not use goes again.
    count_->setVisible(!draw && !view && !loop.count_expression);
    count_expression_->setVisible(!draw && !view && loop.count_expression);
    const bool checks = !where_boxes_.empty();
    where_values_->setVisible(!draw && !checks);
    where_checks_->setVisible(!draw && checks);
    if (draw) {
        contract_->setText(loop.pass.empty()
                               ? qtext(loop.contract)
                               : tr("%1 (the pass %2)").arg(qtext(loop.contract), qtext(loop.pass)));
        tags_include_->setText(namesText(loop.where_tags.include));
        tags_exclude_->setText(namesText(loop.where_tags.exclude));
        tags_require_all_->setText(namesText(loop.where_tags.require_all));
        // UX-3, decision 5: picked among the engine's tags while it lists
        // any; typed, as before, while it does not.
        const auto tags = model_.materialTags();
        tags_listed_ = !tags.empty();
        pick_include_->setTags(tags, loop.where_tags.include);
        pick_exclude_->setTags(tags, loop.where_tags.exclude);
        pick_require_all_->setTags(tags, loop.where_tags.require_all);
        for (QLineEdit *field : {tags_include_, tags_exclude_, tags_require_all_}) {
            field->setEnabled(editable);
            field->setVisible(!tags_listed_);
        }
        for (GraphCanvasTagPicker *picker : {pick_include_, pick_exclude_, pick_require_all_}) {
            picker->setEnabled(editable);
            picker->setVisible(tags_listed_);
        }
    }

    clearRows();
    const auto heading = [this](const QString &text) {
        auto *label = new QLabel(text, rows_);
        QFont font = studioSmallFont();
        font.setBold(true);
        label->setFont(font);
        rows_layout_->addWidget(label);
    };

    // Outputs: p (the last element's) and p[] (every element's), for each
    // output of the body. LP-4e: a draw_loop's one output is its draws.
    if (!loop.body_outputs.empty() && !draw) {
        heading(tr("Outputs"));
        auto *grid = new QGridLayout;
        int row = 0;
        for (const auto &output : loop.body_outputs) {
            int column = 0;
            for (const auto &port : {output, output + "[]"}) {
                auto *box = new QCheckBox(qtext(port), rows_);
                box->setObjectName(fieldName(QStringLiteral("output.") + qtext(port)));
                box->setChecked(std::ranges::find(loop.outputs, port) != loop.outputs.end());
                box->setEnabled(editable);
                box->setToolTip(port.ends_with("[]")
                                    ? tr("One value per element, for another loop to read one of")
                                    : tr("The last element's value"));
                connect(box, &QCheckBox::toggled, this, [this] { commitOutputs(); });
                grid->addWidget(box, row, column++);
                output_boxes_.emplace_back(port, box);
            }
            ++row;
        }
        rows_layout_->addLayout(grid);
    }

    // Carry: an output of one element handed to an input of the next. Not in
    // a draw_loop: no material hands a value to the next.
    if (!loop.body_outputs.empty() && !loop.body_inputs.empty() && !draw) {
        heading(tr("Carry"));
        for (std::size_t index = 0; index < loop.carry.size(); ++index) {
            const auto &carry = loop.carry[index];
            auto *line = new QHBoxLayout;
            auto *from = new QComboBox(rows_);
            from->setObjectName(fieldName(QStringLiteral("carry%1.from").arg(index)));
            for (const auto &output : loop.body_outputs) from->addItem(qtext(output));
            from->setCurrentIndex(from->findText(qtext(carry.from)));
            auto *arrow = new QLabel(QStringLiteral("→"), rows_);
            auto *to = new QComboBox(rows_);
            to->setObjectName(fieldName(QStringLiteral("carry%1.to").arg(index)));
            for (const auto &input : loop.body_inputs) to->addItem(qtext(input));
            to->setCurrentIndex(to->findText(qtext(carry.to)));
            auto *remove = new QPushButton(tr("Remove"), rows_);
            remove->setObjectName(fieldName(QStringLiteral("carry%1.remove").arg(index)));
            for (QWidget *field : std::initializer_list<QWidget *>{from, to, remove})
                field->setEnabled(editable);
            line->addWidget(from, 1);
            line->addWidget(arrow);
            line->addWidget(to, 1);
            line->addWidget(remove);
            rows_layout_->addLayout(line);
            carry_rows_.push_back(CarryRow{from, to});
            connect(from, &QComboBox::currentIndexChanged, this, [this] { commitCarry(); });
            connect(to, &QComboBox::currentIndexChanged, this, [this] { commitCarry(); });
            connect(remove, &QPushButton::clicked, this, [this, index] {
                if (!current_) return;
                std::vector<std::pair<std::string, std::string>> pairs;
                for (std::size_t other = 0; other < current_->carry.size(); ++other)
                    if (other != index)
                        pairs.emplace_back(current_->carry[other].from, current_->carry[other].to);
                after(model_.setLoopCarry(loop_, pairs));
            });
            if (carry.to_unbound) {
                // Decision 3: the engine refuses a carry whose input has no
                // loop edge -- the first element would start from nothing.
                auto *warning = new QLabel(
                    tr("%1 has no loop input: connect one for the first element to start from.")
                        .arg(qtext(carry.to)),
                    rows_);
                warning->setObjectName(fieldName(QStringLiteral("carry%1.warning").arg(index)));
                warning->setWordWrap(true);
                warning->setStyleSheet(QStringLiteral("color: #c0392b;"));
                warning->setFont(studioSmallFont());
                rows_layout_->addWidget(warning);
            }
        }
        auto *add = new QPushButton(tr("Add carry"), rows_);
        add->setObjectName(fieldName(QStringLiteral("addCarry")));
        add->setEnabled(editable);
        rows_layout_->addWidget(add);
        connect(add, &QPushButton::clicked, this, [this] {
            if (!current_ || current_->body_outputs.empty() || current_->body_inputs.empty()) return;
            std::vector<std::pair<std::string, std::string>> pairs;
            for (const auto &carry : current_->carry) pairs.emplace_back(carry.from, carry.to);
            pairs.emplace_back(current_->body_outputs.front(), current_->body_inputs.front());
            after(model_.setLoopCarry(loop_, pairs));
        });
    }

    // Decision 4: the branch table. The last row is the else.
    heading(tr("Branches"));
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    const auto choices = model_.loopBodyChoices(loop.id);
    for (std::size_t index = 0; index < loop.branches.size(); ++index) {
        const auto &branch = loop.branches[index];
        const bool last = index + 1 == loop.branches.size();
        // A draw_loop branch takes a line more: its condition's fields.
        const int row = static_cast<int>(index) * (draw ? 3 : 2);
        BranchRow drawn;
        if (last) {
            auto *label = new QLabel(tr("else"), rows_);
            label->setObjectName(branchFieldName(index, "when"));
            grid->addWidget(label, row, 0, 1, 2);
        } else if (draw) {
            // LP-4e: the condition sits on its own line under the body.
            drawn.when = nullptr;
        } else {
            drawn.when = new QComboBox(rows_);
            drawn.when->setObjectName(branchFieldName(index, "when"));
            for (const auto &choice : WhenChoices)
                drawn.when->addItem(tr(choice.label), QString::fromLatin1(choice.key));
            if (branch.when == "other") drawn.when->addItem(tr("(expression)"), QString{});
            const int when_index = drawn.when->findData(
                branch.when == "other" ? QString{} : qtext(branch.when));
            drawn.when->setCurrentIndex(when_index < 0 ? 0 : when_index);
            drawn.when->setEnabled(editable);
            grid->addWidget(drawn.when, row, 0);
            connect(drawn.when, &QComboBox::currentIndexChanged, this,
                    [this, index] { commitBranchWhen(index); });
            // LP-4f1, decision 4: nothing to type while the count is known --
            // `element =` is a spin, `element in` / `not in` a check per
            // element; an expression count keeps the line of integers.
            const auto known = loop.known_count;
            const bool checks = known && *known > 0 && *known <= CheckLimit;
            if (branch.when == "element") {
                drawn.value = new QSpinBox(rows_);
                drawn.value->setObjectName(branchFieldName(index, "value"));
                const std::int64_t current = branch.values.empty() ? 0 : branch.values.front();
                const std::int64_t top =
                    std::max<std::int64_t>(known ? *known - 1 : 4095, current);
                drawn.value->setRange(0, static_cast<int>(std::clamp<std::int64_t>(top, 0, 1 << 20)));
                drawn.value->setValue(static_cast<int>(std::max<std::int64_t>(current, 0)));
                drawn.value->setKeyboardTracking(false);
                drawn.value->setEnabled(editable);
                drawn.value->setToolTip(tr("The element this branch is for"));
                grid->addWidget(drawn.value, row, 1);
                connect(drawn.value, &QSpinBox::valueChanged, this,
                        [this, index] { commitBranchWhen(index); });
            } else if (checks && branch.when != "other") {
                auto *boxes = new QWidget(rows_);
                boxes->setObjectName(branchFieldName(index, "checks"));
                auto *box_grid = new QGridLayout(boxes);
                box_grid->setContentsMargins(0, 0, 0, 0);
                box_grid->setHorizontalSpacing(6);
                box_grid->setVerticalSpacing(1);
                for (std::int64_t element = 0; element < *known; ++element) {
                    auto *box = new QCheckBox(elementText(loop, element), boxes);
                    box->setObjectName(branchFieldName(index, "check") + QString::number(element));
                    box->setChecked(std::ranges::find(branch.values, element) != branch.values.end());
                    box->setEnabled(editable);
                    box_grid->addWidget(box, static_cast<int>(element / 8),
                                        static_cast<int>(element % 8));
                    connect(box, &QCheckBox::toggled, this, [this, index] { commitBranchWhen(index); });
                    drawn.checks.push_back(box);
                }
                grid->addWidget(boxes, row, 1);
            } else {
                drawn.values = new QLineEdit(integersText(branch.values), rows_);
                drawn.values->setObjectName(branchFieldName(index, "values"));
                drawn.values->setPlaceholderText(QStringLiteral("0"));
                drawn.values->setEnabled(editable && branch.when != "other");
                grid->addWidget(drawn.values, row, 1);
                connect(drawn.values, &QLineEdit::returnPressed, this,
                        [this, index] { commitBranchWhen(index); });
            }
        }
        drawn.body = new QComboBox(rows_);
        drawn.body->setObjectName(branchFieldName(index, "body"));
        drawn.body->addItem(tr("(nothing)"), QStringLiteral("nothing"));
        for (const auto &choice : choices) drawn.body->addItem(qtext(choice), qtext(choice));
        // A body the document names that is not a choice any more (another
        // boundary) stays visible: the engine refuses it by name.
        if (!branch.nothing && drawn.body->findData(qtext(branch.body)) < 0)
            drawn.body->addItem(qtext(branch.body), qtext(branch.body));
        drawn.body->addItem(tr("New definition…"), QStringLiteral("new"));
        drawn.body->setCurrentIndex(branch.nothing ? 0 : drawn.body->findData(qtext(branch.body)));
        drawn.body->setEnabled(editable);
        drawn.body->setToolTip(tr("The definition this branch runs; (nothing) runs no body."));
        grid->addWidget(drawn.body, row, 2);
        connect(drawn.body, &QComboBox::currentIndexChanged, this, [this, index](int chosen) {
            if (!current_ || index >= branch_rows_.size() || chosen < 0) return;
            const auto data = branch_rows_[index].body->itemData(chosen).toString();
            bool sent = false;
            if (data == QLatin1String{"nothing"}) {
                sent = model_.setLoopBranchBody(loop_, index, std::nullopt);
            } else if (data == QLatin1String{"new"}) {
                // The name is proposed, not asked for: the definition can be
                // renamed later, and a new body is only useful once it is
                // there to be opened.
                sent = model_.newLoopBranchDefinition(
                    loop_, index, model_.suggestDefinitionName(loop_ + "_body"));
            } else {
                sent = model_.setLoopBranchBody(loop_, index, data.toStdString());
            }
            if (!sent) restoreBranch(index);
            after(sent);
        });
        if (!last) {
            const auto button = [&](const char *column, const QString &text, bool enabled) {
                auto *pressed = new QPushButton(text, rows_);
                pressed->setObjectName(branchFieldName(index, column));
                pressed->setEnabled(editable && enabled);
                pressed->setMaximumWidth(34);
                return pressed;
            };
            auto *up = button("up", QStringLiteral("↑"), index > 0);
            auto *down = button("down", QStringLiteral("↓"), index + 2 < loop.branches.size());
            auto *remove = button("remove", QStringLiteral("×"), true);
            up->setToolTip(tr("Move up"));
            down->setToolTip(tr("Move down"));
            remove->setToolTip(tr("Remove this branch"));
            grid->addWidget(up, row, 3);
            grid->addWidget(down, row, 4);
            grid->addWidget(remove, row, 5);
            connect(up, &QPushButton::clicked, this,
                    [this, index] { after(model_.moveLoopBranch(loop_, index, -1)); });
            connect(down, &QPushButton::clicked, this,
                    [this, index] { after(model_.moveLoopBranch(loop_, index, 1)); });
            connect(remove, &QPushButton::clicked, this,
                    [this, index] { after(model_.removeLoopBranch(loop_, index)); });
        }
        branch_rows_.push_back(drawn);
        // LP-4e: a draw_loop branch's condition, one field per attribute.
        if (draw && !last) addDrawBranchRow(*grid, loop, index, row + 1, editable);
        // Which elements took the branch, from the report.
        auto *matched = new QLabel(rows_);
        matched->setObjectName(branchFieldName(index, "matched"));
        matched->setFont(studioSmallFont());
        matched->setWordWrap(true);
        const bool none = draw ? branch.matched_materials.empty() : branch.matched.empty();
        if (!loop.reported) {
            matched->setText(draw ? tr("materials: not reported yet")
                                  : tr("elements: not reported yet"));
        } else if (branch.unmatched || none) {
            matched->setText(draw ? tr("no material takes this branch")
                                  : tr("no element takes this branch"));
            matched->setStyleSheet(QStringLiteral("color: #c0392b;"));
        } else if (draw) {
            matched->setText(tr("materials: %1").arg(namesText(branch.matched_materials)));
        } else {
            matched->setText(tr("elements: %1").arg(integersText(branch.matched)));
        }
        grid->addWidget(matched, row + (draw && !last ? 2 : 1), 0, 1, 6);
    }
    rows_layout_->addLayout(grid);
    auto *add_branch = new QPushButton(tr("Add branch"), rows_);
    add_branch->setObjectName(fieldName(QStringLiteral("addBranch")));
    add_branch->setEnabled(editable);
    add_branch->setToolTip(draw ? tr("A branch in front of the else, for the first material to "
                                     "begin with.")
                                : tr("A branch in front of the else, for element 0 to begin with."));
    rows_layout_->addWidget(add_branch);
    connect(add_branch, &QPushButton::clicked, this,
            [this] { after(model_.addLoopBranch(loop_)); });
}

void GraphCanvasLoopFields::commitCount() {
    if (!current_ || loop_.empty()) return;
    if (count_expression_toggle_->isChecked()) {
        const auto expression = count_expression_->text().trimmed().toStdString();
        if (expression.empty() ||
            (current_->count_expression && current_->count_text == expression))
            return;
        // A plain integer typed into the expression field is an integer.
        bool integer = false;
        const auto value = count_expression_->text().trimmed().toLongLong(&integer);
        after(integer && value >= 0 ? model_.setLoopCount(loop_, value)
                                    : model_.setLoopCountExpression(loop_, expression));
        return;
    }
    const auto value = static_cast<std::int64_t>(count_->value());
    if (!current_->count_expression && current_->count == value) return;
    after(model_.setLoopCount(loop_, value));
}

void GraphCanvasLoopFields::commitOver() {
    if (!current_ || loop_.empty()) return;
    const auto over = over_->currentData().toString();
    const bool view = !current_->view_family.empty();
    if (over == QLatin1String{"draw_items"}) {
        // GL-1d, decision 2: the reading pass's contract; without one the
        // item is not offered, and a pick that still arrives goes back.
        if (model_.setLoopDrawItems(loop_)) {
            after(true);
            return;
        }
        const QSignalBlocker block{over_};
        over_->setCurrentIndex(view ? 1 : 0);
        return;
    }
    if (over == QLatin1String{"range"}) {
        // Back to a count: as many as the axis had, so the copies stay.
        if (view) after(model_.setLoopCount(loop_, current_->known_count.value_or(1)));
        return;
    }
    if (view) return;
    for (const auto &family : model_.viewFamilies())
        for (const auto &axis : family.axes)
            if (selectableAxis(axis)) {
                after(model_.setLoopViewAxis(loop_, family.family, axis.axis));
                return;
            }
    // Nothing to run over: the loop stays over its count, and says why.
    {
        const QSignalBlocker block{over_};
        over_->setCurrentIndex(0);
    }
    view_note_->setText(tr("No pass of the frame plan runs in a view family with an axis a loop "
                           "can run over."));
    form_->setRowVisible(view_note_, true);
}

void GraphCanvasLoopFields::fillViewAxes(const std::string &family, const std::string &chosen) {
    const QSignalBlocker block{view_axis_};
    view_axis_->clear();
    auto *items = qobject_cast<QStandardItemModel *>(view_axis_->model());
    const auto &families = model_.viewFamilies();
    const auto found = std::ranges::find(families, family, &GraphCanvasViewFamily::family);
    if (found != families.end())
        for (const auto &axis : found->axes) {
            if (selectableAxis(axis)) {
                view_axis_->addItem(tr("%1 ×%2").arg(qtext(axis.axis)).arg(*axis.length),
                                    qtext(axis.axis));
                continue;
            }
            // Decided each frame: shown, never chosen.
            view_axis_->addItem(tr("%1 (decided each frame)").arg(qtext(axis.axis)),
                                qtext(axis.axis));
            const int index = view_axis_->count() - 1;
            if (items != nullptr) items->item(index)->setEnabled(false);
            view_axis_->setItemData(index,
                                    tr("The engine decides this axis each frame: a loop cannot "
                                       "run over it."),
                                    Qt::ToolTipRole);
        }
    if (!chosen.empty() && view_axis_->findData(qtext(chosen)) < 0)
        view_axis_->addItem(tr("%1 (not in the frame plan)").arg(qtext(chosen)), qtext(chosen));
    int index = chosen.empty() ? -1 : view_axis_->findData(qtext(chosen));
    for (int candidate = 0; index < 0 && candidate < view_axis_->count(); ++candidate)
        if (items == nullptr || items->item(candidate)->isEnabled()) index = candidate;
    view_axis_->setCurrentIndex(index < 0 ? 0 : index);
}

void GraphCanvasLoopFields::commitViewAxis(bool family_changed) {
    if (!current_ || loop_.empty()) return;
    const auto family = view_family_->currentData().toString().toStdString();
    if (family.empty()) return;
    if (family_changed) fillViewAxes(family, {});
    const int index = view_axis_->currentIndex();
    if (index < 0) return;
    const auto *items = qobject_cast<QStandardItemModel *>(view_axis_->model());
    if (items != nullptr && !items->item(index)->isEnabled()) {
        view_note_->setText(tr("Every axis of %1 is decided each frame: a loop cannot run over it.")
                                .arg(qtext(family)));
        return;
    }
    const auto axis = view_axis_->itemData(index).toString().toStdString();
    if (family == current_->view_family && axis == current_->view_axis) return;
    after(model_.setLoopViewAxis(loop_, family, axis));
}

void GraphCanvasLoopFields::insertCountFunction(const QString &function) {
    if (!current_ || loop_.empty()) return;
    const auto target = count_target_->currentText();
    if (target.isEmpty()) return;
    const auto call = QStringLiteral("%1(\"%2\")").arg(function, target);
    // Over nothing, or over a plain integer, the call is the expression;
    // otherwise it goes where the cursor is.
    const auto text = count_expression_->text().trimmed();
    bool integer = false;
    (void)text.toLongLong(&integer);
    if (text.isEmpty() || integer) count_expression_->setText(call);
    else count_expression_->insert(call);
    commitCount();
}

void GraphCanvasLoopFields::rebuildWhereChecks(const GraphCanvasLoop &loop, bool editable) {
    for (QCheckBox *box : where_boxes_) {
        box->hide();
        box->setObjectName(QString{});
        box->deleteLater();
    }
    where_boxes_.clear();
    const auto known = loop.known_count;
    if (loop.draw_items || !known || *known <= 0 || *known > CheckLimit) return;
    auto *grid = static_cast<QGridLayout *>(where_checks_->layout());
    for (std::int64_t element = 0; element < *known; ++element) {
        auto *box = new QCheckBox(elementText(loop, element), where_checks_);
        box->setObjectName(fieldName(QStringLiteral("whereValue%1").arg(element)));
        box->setChecked(std::ranges::find(loop.where_values, element) != loop.where_values.end());
        box->setEnabled(editable && !loop.where.empty());
        grid->addWidget(box, static_cast<int>(element / 8), static_cast<int>(element % 8));
        connect(box, &QCheckBox::toggled, this, [this] { commitWhere(); });
        where_boxes_.push_back(box);
    }
}

void GraphCanvasLoopFields::commitWhere() {
    if (!current_ || loop_.empty()) return;
    const auto kind = where_kind_->currentData().toString().toStdString();
    where_values_->setEnabled(model_.canEdit() && !kind.empty());
    for (QCheckBox *box : where_boxes_) box->setEnabled(model_.canEdit() && !kind.empty());
    if (kind.empty()) {
        after(model_.setLoopWhere(loop_, {}, {}));
        return;
    }
    // LP-4f1: the checked elements. None checked yet is not sent: the next
    // check says which.
    if (!where_boxes_.empty()) {
        std::vector<std::int64_t> checked;
        for (std::size_t element = 0; element < where_boxes_.size(); ++element)
            if (where_boxes_[element]->isChecked())
                checked.push_back(static_cast<std::int64_t>(element));
        if (checked.empty()) return;
        after(model_.setLoopWhere(loop_, kind, checked));
        return;
    }
    const auto values = parseIntegers(where_values_->text());
    if (!values) {
        where_values_->setToolTip(tr("Integers, separated by commas"));
        return;
    }
    after(model_.setLoopWhere(loop_, kind, *values));
}

void GraphCanvasLoopFields::commitLimit() {
    if (!current_ || loop_.empty()) return;
    limit_->setEnabled(model_.canEdit() && limit_enabled_->isChecked());
    after(model_.setLoopLimit(
        loop_, limit_enabled_->isChecked()
                   ? std::optional<std::int64_t>{static_cast<std::int64_t>(limit_->value())}
                   : std::nullopt));
}

void GraphCanvasLoopFields::commitCarry() {
    if (loop_.empty()) return;
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const auto &row : carry_rows_)
        pairs.emplace_back(row.from->currentText().toStdString(),
                           row.to->currentText().toStdString());
    after(model_.setLoopCarry(loop_, pairs));
}

void GraphCanvasLoopFields::commitOutputs() {
    if (loop_.empty()) return;
    std::vector<std::string> outputs;
    for (const auto &[port, box] : output_boxes_)
        if (box->isChecked()) outputs.push_back(port);
    after(model_.setLoopOutputs(loop_, outputs));
}

namespace {

// The any / yes / no of a check the condition may leave out.
constexpr const char *AnyYesNo[] = {"(any)", "yes", "no"};

void fillAnyYesNo(QComboBox &combo, const std::optional<bool> &value) {
    for (const char *label : AnyYesNo) combo.addItem(QString::fromLatin1(label));
    combo.setCurrentIndex(!value ? 0 : (*value ? 1 : 2));
}

std::optional<bool> readAnyYesNo(const QComboBox &combo) {
    if (combo.currentIndex() <= 0) return std::nullopt;
    return combo.currentIndex() == 1;
}

// UX-3, decision 5: has_variant's item data -- "" any, "\x1fyes" / "\x1fno"
// (some variant / none), or a variant's name.
QString hasVariantData(const GraphCanvasDrawWhen &when) {
    if (when.has_variant)
        return *when.has_variant ? QStringLiteral("\x1fyes") : QStringLiteral("\x1fno");
    if (when.has_variant_named) return qtext(*when.has_variant_named);
    return {};
}

void fillHasVariant(QComboBox &combo, const std::vector<std::string> &variants,
                    const GraphCanvasDrawWhen &when) {
    combo.addItem(QString::fromLatin1(AnyYesNo[0]), QString{});
    combo.addItem(QString::fromLatin1(AnyYesNo[1]), QStringLiteral("\x1fyes"));
    combo.addItem(QString::fromLatin1(AnyYesNo[2]), QStringLiteral("\x1fno"));
    auto names = variants;
    if (when.has_variant_named &&
        std::ranges::find(names, *when.has_variant_named) == names.end())
        names.push_back(*when.has_variant_named);
    for (const auto &name : names)
        combo.addItem(QObject::tr("= %1").arg(qtext(name)), qtext(name));
    combo.setCurrentIndex(std::max(0, combo.findData(hasVariantData(when))));
}

void readHasVariant(const QComboBox &combo, GraphCanvasDrawWhen &when) {
    const auto data = combo.currentData().toString();
    when.has_variant.reset();
    when.has_variant_named.reset();
    if (data == QLatin1String{"\x1fyes"}) when.has_variant = true;
    else if (data == QLatin1String{"\x1fno"}) when.has_variant = false;
    else if (!data.isEmpty()) when.has_variant_named = data.toStdString();
}

} // namespace

void GraphCanvasLoopFields::addDrawBranchRow(QGridLayout &grid, const GraphCanvasLoop &loop,
                                             std::size_t index, int row, bool editable) {
    auto &drawn = branch_rows_[index];
    const auto &when = loop.branches[index].draw_when;
    auto *line = new QHBoxLayout;
    const auto label = [this, line](const QString &text) {
        auto *caption = new QLabel(text, rows_);
        caption->setFont(studioSmallFont());
        line->addWidget(caption);
    };
    // material = <key>: the engine's materials by name (UX-3, decision 5),
    // then the ones the report names that it does not list, by key.
    label(tr("material"));
    drawn.material = new QComboBox(rows_);
    drawn.material->setObjectName(branchFieldName(index, "material"));
    drawn.material->addItem(tr("(any)"), QString{});
    for (const auto &material : model_.materials()) {
        drawn.material->addItem(material.builtin ? tr("%1 (built in)").arg(qtext(material.name))
                                                 : qtext(material.name),
                                qtext(material.id));
        drawn.material->setItemData(drawn.material->count() - 1, qtext(material.id),
                                    Qt::ToolTipRole);
    }
    for (const auto &material : loop.materials)
        if (drawn.material->findData(qtext(material)) < 0)
            drawn.material->addItem(qtext(material), qtext(material));
    // A key the report does not name (yet) stays, as the document spells it.
    if (when.material && drawn.material->findData(qtext(*when.material)) < 0)
        drawn.material->addItem(qtext(*when.material), qtext(*when.material));
    drawn.material->setCurrentIndex(
        when.material ? drawn.material->findData(qtext(*when.material)) : 0);
    drawn.material->setToolTip(tr("The material this branch is for, by its matching key"));
    line->addWidget(drawn.material, 2);
    label(tr("tags"));
    drawn.tags = new QLineEdit(when.tags ? namesText(when.tags->include) : QString{}, rows_);
    drawn.tags->setObjectName(branchFieldName(index, "tags"));
    drawn.tags->setPlaceholderText(tr("any of"));
    drawn.tags->setToolTip(tr("Materials with any of these tags (commas between)"));
    line->addWidget(drawn.tags, 1);
    // UX-3, decision 5: picked among the engine's tags while it lists any.
    const auto engine_tags = model_.materialTags();
    drawn.tag_picker = new GraphCanvasTagPicker(branchFieldName(index, "tagsPick"),
                                                tr("any of: -"), rows_);
    drawn.tag_picker->setToolTip(tr("Materials with any of these tags"));
    drawn.tag_picker->setTags(engine_tags,
                              when.tags ? when.tags->include : std::vector<std::string>{});
    drawn.tag_picker->changed = [this, index] { commitDrawWhen(index); };
    line->addWidget(drawn.tag_picker, 1);
    drawn.tags->setVisible(engine_tags.empty());
    drawn.tag_picker->setVisible(!engine_tags.empty());
    label(tr("contract"));
    drawn.contract = new QComboBox(rows_);
    drawn.contract->setObjectName(branchFieldName(index, "contract"));
    drawn.contract->addItem(tr("(any)"), QString{});
    for (const auto &contract : GraphCanvasModel::materialShaderContracts())
        drawn.contract->addItem(qtext(contract), qtext(contract));
    drawn.contract->setCurrentIndex(
        when.contract ? std::max(0, drawn.contract->findData(qtext(*when.contract))) : 0);
    line->addWidget(drawn.contract);
    label(tr("skinned"));
    drawn.skinned = new QComboBox(rows_);
    drawn.skinned->setObjectName(branchFieldName(index, "skinned"));
    fillAnyYesNo(*drawn.skinned, when.skinned);
    line->addWidget(drawn.skinned);
    label(tr("has variant"));
    drawn.has_variant = new QComboBox(rows_);
    drawn.has_variant->setObjectName(branchFieldName(index, "hasVariant"));
    // UX-3, decision 5: any / some variant / none, then one variant by
    // name among those the engine's materials have.
    fillHasVariant(*drawn.has_variant, model_.materialVariants(), when);
    line->addWidget(drawn.has_variant);
    for (QWidget *field : std::initializer_list<QWidget *>{
             drawn.material, drawn.tags, drawn.tag_picker, drawn.contract, drawn.skinned,
             drawn.has_variant})
        field->setEnabled(editable);
    grid.addLayout(line, row, 0, 1, 6);
    for (QComboBox *combo : {drawn.material, drawn.contract, drawn.skinned, drawn.has_variant})
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, index] { commitDrawWhen(index); });
    connect(drawn.tags, &QLineEdit::returnPressed, this, [this, index] { commitDrawWhen(index); });
}

void GraphCanvasLoopFields::commitDrawWhen(std::size_t branch) {
    if (!current_ || loop_.empty() || branch >= branch_rows_.size()) return;
    const auto &row = branch_rows_[branch];
    if (row.material == nullptr) return;
    GraphCanvasDrawWhen when;
    if (const auto material = row.material->currentData().toString(); !material.isEmpty())
        when.material = material.toStdString();
    // The tags a person types are the include list; an exclude or
    // require_all the document already holds is kept as it is.
    auto tags = branch < current_->branches.size() && current_->branches[branch].draw_when.tags
                    ? *current_->branches[branch].draw_when.tags
                    : GraphCanvasTagFilter{};
    tags.include = row.tag_picker != nullptr && !row.tag_picker->isHidden()
                       ? row.tag_picker->selected()
                       : parseNames(row.tags->text());
    if (!tags.empty()) when.tags = std::move(tags);
    if (const auto contract = row.contract->currentData().toString(); !contract.isEmpty())
        when.contract = contract.toStdString();
    when.skinned = readAnyYesNo(*row.skinned);
    readHasVariant(*row.has_variant, when);
    if (when.empty()) {
        // Nothing left to test would make this row a second else.
        row.material->setToolTip(tr("Pick what this branch is for: only the last row has no "
                                    "condition."));
        return;
    }
    const bool sent = model_.setLoopBranchDrawWhen(loop_, branch, when);
    if (!sent) restoreBranch(branch);
    after(sent);
}

void GraphCanvasLoopFields::restoreBranch(std::size_t branch) {
    if (!current_ || branch >= branch_rows_.size() || branch >= current_->branches.size())
        return;
    const auto &row = branch_rows_[branch];
    const auto &shown = current_->branches[branch];
    const auto put = [](QComboBox *combo, int index) {
        if (combo == nullptr) return;
        const QSignalBlocker block{combo};
        combo->setCurrentIndex(std::max(0, index));
    };
    if (row.body != nullptr)
        put(row.body, shown.nothing ? 0 : row.body->findData(qtext(shown.body)));
    if (row.when != nullptr)
        put(row.when, row.when->findData(shown.when == "other" ? QString{} : qtext(shown.when)));
    const auto &when = shown.draw_when;
    if (row.material != nullptr)
        put(row.material, when.material ? row.material->findData(qtext(*when.material)) : 0);
    if (row.contract != nullptr)
        put(row.contract, when.contract ? row.contract->findData(qtext(*when.contract)) : 0);
    if (row.skinned != nullptr) put(row.skinned, !when.skinned ? 0 : (*when.skinned ? 1 : 2));
    if (row.has_variant != nullptr)
        put(row.has_variant, row.has_variant->findData(hasVariantData(when)));
}

void GraphCanvasLoopFields::commitWhereTags() {
    if (!current_ || loop_.empty() || !current_->draw_items) return;
    GraphCanvasTagFilter tags;
    if (tags_listed_) {
        tags.include = pick_include_->selected();
        tags.exclude = pick_exclude_->selected();
        tags.require_all = pick_require_all_->selected();
    } else {
        tags.include = parseNames(tags_include_->text());
        tags.exclude = parseNames(tags_exclude_->text());
        tags.require_all = parseNames(tags_require_all_->text());
    }
    if (tags == current_->where_tags) return;
    after(model_.setLoopWhereTags(loop_, tags));
}

// --- LP-4e: the draw step and material pass fields.

namespace {

// A render_state key draw.override takes (graphmodule.cpp) with its values;
// "(material's)" leaves the key out, so the material's own state stays.
QComboBox *stateCombo(QWidget *parent, const QString &name, const std::vector<const char *> &values,
                      const QString &tip) {
    auto *combo = new QComboBox(parent);
    combo->setObjectName(name);
    combo->addItem(QObject::tr("(material's)"), QString{});
    for (const char *value : values) combo->addItem(QString::fromLatin1(value), QString::fromLatin1(value));
    combo->setToolTip(tip);
    return combo;
}

QString drawFieldName(const char *field) {
    return QStringLiteral("pelican.graphCanvasDraw.") + QString::fromLatin1(field);
}

} // namespace

std::vector<std::string> GraphCanvasDrawFields::parseDefines(const QString &text) {
    std::vector<std::string> defines;
    for (const auto &line : text.split(QLatin1Char{'\n'})) {
        const auto define = line.trimmed();
        if (!define.isEmpty()) defines.push_back(define.toStdString());
    }
    return defines;
}

GraphCanvasDrawFields::GraphCanvasDrawFields(GraphCanvasModel &model, std::function<void()> submitted,
                                             QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)} {
    setObjectName(QStringLiteral("pelican.graphCanvasDraw"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);
    heading_ = new QLabel(this);
    heading_->setFont(studioSmallFont());
    heading_->setWordWrap(true);
    layout->addWidget(heading_);
    const auto caption = [this](const QString &text, QWidget *parent) {
        auto *label = new QLabel(text, parent);
        label->setFont(studioSmallFont());
        return label;
    };

    // render.material_pass: the contract decides its sockets.
    contract_row_ = new QWidget(this);
    auto *contract_form = new QFormLayout(contract_row_);
    contract_form->setContentsMargins(0, 0, 0, 0);
    contract_ = new QComboBox(contract_row_);
    contract_->setObjectName(drawFieldName("contract"));
    for (const auto &contract : GraphCanvasModel::materialPassContracts())
        contract_->addItem(qtext(contract), qtext(contract));
    contract_->setToolTip(tr("The materials this pass draws; its sockets follow it."));
    contract_form->addRow(caption(tr("Contract"), contract_row_), contract_);
    // UX-3, decision 5: the variant every material of the pass draws with,
    // among the variants the engine's materials have.
    material_variant_ = new QComboBox(contract_row_);
    material_variant_->setObjectName(drawFieldName("materialVariant"));
    material_variant_->setToolTip(tr("The variant each material draws with in this pass; a "
                                     "material without it is refused."));
    contract_form->addRow(caption(tr("Variant"), contract_row_), material_variant_);
    connect(material_variant_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || node_.empty()) return;
        after(model_.setMaterialPassVariant(
            node_, material_variant_->itemData(index).toString().toStdString()));
    });
    layout->addWidget(contract_row_);
    connect(contract_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || node_.empty()) return;
        after(model_.setMaterialPassContract(node_, contract_->itemData(index).toString().toStdString()));
    });

    // draw.variant: the named variant each material draws with.
    variant_row_ = new QWidget(this);
    auto *variant_form = new QFormLayout(variant_row_);
    variant_form->setContentsMargins(0, 0, 0, 0);
    variant_ = new QComboBox(variant_row_);
    variant_->setObjectName(drawFieldName("variant"));
    variant_->setEditable(true);
    variant_->setInsertPolicy(QComboBox::NoInsert);
    variant_->setToolTip(tr("The variant each material of this branch draws with; a material "
                            "without it is refused (graph.loop_variant_missing@1)."));
    variant_form->addRow(caption(tr("Variant"), variant_row_), variant_);
    layout->addWidget(variant_row_);
    const auto commit_variant = [this] {
        if (node_.empty()) return;
        after(model_.setDrawVariant(scope_, node_, variant_->currentText().trimmed().toStdString()));
    };
    connect(variant_, &QComboBox::activated, this, commit_variant);
    variant_line_ = variant_->lineEdit();
    connect(variant_line_, &QLineEdit::returnPressed, this, commit_variant);

    // draw.override: the render state it replaces, and its defines.
    override_rows_ = new QWidget(this);
    auto *override_form = new QFormLayout(override_rows_);
    override_form->setContentsMargins(0, 0, 0, 0);
    cull_ = stateCombo(override_rows_, drawFieldName("cull"), {"none", "front", "back"},
                       tr("Which faces are not drawn"));
    depth_test_ = stateCombo(override_rows_, drawFieldName("depthTest"), {"on", "off"},
                             tr("Whether the draw tests depth"));
    depth_write_ = stateCombo(override_rows_, drawFieldName("depthWrite"), {"on", "off"},
                              tr("Whether the draw writes depth (not with the test off)"));
    blend_ = stateCombo(override_rows_, drawFieldName("blend"), {"opaque", "blend", "additive"},
                        tr("How the draw's colour meets what is there"));
    // UX-3, decision 4: the comparison the depth test makes.
    depth_compare_ = stateCombo(override_rows_, drawFieldName("depthCompare"),
                                {"never", "less", "equal", "less_equal", "greater", "not_equal",
                                 "greater_equal", "always"},
                                tr("What the depth test compares the draw's depth by"));
    override_form->addRow(caption(tr("Cull"), override_rows_), cull_);
    override_form->addRow(caption(tr("Depth test"), override_rows_), depth_test_);
    override_form->addRow(caption(tr("Depth write"), override_rows_), depth_write_);
    override_form->addRow(caption(tr("Blend"), override_rows_), blend_);
    override_form->addRow(caption(tr("Depth compare"), override_rows_), depth_compare_);
    defines_ = new QPlainTextEdit(override_rows_);
    defines_->setObjectName(drawFieldName("defines"));
    defines_->setMaximumHeight(70);
    defines_->setPlaceholderText(tr("One define a line: NAME or NAME=VALUE"));
    defines_->setToolTip(tr("Shader defines the draw adds; they need the runtime shader compiler."));
    override_form->addRow(caption(tr("Defines"), override_rows_), defines_);
    apply_defines_ = new QPushButton(tr("Apply defines"), override_rows_);
    apply_defines_->setObjectName(drawFieldName("applyDefines"));
    override_form->addRow(QString{}, apply_defines_);
    layout->addWidget(override_rows_);
    for (QComboBox *combo : {cull_, depth_test_, depth_write_, blend_, depth_compare_})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { commitOverride(); });
    connect(apply_defines_, &QPushButton::clicked, this, [this] { commitOverride(); });
    hide();
}

void GraphCanvasDrawFields::after(bool submitted) {
    if (!submitted) restore();
    if (submitted && submitted_) submitted_();
}

void GraphCanvasDrawFields::showNode(const GraphCanvasNode *node) {
    showNode(node, model_.currentScope());
}

void GraphCanvasDrawFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    const bool shown = node != nullptr && (node->op == "render.material_pass" ||
                                           node->op == "draw.variant" || node->op == "draw.override");
    if (!shown) {
        node_.clear();
        op_.clear();
        scope_.clear();
        hide();
        return;
    }
    const bool same = node_ == node->id && op_ == node->op && scope_ == scope;
    node_ = node->id;
    op_ = node->op;
    scope_ = scope;
    contract_row_->setVisible(op_ == "render.material_pass");
    variant_row_->setVisible(op_ == "draw.variant");
    override_rows_->setVisible(op_ == "draw.override");
    if (op_ == "render.material_pass")
        heading_->setText(tr("Material pass %1").arg(qtext(node_)));
    else if (op_ == "draw.variant")
        heading_->setText(tr("Draw %1 with a named variant").arg(qtext(node_)));
    else
        heading_->setText(tr("Draw %1 with another render state").arg(qtext(node_)));
    // What is being typed into the node on screen stays.
    // UX-3, decision 5: the variants come with the authoring context.
    model_.askRenderTargetVocabulary();
    showParameters(node->parameters,
                   same && variant_->lineEdit() != nullptr && variant_->lineEdit()->hasFocus(),
                   same && defines_->hasFocus());
    followEditable();
    show();
}

void GraphCanvasDrawFields::showParameters(const nlohmann::json &parameters, bool keep_variant,
                                           bool keep_defines) {
    const auto string = [&](const nlohmann::json &object, const char *key) {
        const auto found = object.is_object() ? object.find(key) : object.end();
        return object.is_object() && found != object.end() && found->is_string()
                   ? qtext(found->get<std::string>())
                   : QString{};
    };
    if (op_ == "render.material_pass") {
        const QSignalBlocker block{contract_};
        const auto contract = string(parameters, "material_contract");
        if (contract_->findData(contract) < 0 && !contract.isEmpty())
            contract_->addItem(contract, contract);
        contract_->setCurrentIndex(contract_->findData(contract));
        // UX-3, decision 5: shown while the engine lists variants, or the
        // pass names one.
        const auto variants = model_.materialVariants();
        const auto current = string(parameters, "material_variant");
        const QSignalBlocker block_variant{material_variant_};
        material_variant_->clear();
        material_variant_->addItem(tr("(each material's own)"), QString{});
        for (const auto &variant : variants) material_variant_->addItem(qtext(variant), qtext(variant));
        if (!current.isEmpty() && material_variant_->findData(current) < 0)
            material_variant_->addItem(current, current);
        material_variant_->setCurrentIndex(std::max(0, material_variant_->findData(current)));
        static_cast<QFormLayout *>(contract_row_->layout())
            ->setRowVisible(material_variant_, !variants.empty() || !current.isEmpty());
    } else if (op_ == "draw.variant") {
        if (!keep_variant) {
            const QSignalBlocker block{variant_};
            // UX-3, decision 5: picked among the variants the engine's
            // materials have; typed, as before, while it lists none.
            const auto variants = model_.materialVariants();
            const auto current = string(parameters, "variant");
            variant_->clear();
            variant_->setEditable(variants.empty());
            if (variant_->isEditable() && variant_->lineEdit() != variant_line_) {
                // The combo makes its line again each time it becomes
                // editable: Enter in the new one sends, as in the first.
                variant_line_ = variant_->lineEdit();
                connect(variant_line_, &QLineEdit::returnPressed, this, [this] {
                    if (node_.empty()) return;
                    after(model_.setDrawVariant(scope_, node_,
                                                variant_->currentText().trimmed().toStdString()));
                });
            }
            if (variants.empty()) {
                const auto suggested = qtext(model_.suggestDrawVariant(scope_));
                if (suggested != QLatin1String{"variant"}) variant_->addItem(suggested);
            }
            for (const auto &variant : variants) variant_->addItem(qtext(variant));
            if (variant_->findText(current) < 0) variant_->addItem(current);
            variant_->setCurrentIndex(variant_->findText(current));
            if (variant_->isEditable()) variant_->setEditText(current);
        }
    } else if (op_ == "draw.override") {
        const auto state = parameters.is_object() ? parameters.value("render_state", nlohmann::json::object())
                                                  : nlohmann::json::object();
        const auto pick = [&](QComboBox *combo, const char *key, bool boolean) {
            const QSignalBlocker block{combo};
            QString value;
            if (state.is_object() && state.contains(key)) {
                const auto &stored = state.at(key);
                value = boolean ? (stored.is_boolean() ? (stored.get<bool>() ? QStringLiteral("on")
                                                                              : QStringLiteral("off"))
                                                       : QString{})
                                : (stored.is_string() ? qtext(stored.get<std::string>()) : QString{});
            }
            combo->setCurrentIndex(std::max(0, combo->findData(value)));
        };
        pick(cull_, "cull", false);
        pick(depth_test_, "depth_test", true);
        pick(depth_write_, "depth_write", true);
        pick(blend_, "blend", false);
        pick(depth_compare_, "depth_compare", false);
        if (!keep_defines) {
            QStringList lines;
            if (parameters.is_object())
                for (const auto &define : parameters.value("defines", nlohmann::json::array()))
                    if (define.is_string()) lines << qtext(define.get<std::string>());
            const QSignalBlocker block{defines_};
            defines_->setPlainText(lines.join(QLatin1Char{'\n'}));
        }
    }
}

void GraphCanvasDrawFields::followEditable() {
    // Q-18, as the Loop fields (Q-17): the rows were filled for a model that
    // could edit then; an edit in flight does not fill them again, so they
    // are switched off from the moment it goes out until it is read back.
    const bool editable = model_.canEdit();
    contract_row_->setEnabled(editable);
    variant_row_->setEnabled(editable);
    material_variant_->setEnabled(editable);
    override_rows_->setEnabled(editable);
}

void GraphCanvasDrawFields::restore() {
    // Q-18: a choice the model refused goes back to what the document says,
    // so the field never shows a value that was not sent. Defines being
    // typed stay: they are text, and Apply defines sends them again.
    if (node_.empty()) return;
    showParameters(model_.nodeParameters(scope_, node_), false, true);
}

void GraphCanvasDrawFields::commitOverride() {
    if (node_.empty() || op_ != "draw.override") return;
    nlohmann::json state = nlohmann::json::object();
    const auto put = [&](QComboBox *combo, const char *key, bool boolean) {
        const auto value = combo->currentData().toString();
        if (value.isEmpty()) return;
        if (boolean) state[key] = value == QLatin1String{"on"};
        else state[key] = value.toStdString();
    };
    put(cull_, "cull", false);
    put(depth_test_, "depth_test", true);
    put(depth_write_, "depth_write", true);
    put(blend_, "blend", false);
    put(depth_compare_, "depth_compare", false);
    const auto defines = parseDefines(defines_->toPlainText());
    if (state.empty() && defines.empty()) {
        // The engine refuses an override that replaces nothing.
        cull_->setToolTip(tr("An override replaces at least one render state or adds a define; "
                             "draw.material draws the material as it is."));
        return;
    }
    after(model_.setDrawOverride(scope_, node_, state, defines));
}

void GraphCanvasLoopFields::commitBranchWhen(std::size_t branch) {
    if (!current_ || loop_.empty() || branch >= branch_rows_.size()) return;
    const auto &row = branch_rows_[branch];
    if (row.when == nullptr) return;
    const auto when = row.when->currentData().toString().toStdString();
    if (when.empty()) return;
    // LP-4f1: the spin's one value, the checked elements, or the line.
    std::optional<std::vector<std::int64_t>> values;
    if (row.value != nullptr) {
        values = std::vector<std::int64_t>{static_cast<std::int64_t>(row.value->value())};
    } else if (!row.checks.empty()) {
        values.emplace();
        for (std::size_t element = 0; element < row.checks.size(); ++element)
            if (row.checks[element]->isChecked())
                values->push_back(static_cast<std::int64_t>(element));
    } else if (row.values != nullptr) {
        values = parseIntegers(row.values->text());
        if (!values) {
            row.values->setToolTip(tr("Integers, separated by commas"));
            return;
        }
    } else {
        return;
    }
    // "element =" takes exactly one; the list keeps its first, or 0.
    if (when == "element") {
        const auto first = values->empty() ? std::int64_t{0} : values->front();
        values = std::vector<std::int64_t>{first};
    }
    const bool sent = model_.setLoopBranchWhen(loop_, branch, when, *values);
    if (!sent) restoreBranch(branch);
    after(sent);
}

// --- LP-4f1, decision 4: the element another loop's p[] is read at.

namespace {
constexpr const char *ElementPresets[] = {"element", "element - 1", "element + 1"};
} // namespace

GraphCanvasElementChoice::GraphCanvasElementChoice(const QString &object_name, QWidget *parent)
    : QWidget{parent} {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    choice_ = new QComboBox(this);
    choice_->setObjectName(object_name);
    for (const char *preset : ElementPresets)
        choice_->addItem(QString::fromLatin1(preset), QString::fromLatin1(preset));
    choice_->addItem(tr("Custom…"), QString{});
    choice_->setToolTip(tr("Which element it reads, per element of its own loop: `element` is "
                           "that element, `element - 1` the one before it."));
    layout->addWidget(choice_, 1);
    custom_ = new QLineEdit(this);
    custom_->setObjectName(object_name + QStringLiteral(".custom"));
    custom_->setPlaceholderText(QStringLiteral("element * 2"));
    custom_->hide();
    layout->addWidget(custom_, 1);
    connect(choice_, &QComboBox::currentIndexChanged, this, [this] {
        custom_->setVisible(choice_->currentData().toString().isEmpty());
    });
}

QString GraphCanvasElementChoice::expression() const {
    if (const auto preset = choice_->currentData().toString(); !preset.isEmpty()) return preset;
    const auto typed = custom_->text().trimmed();
    return typed.isEmpty() ? QStringLiteral("element") : typed;
}

void GraphCanvasElementChoice::setExpression(const QString &expression) {
    const auto wanted = expression.trimmed();
    const int preset = wanted.isEmpty() ? -1 : choice_->findData(wanted);
    if (preset >= 0) {
        choice_->setCurrentIndex(preset);
        return;
    }
    custom_->setText(wanted);
    choice_->setCurrentIndex(choice_->count() - 1);
}

// --- LP-4f1, decision 2: a render node's View field on the Graph tab.

GraphCanvasViewFields::GraphCanvasViewFields(GraphCanvasModel &model,
                                             std::function<void()> submitted, QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)} {
    setObjectName(QStringLiteral("pelican.graphCanvasRender"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);
    heading_ = new QLabel(this);
    heading_->setObjectName(QStringLiteral("pelican.graphCanvasRender.heading"));
    heading_->setFont(studioSmallFont());
    heading_->setWordWrap(true);
    layout->addWidget(heading_);
    view_ = new ViewSelectField(QStringLiteral("pelican.graphCanvasRender"), this);
    layout->addWidget(view_);
    view_->changed = [this] { commit(); };
    hide();
}

void GraphCanvasViewFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    field_ = node == nullptr ? std::nullopt : model_.viewField(scope, node->id);
    if (!field_) {
        node_.clear();
        scope_.clear();
        shown_.clear();
        view_->clear();
        hide();
        return;
    }
    node_ = node->id;
    scope_ = scope;
    // Q-18: whether the model can edit is not part of what is shown -- the
    // field follows it (followEditable) without being loaded again.
    QStringList parts{qtext(scope), qtext(node_), qtext(field_->family), qtext(field_->loop),
                      qtext(field_->element_axis),
                      field_->element_chosen ? QStringLiteral("e") : QString{}};
    for (const auto &axis : field_->axes) {
        parts << qtext(axis.axis)
              << (axis.length ? QString::number(*axis.length) : QStringLiteral("-"))
              << (axis.runtime ? QStringLiteral("r") : QString{});
        for (const auto &label : axis.labels) parts << qtext(label);
    }
    for (const auto &chosen : field_->view_select)
        parts << qtext(chosen.axis) << QString::number(chosen.value);
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    if (shown != shown_) {
        shown_ = shown;
        heading_->setText(field_->loop.empty()
                              ? tr("View (%1)").arg(qtext(field_->family))
                              : tr("View (%1) · in loop %2")
                                    .arg(qtext(field_->family), qtext(field_->loop)));
        load();
    }
    followEditable();
    show();
}

void GraphCanvasViewFields::load() {
    if (!field_) return;
    view_->load(field_->axes, field_->view_select, field_->family, field_->element_axis,
                field_->element_chosen);
}

void GraphCanvasViewFields::followEditable() {
    // Q-18: an edit in flight changes nothing the field shows, so it is not
    // loaded again for it; it follows the model here instead.
    view_->setEnabled(model_.canEdit());
}

void GraphCanvasViewFields::commit() {
    if (!field_ || node_.empty()) return;
    // What the field does not show -- an expression other than `element` --
    // is kept; everything it shows is written from it.
    auto parameters = model_.nodeParameters(scope_, node_);
    auto select = parameters.is_object() ? parameters.value("view_select", nlohmann::json::object())
                                         : nlohmann::json::object();
    if (!select.is_object()) select = nlohmann::json::object();
    for (auto entry = select.begin(); entry != select.end();) {
        const bool shown = entry->is_number_integer() ||
                           (entry->is_object() && entry->size() == 1 &&
                            entry->value("$expr", std::string{}) == "element");
        entry = shown ? select.erase(entry) : std::next(entry);
    }
    for (const auto &chosen : view_->selection()) select[chosen.axis] = chosen.value;
    if (const auto axis = view_->elementAxis())
        select[*axis] = nlohmann::json{{"$expr", "element"}};
    if (!model_.setViewSelect(scope_, node_, select,
                              field_->writes_family ? field_->family : std::string{})) {
        // Q-18: refused (an edit in flight, or nothing changed): the field
        // shows the document's choice again, not the one that was not sent,
        // in the combo that was just used.
        view_->showSelection(field_->view_select, field_->element_axis, field_->element_chosen);
        return;
    }
    if (submitted_) submitted_();
}

} // namespace PelicanStudio
