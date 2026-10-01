#include "graphcanvasfoldgroups.hpp"

#include "frameplanedit.hpp"
#include "graphcanvasdrawing.hpp"
#include "graphcanvaswidget.hpp"
#include "jsonrpcrequest.hpp"
#include "nodegraphics.hpp"
#include "studiotheme.hpp"

#include "../model/inspectormodel.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFontMetricsF>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace PelicanStudio {

namespace {

using Json = nlohmann::json;

QString qtext(const std::string &text) { return QString::fromStdString(text); }
QString qtext(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString kindOf(const QGraphicsItem &item) { return item.data(NodeGraphItemKindRole).toString(); }

std::string nameOf(const QGraphicsItem &item) {
    return item.data(NodeGraphNameRole).toString().toStdString();
}

// An item of the root scope on screen, not inside a loop's zone.
bool rootItem(const QGraphicsItem &item) {
    return item.data(GraphCanvasScopeRole).toString().isEmpty() &&
           item.data(GraphCanvasZoneScopeRole).toString().isEmpty();
}

void markTree(QGraphicsItem *item, const QString &group) {
    item->setData(GraphCanvasFoldGroupRole, group);
    for (QGraphicsItem *child : item->childItems()) markTree(child, group);
}

void tagFold(QGraphicsItem *item, const char *kind, const QString &group,
             const QString &name = {}) {
    item->setData(NodeGraphItemKindRole, QString::fromLatin1(kind));
    item->setData(NodeGraphNameRole, name.isEmpty() ? group : name);
    item->setData(GraphCanvasScopeRole, QString{});
    item->setData(GraphCanvasFoldGroupRole, group);
}

QPainterPath roundedRect(const QRectF &rect, qreal radius) {
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

const QColor &staleColour() {
    static const QColor colour{0xe6, 0x7e, 0x22};
    return colour;
}

// One line that crosses a group's edge: the member's end and the other one.
struct Crossing {
    std::string member;
    std::string member_port;
    std::string outside;
    std::string outside_port;
    // The member reads (the line comes in) rather than writes.
    bool into = false;

    [[nodiscard]] std::string row() const { return member + "." + member_port; }
};

QGraphicsItem *socketOf(const QList<QGraphicsItem *> &items, const std::string &node,
                        const std::string &port, bool output) {
    const QString wanted = output ? QStringLiteral("output") : QStringLiteral("input");
    for (QGraphicsItem *item : items) {
        const QString kind = kindOf(*item);
        if (kind != QLatin1String{GraphCanvasSocketItem} &&
            kind != QLatin1String{GraphCanvasDerivedSocketItem})
            continue;
        if (!rootItem(*item) || nameOf(*item) != node) continue;
        if (item->data(GraphCanvasSocketDirectionRole).toString() != wanted) continue;
        const auto bound = item->data(GraphCanvasBoundPortRole).toString().toStdString();
        if (item->data(GraphCanvasPortRole).toString().toStdString() == port || bound == port)
            return item;
    }
    return nullptr;
}

} // namespace

std::string graphFoldNodeId(const std::string &group) { return "fold:" + group; }

// --- The Decisions section.

GraphCanvasFoldFields::GraphCanvasFoldFields(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("pelican.graphCanvasFold"));
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 4, 0, 4);
    hide();
}

void GraphCanvasFoldFields::setEditable(bool editable) {
    editable_ = editable;
    if (body_ != nullptr) body_->setEnabled(editable);
}

void GraphCanvasFoldFields::showGroup(
    const GraphFoldGroup *group, const std::vector<GraphCanvasNode> &root,
    const std::map<std::string, std::int64_t, std::less<>> &loop_counts) {
    if (group == nullptr) {
        group_.reset();
        shown_.clear();
        hide();
        return;
    }
    // Drawn again only when what it shows changed: a redraw under an open
    // combo would take the choice out of the person's hand.
    std::string key = group->group + "|" + group->state + "|" + group->folded().dump() + "|";
    for (const auto &id : group->nodes) {
        key += id + ",";
        if (const auto count = loop_counts.find(id); count != loop_counts.end())
            key += std::to_string(count->second);
        for (const auto &node : root)
            if (node.id == id) {
                key += node.op;
                for (const auto &port : node.inputs)
                    key += ";" + port.port + "<" + port.source_node;
            }
    }
    // GL-4a, decision 5: what a verified group's decisions landed on.
    if (group->state == "verified" && applied)
        for (const auto field : graphFoldFields)
            for (const auto &subject : group->subjects(field)) {
                key += "|" + subject.text() + ">";
                for (const auto &name : applied(field, subject)) key += name + ",";
            }
    group_ = *group;
    root_ = root;
    loop_counts_ = loop_counts;
    show();
    if (key == shown_) return;
    shown_ = key;
    rebuild();
}

