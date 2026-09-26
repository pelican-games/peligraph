#include "graphcanvasloopfields.hpp"

#include "studiotheme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
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
QString signature(const GraphCanvasLoop &loop, const std::vector<std::string> &choices,
                  bool editable) {
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
    return parts.join(QChar{0x1f});
}

} // namespace

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
    connect(count_, &QSpinBox::editingFinished, this, [this] { commitCount(); });
    connect(count_expression_, &QLineEdit::returnPressed, this, [this] { commitCount(); });
    connect(count_expression_toggle_, &QToolButton::toggled, this, [this](bool expression) {
        count_->setVisible(!expression);
        count_expression_->setVisible(expression);
        if (expression && count_expression_->text().isEmpty())
            count_expression_->setText(QString::number(count_->value()));
    });

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
    form->addRow(caption(tr("Tags")), tags_row_);
    form_ = form;
    layout->addLayout(form);

    rows_ = new QWidget(this);
    rows_layout_ = new QVBoxLayout(rows_);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(rows_);
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
    const auto shown = signature(*loop, choices, model_.canEdit()).toStdString();
    loop_ = id;
    current_ = loop;
    if (shown != shown_) {
        shown_ = shown;
        rebuild(*loop);
    }
    show();
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
    rows_ = new QWidget(this);
    rows_layout_ = new QVBoxLayout(rows_);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    static_cast<QVBoxLayout *>(layout())->addWidget(rows_);
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
    for (QWidget *field : std::initializer_list<QWidget *>{
             count_, count_expression_, count_expression_toggle_, order_, limit_enabled_,
             tier_, where_kind_})
        field->setEnabled(editable);
    // LP-4e: a draw_loop takes no count, order, limit, place or element
    // `where` (the engine refuses a limit or a carry there, and its order is
    // the phase's); it has a contract and a tag `where` instead.
    const bool draw = loop.draw_items;
    form_->setRowVisible(count_row_, !draw);
    form_->setRowVisible(order_, !draw);
    form_->setRowVisible(limit_row_, !draw);
    form_->setRowVisible(place_, !draw);
    form_->setRowVisible(where_row_, !draw);
    form_->setRowVisible(contract_, draw);
    form_->setRowVisible(tags_row_, draw);
    if (draw) {
        contract_->setText(loop.pass.empty()
                               ? qtext(loop.contract)
                               : tr("%1 (the pass %2)").arg(qtext(loop.contract), qtext(loop.pass)));
        tags_include_->setText(namesText(loop.where_tags.include));
        tags_exclude_->setText(namesText(loop.where_tags.exclude));
        tags_require_all_->setText(namesText(loop.where_tags.require_all));
        for (QLineEdit *field : {tags_include_, tags_exclude_, tags_require_all_})
            field->setEnabled(editable);
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
            drawn.values = new QLineEdit(integersText(branch.values), rows_);
            drawn.values->setObjectName(branchFieldName(index, "values"));
            drawn.values->setPlaceholderText(QStringLiteral("0"));
            drawn.values->setEnabled(editable && branch.when != "other");
            grid->addWidget(drawn.when, row, 0);
            grid->addWidget(drawn.values, row, 1);
            connect(drawn.when, &QComboBox::currentIndexChanged, this,
                    [this, index] { commitBranchWhen(index); });
            connect(drawn.values, &QLineEdit::returnPressed, this,
                    [this, index] { commitBranchWhen(index); });
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
            if (data == QLatin1String{"nothing"}) {
                after(model_.setLoopBranchBody(loop_, index, std::nullopt));
            } else if (data == QLatin1String{"new"}) {
                // The name is proposed, not asked for: the definition can be
                // renamed later, and a new body is only useful once it is
                // there to be opened.
                after(model_.newLoopBranchDefinition(
                    loop_, index, model_.suggestDefinitionName(loop_ + "_body")));
            } else {
                after(model_.setLoopBranchBody(loop_, index, data.toStdString()));
            }
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

void GraphCanvasLoopFields::commitWhere() {
    if (!current_ || loop_.empty()) return;
    const auto kind = where_kind_->currentData().toString().toStdString();
    where_values_->setEnabled(model_.canEdit() && !kind.empty());
    if (kind.empty()) {
        after(model_.setLoopWhere(loop_, {}, {}));
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
    // material = <key>: the materials the report names, all pages read.
    label(tr("material"));
    drawn.material = new QComboBox(rows_);
    drawn.material->setObjectName(branchFieldName(index, "material"));
    drawn.material->addItem(tr("(any)"), QString{});
    for (const auto &material : loop.materials)
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
    fillAnyYesNo(*drawn.has_variant, when.has_variant);
    line->addWidget(drawn.has_variant);
    for (QWidget *field : std::initializer_list<QWidget *>{
             drawn.material, drawn.tags, drawn.contract, drawn.skinned, drawn.has_variant})
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
    tags.include = parseNames(row.tags->text());
    if (!tags.empty()) when.tags = std::move(tags);
    if (const auto contract = row.contract->currentData().toString(); !contract.isEmpty())
        when.contract = contract.toStdString();
    when.skinned = readAnyYesNo(*row.skinned);
    when.has_variant = readAnyYesNo(*row.has_variant);
    if (when.empty()) {
        // Nothing left to test would make this row a second else.
        row.material->setToolTip(tr("Pick what this branch is for: only the last row has no "
                                    "condition."));
        return;
    }
    after(model_.setLoopBranchDrawWhen(loop_, branch, when));
}

void GraphCanvasLoopFields::commitWhereTags() {
    if (!current_ || loop_.empty() || !current_->draw_items) return;
    GraphCanvasTagFilter tags;
    tags.include = parseNames(tags_include_->text());
    tags.exclude = parseNames(tags_exclude_->text());
    tags.require_all = parseNames(tags_require_all_->text());
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
        after(model_.setDrawVariant(node_, variant_->currentText().trimmed().toStdString()));
    };
    connect(variant_, &QComboBox::activated, this, commit_variant);
    connect(variant_->lineEdit(), &QLineEdit::returnPressed, this, commit_variant);

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
    override_form->addRow(caption(tr("Cull"), override_rows_), cull_);
    override_form->addRow(caption(tr("Depth test"), override_rows_), depth_test_);
    override_form->addRow(caption(tr("Depth write"), override_rows_), depth_write_);
    override_form->addRow(caption(tr("Blend"), override_rows_), blend_);
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
    for (QComboBox *combo : {cull_, depth_test_, depth_write_, blend_})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { commitOverride(); });
    connect(apply_defines_, &QPushButton::clicked, this, [this] { commitOverride(); });
    hide();
}

