#include "graphcanvasloopfields.hpp"

#include "studiotheme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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
    for (const auto &branch : loop.branches)
        parts << qtext(branch.when) << GraphCanvasLoopFields::integersText(branch.values)
              << qtext(branch.body) << (branch.nothing ? QStringLiteral("0") : QStringLiteral("1"))
              << GraphCanvasLoopFields::integersText(branch.matched)
              << (branch.unmatched ? QStringLiteral("u") : QString{});
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
    connect(where_kind_, &QComboBox::currentIndexChanged, this, [this] { commitWhere(); });
    connect(where_values_, &QLineEdit::returnPressed, this, [this] { commitWhere(); });
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

    clearRows();
    const auto heading = [this](const QString &text) {
        auto *label = new QLabel(text, rows_);
        QFont font = studioSmallFont();
        font.setBold(true);
        label->setFont(font);
        rows_layout_->addWidget(label);
    };

    // Outputs: p (the last element's) and p[] (every element's), for each
    // output of the body.
    if (!loop.body_outputs.empty()) {
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

    // Carry: an output of one element handed to an input of the next.
    if (!loop.body_outputs.empty() && !loop.body_inputs.empty()) {
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
        const int row = static_cast<int>(index) * 2;
        BranchRow drawn;
        if (last) {
            auto *label = new QLabel(tr("else"), rows_);
            label->setObjectName(branchFieldName(index, "when"));
            grid->addWidget(label, row, 0, 1, 2);
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
        // Which elements took the branch, from the report.
        auto *matched = new QLabel(rows_);
        matched->setObjectName(branchFieldName(index, "matched"));
        matched->setFont(studioSmallFont());
        if (!loop.reported) {
            matched->setText(tr("elements: not reported yet"));
        } else if (branch.unmatched || branch.matched.empty()) {
            matched->setText(tr("no element takes this branch"));
            matched->setStyleSheet(QStringLiteral("color: #c0392b;"));
        } else {
            matched->setText(tr("elements: %1").arg(integersText(branch.matched)));
        }
        grid->addWidget(matched, row + 1, 0, 1, 6);
        branch_rows_.push_back(drawn);
    }
    rows_layout_->addLayout(grid);
    auto *add_branch = new QPushButton(tr("Add branch"), rows_);
    add_branch->setObjectName(fieldName(QStringLiteral("addBranch")));
    add_branch->setEnabled(editable);
    add_branch->setToolTip(tr("A branch in front of the else, for element 0 to begin with."));
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