void GraphCanvasFoldFields::rebuild() {
    rows_.clear();
    verify_button_ = nullptr;
    quick_box_ = nullptr;
    if (body_ != nullptr) {
        layout_->removeWidget(body_);
        body_->deleteLater();
    }
    body_ = new QWidget(this);
    auto *body = new QVBoxLayout(body_);
    body->setContentsMargins(0, 0, 0, 0);
    layout_->addWidget(body_);
    const auto &group = *group_;

    auto *heading = new QLabel(tr("Decisions \xC2\xB7 %1").arg(qtext(group.group)), body_);
    heading->setObjectName(QStringLiteral("pelican.graphCanvasFold.heading"));
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);
    body->addWidget(heading);
    // GL-4a, decision 1: Verify, and Quick check beside it.
    auto *verify_row = new QWidget(body_);
    auto *verify_line = new QHBoxLayout(verify_row);
    verify_line->setContentsMargins(0, 0, 0, 0);
    verify_button_ = new QPushButton(verify_label_.isEmpty() ? tr("Verify") : verify_label_, verify_row);
    verify_button_->setObjectName(QStringLiteral("pelican.graphCanvasFold.verify"));
    verify_button_->setEnabled(verify_enabled_);
    verify_button_->setToolTip(verify_tooltip_);
    verify_line->addWidget(verify_button_);
    quick_box_ = new QCheckBox(tr("Quick check"), verify_row);
    quick_box_->setObjectName(QStringLiteral("pelican.graphCanvasFold.quickCheck"));
    quick_box_->setChecked(quick_);
    quick_box_->setToolTip(qtext(graphFoldQuickCheckTip));
    verify_line->addWidget(quick_box_);
    verify_line->addStretch(1);
    body->addWidget(verify_row);
    QObject::connect(verify_button_, &QPushButton::clicked, this, [this] {
        if (verify) verify();
    });
    QObject::connect(quick_box_, &QCheckBox::toggled, this, [this](bool on) { quick_ = on; });
    auto *note = new QLabel(qtext(graphFoldDecisionsNote), body_);
    note->setObjectName(QStringLiteral("pelican.graphCanvasFold.note"));
    note->setWordWrap(true);
    note->setFont(studioSmallFont());
    body->addWidget(note);
    auto *state = new QLabel(body_);
    state->setObjectName(QStringLiteral("pelican.graphCanvasFold.state"));
    state->setFont(studioSmallFont());
    state->setWordWrap(true);
    state->setText(group.stateLine().empty() ? tr("verified: the engine applies them")
                                             : qtext(group.stateLine()));
    state->setToolTip(qtext(group.stateTooltip()));
    if (group.stale()) state->setStyleSheet(QStringLiteral("color: #e67e22;"));
    body->addWidget(state);

    rows_.reserve(graphFoldFields.size());
    for (const auto field : graphFoldFields) {
        const QString key = qtext(graphFoldFieldKey(field));
        const QString prefix = QStringLiteral("pelican.graphCanvasFold.") + key;
        auto *title = new QLabel(qtext(graphFoldFieldTitle(field)), body_);
        title->setObjectName(prefix + QStringLiteral(".title"));
        title->setFont(bold);
        body->addWidget(title);
        const auto &subjects = group.subjects(field);
        if (subjects.empty()) {
            auto *none = new QLabel(tr("none"), body_);
            none->setObjectName(prefix + QStringLiteral(".none"));
            none->setFont(studioSmallFont());
            body->addWidget(none);
        }
        for (std::size_t index = 0; index < subjects.size(); ++index) {
            auto *row = new QWidget(body_);
            auto *line = new QHBoxLayout(row);
            line->setContentsMargins(8, 0, 0, 0);
            auto *text = new QLabel(qtext(subjects[index].text()), row);
            text->setObjectName(prefix + QStringLiteral(".row") + QString::number(index));
            line->addWidget(text, 1);
            auto *remove = new QToolButton(row);
            remove->setObjectName(prefix + QStringLiteral(".remove") + QString::number(index));
            remove->setText(QStringLiteral("\xC3\x97"));
            remove->setToolTip(tr("Take this decision out"));
            line->addWidget(remove);
            body->addWidget(row);
            // GL-4a, decision 5: where a verified group's decision landed.
            if (group.state == "verified" && applied) {
                const auto names = applied(field, subjects[index]);
                QStringList listed;
                for (const auto &name : names) listed.push_back(qtext(name));
                auto *where = new QLabel(
                    names.empty() ? qtext(graphFoldNoMatchLine)
                                  : tr("applied \xE2\x86\x92 %1").arg(listed.join(QStringLiteral(", "))),
                    body_);
                where->setObjectName(prefix + QStringLiteral(".applied") + QString::number(index));
                where->setFont(studioSmallFont());
                where->setContentsMargins(16, 0, 0, 0);
                where->setWordWrap(true);
                if (names.empty()) where->setStyleSheet(QStringLiteral("color: gray;"));
                body->addWidget(where);
            }
            QObject::connect(remove, &QToolButton::clicked, this, [this, field, index] {
                if (!group_) return;
                auto edited = *group_;
                auto &list = edited.subjects(field);
                if (index >= list.size()) return;
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                if (changed) changed(edited);
            });
        }
        AddRow add_row;
        add_row.field = field;
        add_row.choices = graphFoldNodeChoices(group, field, root_, loop_counts_);
        auto *row = new QWidget(body_);
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(8, 0, 0, 0);
        add_row.node = new QComboBox(row);
        add_row.node->setObjectName(prefix + QStringLiteral(".node"));
        add_row.node->setToolTip(tr("The node of the group the decision is about"));
        for (const auto &choice : add_row.choices) add_row.node->addItem(qtext(choice.node));
        line->addWidget(add_row.node, 1);
        add_row.element = new QComboBox(row);
        add_row.element->setObjectName(prefix + QStringLiteral(".element"));
        add_row.element->setToolTip(tr("Every element of the loop, or one of them"));
        line->addWidget(add_row.element);
        add_row.port = new QComboBox(row);
        add_row.port->setObjectName(prefix + QStringLiteral(".port"));
        add_row.port->setToolTip(
            tr("The input whose image or buffer is not to share its memory"));
        line->addWidget(add_row.port);
        add_row.add = new QPushButton(tr("Add"), row);
        add_row.add->setObjectName(prefix + QStringLiteral(".add"));
        add_row.add->setEnabled(!add_row.choices.empty());
        if (add_row.choices.empty())
            add_row.add->setToolTip(tr("No node of the group can be named here"));
        line->addWidget(add_row.add);
        body->addWidget(row);
        rows_.push_back(std::move(add_row));
    }
    for (auto &row : rows_) {
        followNode(row);
        AddRow *held = &row;
        QObject::connect(row.node, &QComboBox::currentIndexChanged, this,
                         [this, held](int) { followNode(*held); });
        QObject::connect(row.add, &QPushButton::clicked, this, [this, held] { add(*held); });
    }
    body_->setEnabled(editable_);
}

void GraphCanvasFoldFields::setVerify(bool enabled, const QString &label, const QString &tooltip) {
    verify_enabled_ = enabled;
    verify_label_ = label;
    verify_tooltip_ = tooltip;
    if (verify_button_ == nullptr) return;
    verify_button_->setEnabled(enabled);
    verify_button_->setText(label);
    verify_button_->setToolTip(tooltip);
}

void GraphCanvasFoldFields::setQuickCheck(bool quick) {
    quick_ = quick;
    if (quick_box_ != nullptr) quick_box_->setChecked(quick);
}

void GraphCanvasFoldFields::followNode(AddRow &row) {
    const int index = row.node->currentIndex();
    row.element->clear();
    row.port->clear();
    if (index < 0 || static_cast<std::size_t>(index) >= row.choices.size()) {
        row.element->hide();
        row.port->hide();
        return;
    }
    const auto &choice = row.choices[static_cast<std::size_t>(index)];
    row.element->setVisible(choice.loop);
    if (choice.loop) {
        row.element->addItem(tr("All elements"));
        if (choice.count)
            for (std::int64_t element = 0; element < *choice.count; ++element)
                row.element->addItem(QString::number(element));
    }
    row.port->setVisible(choice.needs_port);
    for (const auto &port : choice.ports) row.port->addItem(qtext(port));
}

void GraphCanvasFoldFields::add(AddRow &row) {
    if (!group_) return;
    const int index = row.node->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= row.choices.size()) return;
    const auto &choice = row.choices[static_cast<std::size_t>(index)];
    GraphFoldSubject subject;
    subject.node = choice.node;
    if (choice.loop && row.element->currentIndex() > 0)
        subject.element = static_cast<std::int64_t>(row.element->currentIndex() - 1);
    if (choice.needs_port) {
        if (row.port->currentIndex() < 0) return;
        subject.port = row.port->currentText().toStdString();
    }
    auto edited = *group_;
    auto &list = edited.subjects(row.field);
    if (std::ranges::find(list, subject) != list.end()) return;
    list.push_back(std::move(subject));
    if (changed) changed(edited);
}

// --- The controller.

