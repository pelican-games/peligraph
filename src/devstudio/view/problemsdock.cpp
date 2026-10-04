#include "problemsdock.hpp"

#include "panelhelp.hpp"
#include "studiotheme.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString qtext(const std::string &value) { return QString::fromStdString(value); }

enum Column { SeverityColumn, ProblemColumn, DocumentColumn, LineColumn, PlaceColumn, ColumnCount };

// What a row is sorted by, beside its text.
constexpr int SeverityRankRole = Qt::UserRole + 2;
constexpr int LineRole = Qt::UserRole + 3;
constexpr int ColumnNumberRole = Qt::UserRole + 4;

// Sorted by severity (errors first), then document and line; by document,
// then line; by line as a number.
class ProblemItem final : public QTreeWidgetItem {
  public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override {
        const int column = treeWidget() != nullptr ? treeWidget()->sortColumn() : SeverityColumn;
        const auto rank = [](const QTreeWidgetItem &item) { return item.data(SeverityColumn, SeverityRankRole).toInt(); };
        const auto line = [](const QTreeWidgetItem &item) { return item.data(LineColumn, LineRole).toLongLong(); };
        const auto at = [](const QTreeWidgetItem &item) {
            return item.data(LineColumn, ColumnNumberRole).toLongLong();
        };
        const auto place = [&](const QTreeWidgetItem &a, const QTreeWidgetItem &b) {
            const int documents = QString::compare(a.text(DocumentColumn), b.text(DocumentColumn), Qt::CaseInsensitive);
            if (documents != 0) return documents < 0;
            if (line(a) != line(b)) return line(a) < line(b);
            return at(a) < at(b);
        };
        switch (column) {
        case SeverityColumn:
            if (rank(*this) != rank(other)) return rank(*this) < rank(other);
            return place(*this, other);
        case DocumentColumn:
            return place(*this, other);
        case LineColumn:
            if (line(*this) != line(other)) return line(*this) < line(other);
            return at(*this) < at(other);
        default:
            return QTreeWidgetItem::operator<(other);
        }
    }
};

} // namespace

struct ProblemsDock::Impl {
    ProblemsDock &owner;
    Hooks hooks;
    ProblemsSession session;
    QComboBox *severity = nullptr;
    QLineEdit *filter = nullptr;
    QLabel *count = nullptr;
    QTreeWidget *tree = nullptr;
    QLabel *detail = nullptr;
    QPushButton *restart = nullptr;
    QLabel *notice = nullptr;
    QTimer *poll = nullptr;
    bool engine = false;
    // A get_status was read since the last tick of the poll.
    bool status_seen = false;
    // The row a click opened last, and when: a double-click is one opening.
    QString opened_id;
    qint64 opened_at = 0;