void GraphCanvasDrawFields::after(bool submitted) {
    if (submitted && submitted_) submitted_();
}

void GraphCanvasDrawFields::showNode(const GraphCanvasNode *node) {
    const bool shown = node != nullptr && (node->op == "render.material_pass" ||
                                           node->op == "draw.variant" || node->op == "draw.override");
    if (!shown) {
        node_.clear();
        op_.clear();
        hide();
        return;
    }
    const bool same = node_ == node->id && op_ == node->op;
    node_ = node->id;
    op_ = node->op;
    const bool editable = model_.canEdit();
    const auto &parameters = node->parameters;
    const auto string = [&](const nlohmann::json &object, const char *key) {
        const auto found = object.is_object() ? object.find(key) : object.end();
        return object.is_object() && found != object.end() && found->is_string()
                   ? qtext(found->get<std::string>())
                   : QString{};
    };
    contract_row_->setVisible(op_ == "render.material_pass");
    variant_row_->setVisible(op_ == "draw.variant");
    override_rows_->setVisible(op_ == "draw.override");
    if (op_ == "render.material_pass") {
        heading_->setText(tr("Material pass %1").arg(qtext(node_)));
        const QSignalBlocker block{contract_};
        const auto contract = string(parameters, "material_contract");
        if (contract_->findData(contract) < 0 && !contract.isEmpty())
            contract_->addItem(contract, contract);
        contract_->setCurrentIndex(contract_->findData(contract));
        contract_->setEnabled(editable);
    } else if (op_ == "draw.variant") {
        heading_->setText(tr("Draw %1 with a named variant").arg(qtext(node_)));
        if (!(same && variant_->lineEdit()->hasFocus())) {
            const QSignalBlocker block{variant_};
            variant_->clear();
            const auto suggested = qtext(model_.suggestDrawVariant());
            if (suggested != QLatin1String{"variant"}) variant_->addItem(suggested);
            const auto current = string(parameters, "variant");
            if (variant_->findText(current) < 0) variant_->addItem(current);
            variant_->setCurrentIndex(variant_->findText(current));
            variant_->setEditText(current);
        }
        variant_->setEnabled(editable);
    } else {
        heading_->setText(tr("Draw %1 with another render state").arg(qtext(node_)));
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
            combo->setEnabled(editable);
        };
        pick(cull_, "cull", false);
        pick(depth_test_, "depth_test", true);
        pick(depth_write_, "depth_write", true);
        pick(blend_, "blend", false);
        if (!(same && defines_->hasFocus())) {
            QStringList lines;
            if (parameters.is_object())
                for (const auto &define : parameters.value("defines", nlohmann::json::array()))
                    if (define.is_string()) lines << qtext(define.get<std::string>());
            const QSignalBlocker block{defines_};
            defines_->setPlainText(lines.join(QLatin1Char{'\n'}));
        }
        defines_->setEnabled(editable);
        apply_defines_->setEnabled(editable);
    }
    show();
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
    const auto defines = parseDefines(defines_->toPlainText());
    if (state.empty() && defines.empty()) {
        // The engine refuses an override that replaces nothing.
        cull_->setToolTip(tr("An override replaces at least one render state or adds a define; "
                             "draw.material draws the material as it is."));
        return;
    }
    after(model_.setDrawOverride(node_, state, defines));
}

void GraphCanvasLoopFields::commitBranchWhen(std::size_t branch) {
    if (!current_ || loop_.empty() || branch >= branch_rows_.size()) return;
    const auto &row = branch_rows_[branch];
    if (row.when == nullptr || row.values == nullptr) return;
    const auto when = row.when->currentData().toString().toStdString();
    if (when.empty()) return;
    auto values = parseIntegers(row.values->text());
    if (!values) {
        row.values->setToolTip(tr("Integers, separated by commas"));
        return;
    }
    // "element =" takes exactly one; the list keeps its first, or 0.
    if (when == "element") {
        const auto first = values->empty() ? std::int64_t{0} : values->front();
        values = std::vector<std::int64_t>{first};
    }
    after(model_.setLoopBranchWhen(loop_, branch, when, *values));
}

} // namespace PelicanStudio