GraphCanvasFoldController::GraphCanvasFoldController(GraphCanvasReadCapability &graph,
                                                     GraphCanvasModel &model,
                                                     GraphCanvasScene &scene, QWidget &owner,
                                                     Hooks hooks)
    : graph_{graph}, model_{model}, scene_{scene}, owner_{owner}, hooks_{std::move(hooks)} {
    fields_ = new GraphCanvasFoldFields(&owner);
    fields_->changed = [this](const GraphFoldGroup &edited) {
        // Not from inside the button's own signal: the fields are drawn again.
        QMetaObject::invokeMethod(
            &owner_, [this, edited] { writeDecisions(edited); }, Qt::QueuedConnection);
    };
    const auto named = [&](const char *name, const QString &text) {
        auto *action = new QAction(text, &owner_);
        action->setObjectName(QString::fromLatin1(name));
        owner_.addAction(action);
        return action;
    };
    fold_action_ = named("pelican.graphCanvas.foldIntoGroup", QObject::tr("Fold into group\xE2\x80\xA6"));
    fold_action_->setToolTip(QObject::tr(
        "Declare the picked root nodes one fold group, named for you; its decisions are "
        "chosen in the side fields."));
    QObject::connect(fold_action_, &QAction::triggered, &owner_, [this] { foldSelection(); });
    add_selected_action_ = named("pelican.graphCanvas.foldAddSelected",
                                 QObject::tr("Add selected nodes"));
    QObject::connect(add_selected_action_, &QAction::triggered, &owner_,
                     [this] { addSelectedNodes(selected_); });
    unfold_action_ = named("pelican.graphCanvas.unfoldGroup", QObject::tr("Unfold (delete group)"));
    QObject::connect(unfold_action_, &QAction::triggered, &owner_, [this] { askUnfold(selected_); });
    toggle_action_ = named("pelican.graphCanvas.toggleFoldGroup", QObject::tr("Fold / Open"));
    QObject::connect(toggle_action_, &QAction::triggered, &owner_, [this] {
        if (!selected_.empty()) setDisplayFolded(selected_, !displayFolded(selected_));
    });
    fields_->verify = [this] {
        QMetaObject::invokeMethod(
            &owner_, [this] { verify(selected_); }, Qt::QueuedConnection);
    };
    fields_->applied = [this](GraphFoldField field, const GraphFoldSubject &subject) {
        return graphFoldAppliedNames(
            selected_, field, subject, model_.planFoldDecisions(), model_.planPasses(),
            [this](const std::string &node, const std::string &port) {
                return model_.outputResource(node, port);
            });
    };
    verify_clock_ = new QTimer(&owner_);
    verify_clock_->setInterval(1000);
    QObject::connect(verify_clock_, &QTimer::timeout, &owner_, [this] { tickVerify(); });
    redraw_timer_ = new QTimer(&owner_);
    redraw_timer_->setSingleShot(true);
    redraw_timer_->setInterval(0);
    QObject::connect(redraw_timer_, &QTimer::timeout, &owner_, [this] { redraw(); });

    scene_.foldItemPressed = [this](QGraphicsItem &item, Qt::MouseButton button) {
        return pressed(item, button);
    };
    scene_.foldItemDoubleClicked = [this](QGraphicsItem &item) { return doubleClicked(item); };
    scene_.foldItemMenu = [this](QGraphicsItem &item, const QPoint &screen) {
        return menu(item, screen);
    };
    scene_.foldMenuEntries = [this](QMenu &menu) { menuEntries(menu); };
}

GraphCanvasFoldController::~GraphCanvasFoldController() {
    scene_.foldItemPressed = {};
    scene_.foldItemDoubleClicked = {};
    scene_.foldItemMenu = {};
    scene_.foldMenuEntries = {};
}

bool GraphCanvasFoldController::onRealRoot() const {
    return model_.currentScope().empty() && !model_.foldedView();
}

const GraphFoldGroup *GraphCanvasFoldController::find(const std::string &group) const {
    const auto found = std::ranges::find(groups_, group, &GraphFoldGroup::group);
    return found != groups_.end() ? &*found : nullptr;
}

std::map<std::string, std::int64_t, std::less<>> GraphCanvasFoldController::loopCounts() const {
    std::map<std::string, std::int64_t, std::less<>> counts;
    for (const auto &node : model_.nodes({}))
        if (node.loop)
            if (const auto loop = model_.loop(node.id)) {
                if (loop->known_count) counts[node.id] = *loop->known_count;
                else if (loop->reported) counts[node.id] = loop->reported_count;
            }
    return counts;
}

void GraphCanvasFoldController::say(bool error, const std::string &line) {
    model_.setEditNotice({error ? InspectorNoticeKind::Error : InspectorNoticeKind::Information, line});
    if (hooks_.notice) hooks_.notice();
}

// --- Reading.

void GraphCanvasFoldController::follow() {
    // A refusal for a moved document: once the document read after it is
    // here, the gesture goes once more.
    if (retry_ && retry_after_ && model_.digest() != *retry_after_ && !busy()) {
        auto retry = std::exchange(retry_, {});
        retry_after_.reset();
        retry();
        retry_ = {};
    }
    if (!graph_.requestFoldGroups || !model_.connected() || !model_.contextReceived() ||
        !model_.graphRoot() || model_.digest().empty())
        return;
    if (model_.digest() == read_digest_) return;
    if (groups_request_ != 0 || root_request_ != 0) {
        reread_ = true;
        return;
    }
    QString error;
    groups_request_ = graph_.requestFoldGroups(&error);
    if (groups_request_ != 0) read_digest_ = model_.digest();
}

void GraphCanvasFoldController::readGroups(const Json &report) {
    auto groups = parseGraphFoldGroupsReport(report);
    // The decisions are kept from the last read until the root's arrive.
    for (auto &group : groups)
        if (const auto *old = find(group.group))
            for (const auto field : graphFoldFields) group.subjects(field) = old->subjects(field);
    groups_ = std::move(groups);
    if (!selected_.empty() && find(selected_) == nullptr) selected_.clear();
    for (auto it = folded_display_.begin(); it != folded_display_.end();)
        it = find(*it) == nullptr ? folded_display_.erase(it) : std::next(it);
    if (!groups_.empty() && graph_.requestRootDocument && !model_.sourceReference().empty()) {
        QString error;
        root_request_ = graph_.requestRootDocument(qtext(model_.sourceReference()), &error);
    }
    scheduleRedraw();
    if (hooks_.details) hooks_.details();
}

void GraphCanvasFoldController::readRoot(const Json &response) {
    const auto documents = response.value("documents", Json::array());
    if (!documents.is_array()) return;
    for (const auto &document : documents) {
        if (!document.is_object() ||
            document.value("reference", std::string{}) != model_.sourceReference())
            continue;
        const auto content =
            Json::parse(document.value("content", std::string{}), nullptr, false);
        if (content.is_discarded()) return;
        readGraphFoldDecisions(groups_, content);
        if (hooks_.details) hooks_.details();
        return;
    }
}

bool GraphCanvasFoldController::deliverResult(qint64 id, const QByteArray &bytes) {
    if (id == 0) return false;
    // GL-4a: verify_fold's acceptance, and each get_edit_result after it.
    if (id == verify_request_ || id == verify_ticket_request_) {
        const bool submission = id == verify_request_;
        (submission ? verify_request_ : verify_ticket_request_) = 0;
        const auto response =
            Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
        const auto status = response.is_object() ? response.value("status", std::string{}) : std::string{};
        if (submission && status == "accepted" && response.contains("ticket") &&
            response.at("ticket").is_string()) {
            verify_ticket_ = qtext(response.at("ticket").get<std::string>());
            QTimer::singleShot(500, &owner_, [this] { requestVerifyTicket(); });
            return true;
        }
        if (!submission && (status == "running" || status == "pending")) {
            QTimer::singleShot(500, &owner_, [this] { requestVerifyTicket(); });
            return true;
        }
        verifyFinished(response);
        return true;
    }
    const auto parse = [&]() {
        return Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    };
    if (id == groups_request_) {
        groups_request_ = 0;
        if (const auto report = parse(); report.is_object()) readGroups(report);
        if (std::exchange(reread_, false)) read_digest_.clear();
        return true;
    }
    if (id == root_request_) {
        root_request_ = 0;
        if (const auto response = parse(); response.is_object()) readRoot(response);
        if (reread_) {
            reread_ = false;
            read_digest_.clear();
        }
        return true;
    }
    if (id != edit_request_ && id != ticket_request_) return false;
    const bool submission = id == edit_request_;
    (submission ? edit_request_ : ticket_request_) = 0;
    const auto response = parse();
    if (!response.is_object()) {
        ticket_.clear();
        say(true, "The engine's answer to fold_nodes could not be read.");
        return true;
    }
    const auto status = response.value("status", std::string{});
    if (submission) {
        const auto ticket = response.find("ticket");
        if (status != "accepted" || ticket == response.end() || !ticket->is_string()) {
            refused(response);
            return true;
        }
        ticket_ = qtext(ticket->get<std::string>());
        requestTicket();
        return true;
    }
    if (status == "pending") {
        QTimer::singleShot(25, &owner_, [this] { requestTicket(); });
        return true;
    }
    ticket_.clear();
    if (status != "committed" || !response.value("committed", false)) {
        refused(response);
        return true;
    }
    retry_ = {};
    retry_after_.reset();
    if (edit_deletes_) {
        if (selected_ == edit_group_) selected_.clear();
        folded_display_.erase(edit_group_);
    } else {
        selected_ = edit_group_;
    }
    read_digest_.clear();
    if (hooks_.committed) hooks_.committed(response, edit_line_);
    return true;
}