    Impl(ProblemsDock &dock, Hooks dock_hooks)
        : owner{dock}, hooks{std::move(dock_hooks)},
          session{ProblemsSession::Transport{
              .request = [this](const std::string &method, const Json &params, std::string *error) -> std::int64_t {
                  if (!hooks.send) return 0;
                  const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(params.dump())).object();
                  QString message;
                  const qint64 id = hooks.send(qtext(method), object, &message);
                  if (id == 0 && error != nullptr) *error = message.toStdString();
                  return id;
              },
          }} {
        session.changed = [this] { present(); };
        build();
    }

    void build() {
        owner.setFont(studioPanelFont());
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        auto *top = new QHBoxLayout;
        severity = new QComboBox(&owner);
        severity->setObjectName(QStringLiteral("pelican.problems.severity"));
        severity->addItem(owner.tr("All"));
        severity->addItem(owner.tr("Errors"));
        severity->addItem(owner.tr("Warnings"));
        severity->setToolTip(owner.tr("Show every problem, or only the errors or the warnings"));
        top->addWidget(severity);
        filter = new QLineEdit(&owner);
        filter->setObjectName(QStringLiteral("pelican.problems.filter"));
        filter->setPlaceholderText(owner.tr("Filter (words of the problem, the document or the code)"));
        filter->setClearButtonEnabled(true);
        top->addWidget(filter, 1);
        count = new QLabel(&owner);
        count->setObjectName(QStringLiteral("pelican.problems.count"));
        top->addWidget(count);
        layout->addLayout(top);

        tree = new QTreeWidget(&owner);
        tree->setObjectName(QStringLiteral("pelican.problems.list"));
        tree->setColumnCount(ColumnCount);
        tree->setHeaderLabels({owner.tr("Severity"), owner.tr("Problem"), owner.tr("Document"),
                               owner.tr("Line"), owner.tr("Fix in")});
        tree->setRootIsDecorated(false);
        tree->setUniformRowHeights(true);
        tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tree->setSelectionMode(QAbstractItemView::SingleSelection);
        tree->setSortingEnabled(true);
        tree->sortByColumn(SeverityColumn, Qt::AscendingOrder);
        tree->header()->setSectionResizeMode(SeverityColumn, QHeaderView::ResizeToContents);
        tree->header()->setSectionResizeMode(ProblemColumn, QHeaderView::Stretch);
        tree->header()->setSectionResizeMode(DocumentColumn, QHeaderView::ResizeToContents);
        tree->header()->setSectionResizeMode(LineColumn, QHeaderView::ResizeToContents);
        tree->header()->setSectionResizeMode(PlaceColumn, QHeaderView::ResizeToContents);
        tree->header()->setStretchLastSection(false);
        tree->setToolTip(owner.tr("Click a problem to go where it is fixed"));
        layout->addWidget(tree, 1);

        auto *bottom = new QHBoxLayout;
        detail = new QLabel(&owner);
        detail->setObjectName(QStringLiteral("pelican.problems.detail"));
        applyPanelNoticeStyle(detail);
        detail->hide();
        bottom->addWidget(detail, 1);
        restart = new QPushButton(owner.tr("Restart engine"), &owner);
        restart->setObjectName(QStringLiteral("pelican.problems.restart"));
        restart->hide();
        bottom->addWidget(restart);
        layout->addLayout(bottom);
        notice = new QLabel(&owner);
        notice->setObjectName(QStringLiteral("pelican.problems.notice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        layout->addWidget(notice);

        QObject::connect(severity, &QComboBox::currentIndexChanged, &owner, [this](int) { applyFilter(); });
        QObject::connect(filter, &QLineEdit::textChanged, &owner, [this](const QString &) { applyFilter(); });
        QObject::connect(tree, &QTreeWidget::currentItemChanged, &owner,
                         [this](QTreeWidgetItem *, QTreeWidgetItem *) { presentDetail(); });
        // On the press, as a link is: the row's detail appears under the list
        // as it is selected, the list's height changes with it, and Qt's
        // click (press and release on one row) did not reliably follow.
        QObject::connect(tree, &QTreeWidget::itemPressed, &owner,
                         [this](QTreeWidgetItem *item, int) { openItem(item); });
        // The keyboard's Return (and a double-click, which was a click first).
        QObject::connect(tree, &QTreeWidget::itemActivated, &owner,
                         [this](QTreeWidgetItem *item, int) { openItem(item); });
        QObject::connect(restart, &QPushButton::clicked, &owner, [this] {
            const auto *row = currentRow();
            if (row == nullptr || row->place != ProblemRow::Place::Restart || !hooks.restart) return;
            hooks.restart(owner.tr("the rendering setup turns on %1, which this engine was started without")
                              .arg(qtext(row->restart_feature)));
        });

        poll = new QTimer(&owner);
        poll->setInterval(StatusPollMs);
        QObject::connect(poll, &QTimer::timeout, &owner, [this] {
            if (engine && !std::exchange(status_seen, false) && hooks.read_status) hooks.read_status();
        });
        present();
    }

    const ProblemRow *currentRow() const {
        const auto *item = tree->currentItem();
        if (item == nullptr) return nullptr;
        return session.find(item->data(SeverityColumn, ProblemIdRole).toString().toStdString());
    }

    void openItem(QTreeWidgetItem *item) {
        if (item == nullptr) return;
        const auto id = item->data(SeverityColumn, ProblemIdRole).toString();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (id == opened_id && now - opened_at < 600) return;
        opened_id = id;
        opened_at = now;
        owner.openProblem(id);
    }

    bool shown(const ProblemRow &row) const {
        const int wanted = severity->currentIndex();
        if (wanted == 1 && !row.error()) return false;
        if (wanted == 2 && row.error()) return false;
        const auto words = filter->text().trimmed();
        if (words.isEmpty()) return true;
        for (const auto &text : {row.sentence, row.document, row.code, row.place_text, row.message})
            if (qtext(text).contains(words, Qt::CaseInsensitive)) return true;
        return false;
    }

    void applyFilter() {
        for (int index = 0; index < tree->topLevelItemCount(); ++index) {
            auto *item = tree->topLevelItem(index);
            const auto *row = session.find(item->data(SeverityColumn, ProblemIdRole).toString().toStdString());
            item->setHidden(row == nullptr || !shown(*row));
        }
        // A row the filter hid is not the one being read about.
        if (auto *current = tree->currentItem(); current != nullptr && current->isHidden()) {
            tree->clearSelection();
            tree->setCurrentItem(nullptr);
        }
        presentDetail();
    }

    void presentDetail() {
        const auto *row = currentRow();
        if (row == nullptr) {
            setPanelNotice(detail, {});
            restart->hide();
            return;
        }
        QString where = qtext(row->document);
        if (row->line > 0) where += QLatin1Char(':') + qtext(row->lineText());
        if (!row->node.empty())
            where += (where.isEmpty() ? QString{} : QStringLiteral(" ")) +
                     owner.tr("node %1").arg(qtext(row->definition.empty() ? row->node
                                                                          : row->definition + "/" + row->node));
        QString line = where.isEmpty() ? qtext(row->sentence)
                                       : where + QStringLiteral(" — ") + qtext(row->sentence);
        QString full = line;
        if (!row->message.empty() && row->message != row->sentence) full += QLatin1Char('\n') + qtext(row->message);
        if (!row->source.empty()) full += QLatin1Char('\n') + owner.tr("Met: %1").arg(qtext(row->source));
        setPanelNotice(detail, line, full);
        restart->setVisible(row->place == ProblemRow::Place::Restart && static_cast<bool>(hooks.restart));
    }

    void present() {
        const QString kept =
            tree->currentItem() != nullptr ? tree->currentItem()->data(SeverityColumn, ProblemIdRole).toString()
                                           : QString{};
        const QIcon error_icon = owner.style()->standardIcon(QStyle::SP_MessageBoxCritical);
        const QIcon warning_icon = owner.style()->standardIcon(QStyle::SP_MessageBoxWarning);
        QTreeWidgetItem *current = nullptr;
        {
            const QSignalBlocker block{tree};
            tree->setSortingEnabled(false);
            tree->clear();
            for (const auto &row : session.rows()) {
                auto *item = new ProblemItem(tree);
                item->setText(SeverityColumn, row.error() ? owner.tr("Error") : owner.tr("Warning"));
                item->setIcon(SeverityColumn, row.error() ? error_icon : warning_icon);
                item->setData(SeverityColumn, ProblemIdRole, qtext(row.id));
                item->setData(SeverityColumn, SeverityRankRole, row.error() ? 0 : 1);
                item->setText(ProblemColumn, qtext(row.sentence));
                item->setToolTip(ProblemColumn,
                                 row.message.empty() || row.message == row.sentence
                                     ? qtext(row.sentence)
                                     : qtext(row.sentence) + QLatin1Char('\n') + qtext(row.message));
                item->setText(DocumentColumn, qtext(row.document));
                item->setToolTip(DocumentColumn, qtext(row.document_kind.empty()
                                                           ? row.document
                                                           : row.document + " (" + row.document_kind + ")"));
                item->setText(LineColumn, qtext(row.lineText()));
                item->setData(LineColumn, LineRole, static_cast<qlonglong>(row.line));
                item->setData(LineColumn, ColumnNumberRole, static_cast<qlonglong>(row.column));
                item->setText(PlaceColumn, row.place == ProblemRow::Place::None ? QStringLiteral("—")
                                                                                : qtext(row.place_text));
                if (!kept.isEmpty() && qtext(row.id) == kept) current = item;
            }
            tree->setSortingEnabled(true);
        }
        // Decision 5: a row that went takes its selection with it.
        if (current != nullptr) {
            const QSignalBlocker block{tree};
            tree->setCurrentItem(current);
        } else {
            tree->clearSelection();
            tree->setCurrentItem(nullptr);
        }
        applyFilter();
        const auto errors = session.errors();
        const auto warnings = session.warnings();
        count->setText(qtext(problemsCountLine(errors, warnings)));
        setPanelNotice(notice, qtext(session.notice()));
        if (hooks.counted) hooks.counted(errors, warnings);
    }
};

ProblemsDock::ProblemsDock(Hooks hooks, QWidget *parent) : QWidget(parent) {
    impl_ = std::make_unique<Impl>(*this, std::move(hooks));
}

ProblemsDock::~ProblemsDock() {
    impl_->session.changed = nullptr;
    for (QObject *child : findChildren<QObject *>()) QObject::disconnect(child, nullptr, this, nullptr);
}

void ProblemsDock::engineAvailable() {
    impl_->engine = true;
    impl_->status_seen = false;
    impl_->session.engineReady();
    impl_->poll->start();
    // The first reading: the list as the engine started.
    if (impl_->hooks.read_status) impl_->hooks.read_status();
}

void ProblemsDock::engineUnavailable() {
    impl_->engine = false;
    impl_->poll->stop();
    impl_->session.engineGone();
}

void ProblemsDock::observeStatus(const nlohmann::json &status) {
    impl_->status_seen = true;
    impl_->session.observeStatus(status);
}

void ProblemsDock::readNow() { impl_->session.readNow(); }

void ProblemsDock::setAssetIssues(const std::vector<AssetStoreVerification> &stores) {
    (void)impl_->session.setAssetIssues(stores);
}

bool ProblemsDock::receiveResult(qint64 request_id, const QByteArray &result_json) {
    if (!impl_->session.owns(request_id)) return false;
    const auto result =
        Json::parse(result_json.constData(), result_json.constData() + result_json.size(), nullptr, false);
    return impl_->session.receiveResult(request_id, result.is_discarded() ? Json{} : result);
}

bool ProblemsDock::receiveFailure(qint64 request_id, const QString &message) {
    return impl_->session.receiveFailure(request_id, message.toStdString());
}

void ProblemsDock::openProblem(const QString &id) {
    const auto *row = impl_->session.find(id.toStdString());
    if (row == nullptr) return;
    // A restart is asked for with the button under the list, not by a click.
    if (row->place == ProblemRow::Place::Restart || row->place == ProblemRow::Place::None) return;
    if (impl_->hooks.open) impl_->hooks.open(*row);
}

const ProblemsSession &ProblemsDock::session() const noexcept { return impl_->session; }
QTreeWidget *ProblemsDock::list() const noexcept { return impl_->tree; }
QLabel *ProblemsDock::countLine() const noexcept { return impl_->count; }
QLabel *ProblemsDock::detailLine() const noexcept { return impl_->detail; }
QComboBox *ProblemsDock::severityFilter() const noexcept { return impl_->severity; }
QLineEdit *ProblemsDock::textFilter() const noexcept { return impl_->filter; }
QPushButton *ProblemsDock::restartButton() const noexcept { return impl_->restart; }

} // namespace PelicanStudio