bool GraphCanvasFoldController::deliverFailure(qint64 id, const QString &message) {
    if (id == 0) return false;
    if (id == verify_request_ || id == verify_ticket_request_) {
        verify_request_ = 0;
        verify_ticket_request_ = 0;
        verify_ticket_.clear();
        const auto group = std::exchange(verifying_, {});
        verify_clock_->stop();
        say(true, group + ": " + message.toStdString());
        redraw();
        if (hooks_.details) hooks_.details();
        return true;
    }
    if (id == groups_request_ || id == root_request_) {
        (id == groups_request_ ? groups_request_ : root_request_) = 0;
        return true;
    }
    if (id != edit_request_ && id != ticket_request_) return false;
    edit_request_ = id == edit_request_ ? 0 : edit_request_;
    ticket_request_ = id == ticket_request_ ? 0 : ticket_request_;
    ticket_.clear();
    retry_ = {};
    say(true, message.toStdString());
    return true;
}

// --- Writing.

bool GraphCanvasFoldController::send(const std::string &group, std::vector<std::string> nodes,
                                     Json folded, std::string line) {
    if (busy()) {
        say(true, "Wait for the current change to finish.");
        return false;
    }
    // GL-4a, decision 2.
    if (!verifying_.empty() && group == verifying_) {
        say(true, group + " is being verified; edits wait until it finishes");
        return false;
    }
    if (!graph_.requestFoldNodes) {
        say(true, "This engine connection cannot write fold groups.");
        return false;
    }
    if (!model_.connected() || model_.digest().empty()) {
        say(true, "The Graph tab has not read the document yet.");
        return false;
    }
    QJsonObject params;
    try {
        params = exactJsonRpcObject(graphFoldNodesParams(model_.digest(), group, nodes, folded));
    } catch (const std::exception &error) {
        say(true, error.what());
        return false;
    }
    QString error;
    edit_request_ = graph_.requestFoldNodes(params, &error);
    if (edit_request_ == 0) {
        say(true, error.toStdString());
        return false;
    }
    edit_group_ = group;
    edit_deletes_ = folded.is_null();
    edit_line_ = std::move(line);
    retry_ = [this, group, nodes = std::move(nodes), folded = std::move(folded),
              line = edit_line_]() mutable {
        return send(group, std::move(nodes), std::move(folded), std::move(line));
    };
    say(false, group + ": saving the fold group...");
    return true;
}

void GraphCanvasFoldController::requestTicket() {
    if (ticket_.isEmpty() || ticket_request_ != 0) return;
    QString error;
    ticket_request_ = graph_.requestEditResult ? graph_.requestEditResult(ticket_, &error) : 0;
    if (ticket_request_ != 0) return;
    ticket_.clear();
    say(true, error.toStdString());
}

void GraphCanvasFoldController::refused(const Json &response) {
    const auto error = response.find("error");
    const auto &reason = error != response.end() && error->is_object() ? *error : response;
    const auto code = reason.value("code", std::string{});
    // The document moved since the tab read it: read it again, and send the
    // gesture once more from what arrives.
    if (code == "external_modification" && retry_ && !retry_after_) {
        retry_after_ = model_.digest();
        say(false, "The document changed since the Graph read it; reading it again...");
        model_.refresh();
        if (hooks_.dispatch) hooks_.dispatch();
        return;
    }
    retry_ = {};
    retry_after_.reset();
    // Decision 7: a fold refusal reads as Studio's sentence.
    auto line = graphLoopRefusalDescription(reason);
    if (line.empty()) line = engineRefusalLine(reason);
    if (line.empty()) line = response.dump();
    say(true, line);
}

bool GraphCanvasFoldController::foldSelection() {
    if (!onRealRoot()) {
        say(true, "Fold into group\xE2\x80\xA6 works on the root graph: go back to the root first.");
        return false;
    }
    const auto list = picks();
    if (const auto refusal = graphFoldPickRefusal(list); !refusal.empty()) {
        say(true, refusal);
        return false;
    }
    // In the document's order, whatever order they were picked in.
    const auto root = model_.nodes({});
    std::vector<std::string> nodes;
    for (const auto &node : root)
        if (std::ranges::any_of(list, [&](const GraphFoldPick &pick) { return pick.node == node.id; }))
            nodes.push_back(node.id);
    const auto name = suggestGraphFoldGroupName(groups_, nodes, root);
    std::string joined;
    for (const auto &node : nodes) joined += (joined.empty() ? "" : ", ") + node;
    return send(name, nodes, graphFoldNewFolded(),
                "Folded " + joined + " into " + name + " (not verified: it runs as it is).");
}

bool GraphCanvasFoldController::addSelectedNodes(const std::string &group) {
    const auto *found = find(group);
    if (found == nullptr) {
        say(true, "Pick a fold group first.");
        return false;
    }
    const auto list = picks();
    if (const auto refusal = graphFoldPickRefusal(list); !refusal.empty()) {
        say(true, refusal);
        return false;
    }
    auto nodes = found->nodes;
    std::size_t added = 0;
    for (const auto &pick : list)
        if (std::ranges::find(nodes, pick.node) == nodes.end()) {
            nodes.push_back(pick.node);
            ++added;
        }
    if (added == 0) {
        say(true, "The picked nodes are in " + group + " already.");
        return false;
    }
    return send(group, std::move(nodes), found->folded(),
                group + ": " + std::to_string(added) + (added == 1 ? " node" : " nodes") +
                    " added.");
}

bool GraphCanvasFoldController::removeNode(const std::string &group, const std::string &node) {
    const auto *found = find(group);
    if (found == nullptr || !found->contains(node)) return false;
    if (found->nodes.size() == 1) {
        say(true, group + " keeps one node at least: Unfold deletes the group.");
        return false;
    }
    auto edited = *found;
    std::erase(edited.nodes, node);
    // A decision about the node goes with it (the engine refuses a subject
    // outside the group).
    for (const auto field : graphFoldFields)
        std::erase_if(edited.subjects(field),
                      [&](const GraphFoldSubject &subject) { return subject.node == node; });
    return send(group, edited.nodes, edited.folded(), group + ": " + node + " taken out.");
}

void GraphCanvasFoldController::askUnfold(const std::string &group) {
    const auto *found = find(group);
    if (found == nullptr) {
        say(true, "Pick a fold group first.");
        return;
    }
    std::size_t decisions = 0;
    for (const auto field : graphFoldFields) decisions += found->subjects(field).size();
    auto *prompt = new QMessageBox(
        QMessageBox::Question, QObject::tr("Unfold"),
        QObject::tr("Unfold %1? The group and its %2 decision(s) are deleted; the nodes stay "
                    "where they are.")
            .arg(qtext(group))
            .arg(static_cast<qulonglong>(decisions)),
        QMessageBox::NoButton, &owner_);
    prompt->setObjectName(QStringLiteral("pelican.graphCanvasFold.unfoldConfirm"));
    auto *accept = prompt->addButton(QObject::tr("Unfold"), QMessageBox::AcceptRole);
    accept->setObjectName(QStringLiteral("pelican.graphCanvasFold.unfoldConfirm.ok"));
    auto *cancel = prompt->addButton(QMessageBox::Cancel);
    cancel->setObjectName(QStringLiteral("pelican.graphCanvasFold.unfoldConfirm.cancel"));
    prompt->setDefaultButton(cancel);
    prompt->setEscapeButton(cancel);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(prompt, &QMessageBox::buttonClicked, &owner_,
                     [this, accept, group](QAbstractButton *button) {
                         if (button != accept) {
                             say(false, group + " is left as it is.");
                             return;
                         }
                         unfold(group);
                     });
    prompt->open();
}

bool GraphCanvasFoldController::unfold(const std::string &group) {
    if (find(group) == nullptr) return false;
    return send(group, {}, Json(nullptr),
                "Unfolded " + group + ": the group and its decisions are gone; the nodes stay.");
}

bool GraphCanvasFoldController::writeDecisions(const GraphFoldGroup &edited) {
    if (find(edited.group) == nullptr) return false;
    return send(edited.group, edited.nodes, edited.folded(),
                edited.group + ": decisions saved (" + std::string{graphFoldUnverifiedLine} + ").");
}

bool GraphCanvasFoldController::setDisplayFolded(const std::string &group, bool folded) {
    if (find(group) == nullptr) return false;
    if (folded) folded_display_.insert(group);
    else folded_display_.erase(group);
    redraw();
    return true;
}

bool GraphCanvasFoldController::selectGroup(const std::string &group) {
    if (find(group) == nullptr) return false;
    scene_.clearPicks();
    selected_ = group;
    redraw();
    if (hooks_.details) hooks_.details();
    return true;
}

void GraphCanvasFoldController::nodePicked() {
    if (selected_.empty()) return;
    if (!scene_.selectedNode() && scene_.selectedItems().isEmpty()) return;
    selected_.clear();
    scheduleRedraw();
}

void GraphCanvasFoldController::syncFields() {
    const auto *group = onRealRoot() ? find(selected_) : nullptr;
    if (group != nullptr) {
        const bool running = !verifying_.empty();
        const bool stale = group->stale();
        fields_->setVerify(
            running ? false : group->state != "verified" && graph_.requestVerifyFold != nullptr,
            running && verifying_ == group->group
                ? qtext(graphFoldVerifyingLine(std::chrono::duration_cast<std::chrono::seconds>(
                                                   std::chrono::steady_clock::now() - verify_started_)
                                                   .count()))
                : stale ? QObject::tr("Verify again")
                        : QObject::tr("Verify"),
            stale ? qtext(graphFoldVerifyAgainTip)
                  : QObject::tr("Draw the group's open form twice and its decided form once in "
                                "the engine's own runs, and compare: the decisions apply once "
                                "they draw the same picture."));
    }
    fields_->showGroup(group, model_.nodes({}), loopCounts());
    fields_->setEditable(!busy() && model_.connected() &&
                         (group == nullptr || group->group != verifying_));
    add_selected_action_->setEnabled(group != nullptr);
    unfold_action_->setEnabled(group != nullptr);
    toggle_action_->setEnabled(group != nullptr);
}

std::vector<GraphFoldPick> GraphCanvasFoldController::picks() const {
    std::vector<GraphFoldPick> list;
    const auto add = [&](GraphFoldPick pick) {
        if (std::ranges::none_of(list, [&](const GraphFoldPick &other) {
                return other.node == pick.node && other.zone_scope == pick.zone_scope;
            }))
            list.push_back(std::move(pick));
    };
    for (QGraphicsItem *item : scene_.selectedItems()) {
        if (kindOf(*item) != QLatin1String{GraphCanvasNodeItem}) continue;
        if (item->data(GraphCanvasFoldGroupRole).isValid()) continue;
        GraphFoldPick pick;
        pick.node = nameOf(*item);
        pick.zone_scope = item->data(GraphCanvasZoneScopeRole).toString().toStdString();
        if (pick.zone_scope.empty())
            pick.zone_scope = item->data(GraphCanvasScopeRole).toString().toStdString();
        pick.derived = item->data(GraphCanvasDerivedRole).toBool();
        add(std::move(pick));
    }
    if (list.empty() && scene_.selectedNode()) {
        GraphFoldPick pick;
        pick.node = *scene_.selectedNode();
        pick.zone_scope = scene_.selectedScope();
        for (const auto &folded : model_.foldedNodes())
            if (folded.id == pick.node) pick.derived = true;
        add(std::move(pick));
    }
    return list;
}

// --- GL-4a: Verify.

bool GraphCanvasFoldController::verify(const std::string &group) {
    const auto *found = find(group);
    if (found == nullptr) {
        say(true, "Pick a fold group first.");
        return false;
    }
    if (!verifying_.empty()) {
        say(true, verifying_ + " is being verified; verify " + group + " after it.");
        return false;
    }
    if (busy()) {
        say(true, "Wait for the current change to finish.");
        return false;
    }
    if (!graph_.requestVerifyFold) {
        say(true, "This engine connection cannot verify a fold group.");
        return false;
    }
    if (!model_.connected() || model_.digest().empty()) {
        say(true, "The Graph tab has not read the document yet.");
        return false;
    }
    QJsonObject params;
    try {
        params = exactJsonRpcObject(
            graphFoldVerifyParams(model_.digest(), group, fields_->quickCheck()));
    } catch (const std::exception &error) {
        say(true, error.what());
        return false;
    }
    QString error;
    verify_request_ = graph_.requestVerifyFold(params, &error);
    if (verify_request_ == 0) {
        say(true, error.toStdString());
        return false;
    }
    verifying_ = group;
    verify_started_ = std::chrono::steady_clock::now();
    marks_.erase(group);
    verify_clock_->start();
    say(false, group + ": verifying (the engine draws it in runs of its own)...");
    redraw();
    if (hooks_.details) hooks_.details();
    return true;
}

void GraphCanvasFoldController::requestVerifyTicket() {
    if (verify_ticket_.isEmpty() || verify_ticket_request_ != 0) return;
    QString error;
    verify_ticket_request_ =
        graph_.requestEditResult ? graph_.requestEditResult(verify_ticket_, &error) : 0;
    if (verify_ticket_request_ != 0) return;
    verify_ticket_.clear();
    const auto group = std::exchange(verifying_, {});
    verify_clock_->stop();
    say(true, group + ": " + error.toStdString());
}

void GraphCanvasFoldController::verifyFinished(const Json &response) {
    verify_ticket_.clear();
    verify_clock_->stop();
    const auto group = std::exchange(verifying_, {});
    const auto outcome = readGraphFoldVerifyOutcome(response, group);
    if (outcome.kind == GraphFoldVerifyOutcome::Kind::passed) {
        marks_.erase(group);
        read_digest_.clear();
        // What any render edit of the tab does after its commit: the plan
        // is read again, and with it the decisions now applied.
        if (hooks_.committed) hooks_.committed(response, outcome.notice);
        redraw();
        return;
    }
    if (outcome.kind == GraphFoldVerifyOutcome::Kind::mismatch ||
        outcome.kind == GraphFoldVerifyOutcome::Kind::unstable) {
        const auto *found = find(group);
        marks_[group] = VerifyMark{outcome.kind, outcome.heading,
                                   found != nullptr ? found->digest : std::string{}};
    }
    say(true, outcome.notice);
    redraw();
    if (hooks_.details) hooks_.details();
}

void GraphCanvasFoldController::tickVerify() {
    if (verifying_.empty()) {
        verify_clock_->stop();
        return;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - verify_started_)
                             .count();
    const QString line = qtext(graphFoldVerifyingLine(seconds));
    for (QGraphicsItem *item : scene_.items())
        if (kindOf(*item) == QLatin1String{GraphCanvasFoldStateItem} &&
            nameOf(*item) == verifying_)
            if (auto *text = dynamic_cast<QGraphicsSimpleTextItem *>(item)) text->setText(line);
    if (selected_ == verifying_) fields_->setVerify(false, line, {});
}

std::string GraphCanvasFoldController::verifyingGroupOf(const std::string &node) const {
    if (verifying_.empty()) return {};
    const auto *group = find(verifying_);
    return group != nullptr && group->contains(node) ? verifying_ : std::string{};
}

void GraphCanvasFoldController::gateNodeFields(const std::vector<QWidget *> &fields) {
    const auto &picked = scene_.selectedNode();
    const auto group = picked && scene_.selectedScope().empty() && onRealRoot()
                           ? verifyingGroupOf(*picked)
                           : std::string{};
    if (group.empty()) {
        gated_node_.clear();
        return;
    }
    for (QWidget *field : fields)
        if (field != nullptr) field->setEnabled(false);
    if (gated_node_ == *picked) return;
    gated_node_ = *picked;
    say(false, group + " is being verified; edits wait until it finishes");
}

// --- The canvas.

void GraphCanvasFoldController::scheduleRedraw() {
    if (redraw_timer_ != nullptr) redraw_timer_->start();
}

void GraphCanvasFoldController::redraw() {
    // What the last draw put there, and what it hid.
    std::vector<QGraphicsItem *> old;
    for (QGraphicsItem *item : scene_.items()) {
        if (item->data(GraphCanvasFoldHiddenRole).toBool()) {
            item->setData(GraphCanvasFoldHiddenRole, QVariant{});
            item->setVisible(true);
        }
        if (item->parentItem() == nullptr && item->data(GraphCanvasFoldGroupRole).isValid())
            old.push_back(item);
    }
    for (QGraphicsItem *item : old) {
        // A press on one of them is still being answered: it goes on the
        // next trip through the event loop.
        if (scene_.mouseGrabberItem() == item) {
            scheduleRedraw();
            return;
        }
    }
    for (QGraphicsItem *item : old) {
        scene_.removeItem(item);
        delete item;
    }
    if (!onRealRoot() || groups_.empty()) return;

    const auto &theme = currentStudioTheme();
    const auto everything = scene_.items();
    // The root's node items by id (graph nodes, and the folded nodes a line
    // can come from).
    std::map<std::string, QGraphicsItem *> nodes;
    for (QGraphicsItem *item : everything)
        if (item->parentItem() == nullptr && kindOf(*item) == QLatin1String{GraphCanvasNodeItem} &&
            rootItem(*item))
            nodes[nameOf(*item)] = item;

    QFont heading_font = studioPanelFont();
    heading_font.setBold(true);
    const QFont small_font = studioSmallFont();
    const QFontMetricsF heading_metrics{heading_font};
    const QFontMetricsF small_metrics{small_font};
    GraphCanvasDrawing::Builder builder{scene_, QString{}};

    for (const auto &group : groups_) {
        std::vector<QGraphicsItem *> members;
        std::set<std::string> member_ids;
        for (const auto &id : group.nodes)
            if (const auto found = nodes.find(id);
                found != nodes.end() && !found->second->data(GraphCanvasDerivedRole).toBool()) {
                members.push_back(found->second);
                member_ids.insert(id);
            }
        if (members.empty()) continue;
        const QString name = qtext(group.group);
        QRectF box;
        for (QGraphicsItem *member : members) {
            const QRectF own =
                member->sceneBoundingRect().united(member->mapRectToScene(member->childrenBoundingRect()));
            box = box.isNull() ? own : box.united(own);
        }
        const bool folded = folded_display_.contains(group.group);
        if (folded) {
            // Decision 2: one folded node in their place, with a row for each
            // line that crosses the group's edge.
            std::vector<Crossing> crossings;
            for (QGraphicsItem *item : everything) {
                const QString kind = kindOf(*item);
                if (kind != QLatin1String{GraphCanvasEdgeItem} &&
                    kind != QLatin1String{GraphCanvasEdgeArrowItem} &&
                    kind != QLatin1String{GraphCanvasPassInputEdgeItem} &&
                    kind != QLatin1String{GraphCanvasDerivedEdgeItem} &&
                    kind != QLatin1String{GraphCanvasRerouteItem})
                    continue;
                if (!rootItem(*item) || item->parentItem() != nullptr) continue;
                const auto from = item->data(NodeGraphFromNameRole).toString().toStdString();
                const auto to = kind == QLatin1String{GraphCanvasRerouteItem}
                                    ? nameOf(*item)
                                    : item->data(NodeGraphToNameRole).toString().toStdString();
                const bool from_inside = member_ids.contains(from);
                const bool to_inside = member_ids.contains(to);
                if (!from_inside && !to_inside) continue;
                item->setData(GraphCanvasFoldHiddenRole, true);
                item->setVisible(false);
                if (from_inside == to_inside || kind == QLatin1String{GraphCanvasEdgeArrowItem} ||
                    kind == QLatin1String{GraphCanvasRerouteItem})
                    continue;
                Crossing crossing;
                crossing.into = to_inside;
                crossing.member = to_inside ? to : from;
                crossing.member_port =
                    item->data(to_inside ? GraphCanvasToPortRole : GraphCanvasFromPortRole)
                        .toString()
                        .toStdString();
                crossing.outside = to_inside ? from : to;
                crossing.outside_port =
                    item->data(to_inside ? GraphCanvasFromPortRole : GraphCanvasToPortRole)
                        .toString()
                        .toStdString();
                crossings.push_back(std::move(crossing));
            }
            for (QGraphicsItem *member : members) {
                member->setData(GraphCanvasFoldHiddenRole, true);
                member->setVisible(false);
            }
            GraphCanvasFoldedNode node;
            node.id = graphFoldNodeId(group.group);
            node.label = group.group;
            node.type = "fold group";
            node.source = "project";
            for (const auto &crossing : crossings) {
                auto &rows = crossing.into ? node.reads : node.writes;
                if (std::ranges::find(rows, crossing.row()) == rows.end()) rows.push_back(crossing.row());
            }
            QGraphicsItem *item = builder.addDerivedNode(node, box.topLeft(), [](const QPointF &) {});
            item->setFlag(QGraphicsItem::ItemIsMovable, false);
            item->setFlag(QGraphicsItem::ItemIsSelectable, false);
            markTree(item, name);
            const QString tip =
                QObject::tr("Fold group %1: %2 nodes, drawn folded (only how it is looked at).\n"
                            "Double-click or Open shows them again.")
                    .arg(name)
                    .arg(static_cast<qulonglong>(group.nodes.size()));
            item->setToolTip(tip);
            for (QGraphicsItem *child : item->childItems())
                if (child->toolTip().isEmpty() || kindOf(*child) != QLatin1String{GraphCanvasDerivedSocketItem})
                    child->setToolTip(tip);
            box = item->sceneBoundingRect();
            // The lines to its rows, from the sockets they left or reach.
            const auto rows = item->childItems();
            for (const auto &crossing : crossings) {
                QPointF inner;
                bool found_row = false;
                for (QGraphicsItem *child : rows)
                    if (kindOf(*child) == QLatin1String{GraphCanvasDerivedSocketItem} &&
                        child->data(GraphCanvasPortRole).toString().toStdString() == crossing.row()) {
                        inner = child->sceneBoundingRect().center();
                        found_row = true;
                    }
                const auto outside = nodes.find(crossing.outside);
                if (!found_row || outside == nodes.end()) continue;
                QPointF outer;
                if (QGraphicsItem *socket = socketOf(everything, crossing.outside,
                                                     crossing.outside_port, crossing.into))
                    outer = socket->sceneBoundingRect().center();
                else {
                    const QRectF rect = outside->second->sceneBoundingRect();
                    outer = crossing.into ? QPointF{rect.right(), rect.center().y()}
                                          : QPointF{rect.left(), rect.center().y()};
                }
                const auto geometry = crossing.into ? curveGeometryBetween(outer, inner)
                                                    : curveGeometryBetween(inner, outer);
                auto *link = new QGraphicsPathItem(geometry.path);
                scene_.addItem(link);
                link->setZValue(0.0);
                QPen pen{theme.canvas.derived_border, 1.4};
                pen.setStyle(Qt::DashLine);
                link->setPen(pen);
                link->setAcceptedMouseButtons(Qt::NoButton);
                tagFold(link, GraphCanvasFoldLinkItem, name,
                        name + QStringLiteral("/") + qtext(crossing.row()));
            }
        }

        // The heading: the name, the state's words, Verify, and Fold / Open.
        // GL-4a: while a verify runs its seconds; a difference it found
        // (red: the decisions; yellow: the open form itself) in place of
        // the state's words.
        const bool running = verifying_ == group.group;
        const VerifyMark *found_mark = nullptr;
        if (const auto mark = marks_.find(group.group);
            mark != marks_.end() && mark->second.digest == group.digest && group.state != "verified")
            found_mark = &mark->second;
        QString state = qtext(group.stateLine());
        QColor mark_colour = staleColour();
        bool marked = group.stale();
        QString mark_tip = qtext(group.stateTooltip());
        if (running) {
            state = qtext(graphFoldVerifyingLine(std::chrono::duration_cast<std::chrono::seconds>(
                                                     std::chrono::steady_clock::now() - verify_started_)
                                                     .count()));
            marked = false;
        } else if (found_mark != nullptr) {
            state = qtext(found_mark->line);
            marked = true;
            mark_colour = found_mark->kind == GraphFoldVerifyOutcome::Kind::mismatch
                              ? QColor{0xd9, 0x4c, 0x3d}
                              : QColor{0xe5, 0xc0, 0x30};
            mark_tip = found_mark->kind == GraphFoldVerifyOutcome::Kind::mismatch
                           ? QObject::tr("The decided form draws differently: the decisions do not apply.")
                           : QObject::tr("Two runs of the open form drew differently, so the decisions "
                                         "could not be judged. Clear Quick check (a longer warm-up) "
                                         "and verify again.");
        }
        const QString toggle_text = folded ? QObject::tr("Open") : QObject::tr("Fold");
        const QString verify_text = group.stale() ? QObject::tr("Verify again") : QObject::tr("Verify");
        const bool offers_verify =
            !running && group.state != "verified" && graph_.requestVerifyFold != nullptr;
        const qreal mark_width = marked ? 14.0 : 0.0;
        const qreal toggle_width = small_metrics.horizontalAdvance(toggle_text) + 18.0;
        const qreal verify_width =
            offers_verify ? small_metrics.horizontalAdvance(verify_text) + 18.0 + 6.0 : 0.0;
        const qreal heading_width = 12.0 + heading_metrics.horizontalAdvance(name) + 10.0 +
                                    mark_width +
                                    (state.isEmpty() ? 0.0 : small_metrics.horizontalAdvance(state) + 10.0) +
                                    verify_width + toggle_width + 10.0;
        QRectF frame = box.adjusted(-GraphCanvasFoldPadding,
                                    -GraphCanvasFoldPadding - GraphCanvasFoldHeadingHeight,
                                    GraphCanvasFoldPadding, GraphCanvasFoldPadding);
        if (frame.width() < heading_width) frame.setWidth(heading_width);
        const bool picked = selected_ == group.group;

        auto *outline = new QGraphicsPathItem(roundedRect(QRectF{QPointF{}, frame.size()}, 10.0));
        scene_.addItem(outline);
        outline->setPos(frame.topLeft());
        outline->setZValue(-3.0);
        QColor fill = theme.canvas.tab_selected;
        fill.setAlphaF(0.12F);
        outline->setBrush(fill);
        QPen frame_pen{picked ? theme.canvas.edge_selected : theme.canvas.derived_border,
                       picked ? 2.0 : 1.2};
        outline->setPen(frame_pen);
        outline->setAcceptedMouseButtons(Qt::NoButton);
        tagFold(outline, GraphCanvasFoldGroupItem, name);
        outline->setData(GraphCanvasLabelRole, name);

        QPainterPath band_path;
        band_path.addRoundedRect(QRectF{0.0, 0.0, frame.width(), GraphCanvasFoldHeadingHeight}, 10.0, 10.0);
        auto *band = new QGraphicsPathItem(band_path);
        scene_.addItem(band);
        band->setPos(frame.topLeft());
        band->setZValue(1.6);
        QColor band_fill = theme.canvas.tab_selected;
        band_fill.setAlphaF(picked ? 0.55F : 0.30F);
        band->setBrush(band_fill);
        band->setPen(Qt::NoPen);
        band->setCursor(Qt::PointingHandCursor);
        band->setToolTip(QObject::tr("Fold group %1: click to choose its decisions; right-click "
                                     "for its nodes and Unfold.")
                             .arg(name));
        tagFold(band, GraphCanvasFoldHeadingItem, name);
        band->setData(GraphCanvasLabelRole, name);
        auto *title = new QGraphicsSimpleTextItem(name, band);
        title->setFont(heading_font);
        title->setBrush(theme.canvas.heading_text);
        title->setPos(12.0, (GraphCanvasFoldHeadingHeight - heading_metrics.height()) / 2.0);
        tagFold(title, GraphCanvasFoldHeadingItem, name);
        title->setToolTip(band->toolTip());
        qreal cursor = 12.0 + heading_metrics.horizontalAdvance(name) + 10.0;
        if (marked) {
            // Rule 5: one mark, and only while it differs.
            auto *mark = new QGraphicsEllipseItem(0.0, 0.0, 9.0, 9.0, band);
            mark->setPos(cursor, (GraphCanvasFoldHeadingHeight - 9.0) / 2.0);
            mark->setBrush(mark_colour);
            mark->setPen(Qt::NoPen);
            mark->setToolTip(mark_tip);
            tagFold(mark, GraphCanvasFoldMarkItem, name);
            cursor += mark_width;
        }
        if (!state.isEmpty()) {
            auto *words = new QGraphicsSimpleTextItem(state, band);
            words->setFont(small_font);
            words->setBrush(marked ? QBrush{mark_colour} : QBrush{theme.canvas.dim_text});
            words->setPos(cursor, (GraphCanvasFoldHeadingHeight - small_metrics.height()) / 2.0);
            words->setToolTip(marked ? mark_tip : qtext(group.stateTooltip()));
            tagFold(words, GraphCanvasFoldStateItem, name);
        }
        const qreal toggle_height = GraphCanvasFoldHeadingHeight - 8.0;
        auto *toggle = new QGraphicsPathItem(
            roundedRect(QRectF{0.0, 0.0, toggle_width, toggle_height}, toggle_height / 2.0), band);
        toggle->setPos(frame.width() - toggle_width - 8.0, 4.0);
        toggle->setBrush(theme.canvas.pill_bg);
        toggle->setPen(QPen{theme.canvas.edge, 1.0});
        toggle->setCursor(Qt::PointingHandCursor);
        toggle->setToolTip(folded ? QObject::tr("Show the group's nodes again (only how it is looked at)")
                                  : QObject::tr("Draw the group as one folded node (only how it "
                                                "is looked at; the document does not change)"));
        tagFold(toggle, GraphCanvasFoldToggleItem, name);
        toggle->setData(GraphCanvasLabelRole, toggle_text);
        auto *toggle_label = new QGraphicsSimpleTextItem(toggle_text, toggle);
        toggle_label->setFont(small_font);
        toggle_label->setBrush(theme.canvas.heading_text);
        toggle_label->setPos(9.0, (toggle_height - small_metrics.height()) / 2.0);
        tagFold(toggle_label, GraphCanvasFoldToggleItem, name);
        toggle_label->setData(GraphCanvasLabelRole, toggle_text);
        toggle_label->setToolTip(toggle->toolTip());
        // GL-4a, decision 1 (and 4: Verify again on a stale group).
        if (offers_verify) {
            const qreal width = verify_width - 6.0;
            auto *verify = new QGraphicsPathItem(
                roundedRect(QRectF{0.0, 0.0, width, toggle_height}, toggle_height / 2.0), band);
            verify->setPos(frame.width() - toggle_width - 8.0 - 6.0 - width, 4.0);
            verify->setBrush(theme.canvas.pill_bg);
            verify->setPen(QPen{group.stale() ? staleColour() : theme.canvas.edge, 1.0});
            verify->setCursor(Qt::PointingHandCursor);
            verify->setToolTip(group.stale()
                                   ? qtext(graphFoldVerifyAgainTip)
                                   : QObject::tr("Check that the decisions draw the same picture "
                                                 "(the side fields' Quick check is used)"));
            tagFold(verify, GraphCanvasFoldVerifyItem, name);
            verify->setData(GraphCanvasLabelRole, verify_text);
            auto *verify_label = new QGraphicsSimpleTextItem(verify_text, verify);
            verify_label->setFont(small_font);
            verify_label->setBrush(theme.canvas.heading_text);
            verify_label->setPos(9.0, (toggle_height - small_metrics.height()) / 2.0);
            tagFold(verify_label, GraphCanvasFoldVerifyItem, name);
            verify_label->setData(GraphCanvasLabelRole, verify_text);
            verify_label->setToolTip(verify->toolTip());
        }
    }
}

bool GraphCanvasFoldController::pressed(QGraphicsItem &item, Qt::MouseButton button) {
    const auto group = item.data(GraphCanvasFoldGroupRole).toString().toStdString();
    if (group.empty()) return false;
    const QString kind = kindOf(item);
    if (kind == QLatin1String{GraphCanvasFoldGroupItem} || kind == QLatin1String{GraphCanvasFoldLinkItem})
        return false;
    if (button != Qt::LeftButton) return true;
    // From the event loop: the redraw deletes the item being pressed.
    if (kind == QLatin1String{GraphCanvasFoldVerifyItem}) {
        QMetaObject::invokeMethod(
            &owner_,
            [this, group] {
                selectGroup(group);
                verify(group);
            },
            Qt::QueuedConnection);
        return true;
    }
    if (kind == QLatin1String{GraphCanvasFoldToggleItem}) {
        QMetaObject::invokeMethod(
            &owner_,
            [this, group] {
                setDisplayFolded(group, !displayFolded(group));
                selectGroup(group);
            },
            Qt::QueuedConnection);
        return true;
    }
    QMetaObject::invokeMethod(&owner_, [this, group] { selectGroup(group); }, Qt::QueuedConnection);
    return true;
}

bool GraphCanvasFoldController::doubleClicked(QGraphicsItem &item) {
    const auto group = item.data(GraphCanvasFoldGroupRole).toString().toStdString();
    if (group.empty()) return false;
    const QString kind = kindOf(item);
    if (kind == QLatin1String{GraphCanvasFoldGroupItem} || kind == QLatin1String{GraphCanvasFoldLinkItem})
        return false;
    // Rule 6: a double-click on the folded node goes in -- its nodes are
    // shown again. On the heading it renames nothing.
    if (displayFolded(group) && kind != QLatin1String{GraphCanvasFoldToggleItem})
        QMetaObject::invokeMethod(
            &owner_,
            [this, group] {
                setDisplayFolded(group, false);
                selectGroup(group);
            },
            Qt::QueuedConnection);
    return true;
}

bool GraphCanvasFoldController::menu(QGraphicsItem &item, const QPoint &screen) {
    const auto group = item.data(GraphCanvasFoldGroupRole).toString().toStdString();
    if (group.empty()) return false;
    const QString kind = kindOf(item);
    if (kind == QLatin1String{GraphCanvasFoldGroupItem} || kind == QLatin1String{GraphCanvasFoldLinkItem})
        return false;
    groupMenu(group, screen);
    return true;
}

void GraphCanvasFoldController::groupMenu(const std::string &group, const QPoint &screen) {
    const auto *found = find(group);
    if (found == nullptr) return;
    selected_ = group;
    syncFields();
    QMenu menu;
    add_selected_action_->setEnabled(!busy());
    menu.addAction(add_selected_action_);
    QMenu *remove = menu.addMenu(QObject::tr("Remove node\xE2\x80\xA6"));
    remove->setObjectName(QStringLiteral("pelican.graphCanvasFold.removeNode"));
    for (const auto &node : found->nodes) {
        QAction *entry = remove->addAction(qtext(node));
        entry->setEnabled(found->nodes.size() > 1 && !busy());
        QObject::connect(entry, &QAction::triggered, &owner_, [this, group, node] {
            QMetaObject::invokeMethod(
                &owner_, [this, group, node] { removeNode(group, node); }, Qt::QueuedConnection);
        });
    }
    if (found->nodes.size() == 1)
        remove->setToolTip(QObject::tr("A group keeps one node at least: Unfold deletes it."));
    menu.addAction(toggle_action_);
    menu.addSeparator();
    unfold_action_->setEnabled(!busy());
    menu.addAction(unfold_action_);
    menu.exec(screen);
    scheduleRedraw();
    if (hooks_.details) hooks_.details();
}

void GraphCanvasFoldController::menuEntries(QMenu &menu) {
    if (!onRealRoot() || !graph_.requestFoldNodes) return;
    const auto refusal = graphFoldPickRefusal(picks());
    fold_action_->setEnabled(refusal.empty() && !busy());
    fold_action_->setStatusTip(qtext(refusal));
    menu.addSeparator();
    menu.addAction(fold_action_);
    if (!refusal.empty()) {
        // Decision 1: why, in one line, where the entry is.
        QAction *why = menu.addAction(qtext(refusal));
        why->setEnabled(false);
    }
}

} // namespace PelicanStudio
