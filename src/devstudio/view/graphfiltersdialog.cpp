#include "graphfiltersdialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <utility>

namespace PelicanStudio {
namespace {

QString qtext(const std::string &text) { return QString::fromStdString(text); }
QString graphFiltersObjectName(const QString &suffix) {
    return QStringLiteral("pelican.graphFiltersDialog.") + suffix;
}

class StringListWidget final : public QWidget {
  public:
    QListWidget *list = nullptr;
    QLineEdit *edit = nullptr;
    QPushButton *add = nullptr;
    QPushButton *remove = nullptr;
    std::function<void(const std::vector<std::string> &)> edited;
    std::function<bool(const std::string &)> accepts;
    QStringList completions;

    explicit StringListWidget(const QString &name, QWidget *parent = nullptr)
        : QWidget(parent) {
        setObjectName(graphFiltersObjectName(name));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        list = new QListWidget(this);
        list->setObjectName(graphFiltersObjectName(name + QStringLiteral(".list")));
        layout->addWidget(list);
        auto *row = new QHBoxLayout;
        edit = new QLineEdit(this);
        edit->setObjectName(graphFiltersObjectName(name + QStringLiteral(".edit")));
        row->addWidget(edit, 1);
        add = new QPushButton(QObject::tr("Add"), this);
        add->setObjectName(graphFiltersObjectName(name + QStringLiteral(".add")));
        row->addWidget(add);
        remove = new QPushButton(QObject::tr("Remove"), this);
        remove->setObjectName(graphFiltersObjectName(name + QStringLiteral(".remove")));
        row->addWidget(remove);
        layout->addLayout(row);
        auto *completer = new QCompleter(this);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        edit->setCompleter(completer);
        const auto append = [this] {
            const auto value = edit->text().trimmed().toStdString();
            if (value.empty() || (accepts && !accepts(value))) return;
            auto values = strings();
            if (std::ranges::find(values, value) == values.end()) values.push_back(value);
            setStrings(values);
            edit->clear();
            if (edited) edited(values);
        };
        QObject::connect(add, &QPushButton::clicked, this, append);
        QObject::connect(edit, &QLineEdit::returnPressed, this, append);
        QObject::connect(remove, &QPushButton::clicked, this, [this] {
            auto values = strings();
            const int row_ = list->currentRow();
            if (row_ < 0 || row_ >= static_cast<int>(values.size())) return;
            values.erase(values.begin() + row_);
            setStrings(values);
            if (edited) edited(values);
        });
    }

    void setStrings(const std::vector<std::string> &values) {
        QSignalBlocker block{list};
        list->clear();
        for (const auto &value : values) list->addItem(qtext(value));
        edit->setCompleter(new QCompleter(completions, edit));
    }

    std::vector<std::string> strings() const {
        std::vector<std::string> values;
        for (int row = 0; row < list->count(); ++row)
            values.push_back(list->item(row)->text().toStdString());
        return values;
    }
};

QPushButton *button(const QString &name, const QString &label, QWidget *parent) {
    auto *result = new QPushButton(label, parent);
    result->setObjectName(graphFiltersObjectName(name));
    return result;
}

} // namespace

struct GraphFiltersDialog::Impl {
    GraphFiltersDialog &view;
    GraphFiltersEdit *session = nullptr;
    QTabWidget *tabs = nullptr;
    QListWidget *filters = nullptr;
    QPushButton *add_filter = nullptr;
    QPushButton *remove_filter = nullptr;
    QPushButton *up = nullptr;
    QPushButton *down = nullptr;
    QLineEdit *label = nullptr;
    QLabel *children = nullptr;
    StringListWidget *ops = nullptr;
    StringListWidget *node_ids = nullptr;
    StringListWidget *features = nullptr;
    StringListWidget *categories = nullptr;
    QPushButton *add_selected = nullptr;
    QListWidget *category_list = nullptr;
    QPushButton *add_category = nullptr;
    QPushButton *remove_category = nullptr;
    QLineEdit *category_label = nullptr;
    QLabel *category_references = nullptr;
    QLineEdit *feature_name = nullptr;
    QPushButton *add_feature = nullptr;
    QTableWidget *feature_table = nullptr;
    QLabel *preview = nullptr;
    QLabel *state = nullptr;
    QPushButton *save = nullptr;
    QPushButton *undo = nullptr;
    QPushButton *discard = nullptr;
    QPushButton *load = nullptr;
    QPushButton *close = nullptr;
    QString selected_filter;
    QString selected_category;
    std::set<std::string> typed_features;
    bool syncing = false;

    explicit Impl(GraphFiltersDialog &owner) : view(owner) {}
    void sync();
    void syncFilters();
    void syncCategories();
    void syncFeatures();
    void syncMatch();
    void updateMatch();
    void addFeature();
    QString currentFilter() const;
    QString currentCategory() const;
};

GraphFiltersDialog::GraphFiltersDialog(QWidget *parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(*this)) {
    setObjectName(QStringLiteral("pelican.graphFiltersDialog"));
    setWindowTitle(tr("Graph filters"));
    setModal(false);
    auto *layout = new QVBoxLayout(this);
    impl_->tabs = new QTabWidget(this);
    impl_->tabs->setObjectName(graphFiltersObjectName(QStringLiteral("tabs")));
    layout->addWidget(impl_->tabs, 1);

    auto *filters_page = new QWidget(impl_->tabs);
    filters_page->setObjectName(graphFiltersObjectName(QStringLiteral("tab.filters")));
    auto *filters_layout = new QHBoxLayout(filters_page);
    auto *filter_column = new QVBoxLayout;
    impl_->filters = new QListWidget(filters_page);
    impl_->filters->setObjectName(graphFiltersObjectName(QStringLiteral("filters")));
    filter_column->addWidget(impl_->filters, 1);
    auto *filter_buttons = new QHBoxLayout;
    impl_->add_filter = button(QStringLiteral("addFilter"), tr("Add"), filters_page);
    impl_->remove_filter = button(QStringLiteral("removeFilter"), tr("Remove"), filters_page);
    impl_->up = button(QStringLiteral("up"), tr("Up"), filters_page);
    impl_->down = button(QStringLiteral("down"), tr("Down"), filters_page);
    for (auto *control : {impl_->add_filter, impl_->remove_filter, impl_->up, impl_->down})
        filter_buttons->addWidget(control);
    filter_column->addLayout(filter_buttons);
    filters_layout->addLayout(filter_column);

    auto *details = new QVBoxLayout;
    auto *label_row = new QHBoxLayout;
    label_row->addWidget(new QLabel(tr("Label"), filters_page));
    impl_->label = new QLineEdit(filters_page);
    impl_->label->setObjectName(graphFiltersObjectName(QStringLiteral("label")));
    label_row->addWidget(impl_->label, 1);
    details->addLayout(label_row);
    impl_->children = new QLabel(filters_page);
    impl_->children->setObjectName(graphFiltersObjectName(QStringLiteral("children")));
    details->addWidget(impl_->children);
    const auto make_list = [&](const QString &name, const QString &title) {
        auto *row = new QHBoxLayout;
        auto *caption = new QLabel(title, filters_page);
        caption->setMinimumWidth(caption->fontMetrics().averageCharWidth() * 10);
        row->addWidget(caption);
        auto *list = new StringListWidget(name, filters_page);
        row->addWidget(list, 1);
        details->addLayout(row);
        return list;
    };
    impl_->ops = make_list(QStringLiteral("ops"), tr("Operations"));
    impl_->node_ids = make_list(QStringLiteral("nodeIds"), tr("Node ids"));
    impl_->features = make_list(QStringLiteral("features"), tr("Features"));
    impl_->categories = make_list(QStringLiteral("categories"), tr("Categories"));
    impl_->add_selected = button(QStringLiteral("addSelected"), tr("Add selected nodes"), filters_page);
    details->addWidget(impl_->add_selected);
    impl_->preview = new QLabel(filters_page);
    impl_->preview->setObjectName(graphFiltersObjectName(QStringLiteral("preview")));
    impl_->preview->setWordWrap(true);
    details->addWidget(impl_->preview);
    auto *preview_note = new QLabel(tr("Any listed item matches. Show boxes also apply."), filters_page);
    preview_note->setObjectName(graphFiltersObjectName(QStringLiteral("preview.note")));
    details->addWidget(preview_note);
    details->addStretch(1);
    filters_layout->addLayout(details, 1);
    impl_->tabs->addTab(filters_page, tr("Filters"));

    auto *categories_page = new QWidget(impl_->tabs);
    categories_page->setObjectName(graphFiltersObjectName(QStringLiteral("tab.categories")));
    auto *categories_layout = new QVBoxLayout(categories_page);
    auto *category_row = new QHBoxLayout;
    impl_->category_list = new QListWidget(categories_page);
    impl_->category_list->setObjectName(graphFiltersObjectName(QStringLiteral("categoryList")));
    category_row->addWidget(impl_->category_list, 1);
    auto *category_controls = new QVBoxLayout;
    impl_->add_category = button(QStringLiteral("addCategory"), tr("Add"), categories_page);
    impl_->remove_category = button(QStringLiteral("removeCategory"), tr("Remove"), categories_page);
    category_controls->addWidget(impl_->add_category);
    category_controls->addWidget(impl_->remove_category);
    impl_->category_label = new QLineEdit(categories_page);
    impl_->category_label->setObjectName(graphFiltersObjectName(QStringLiteral("categoryLabel")));
    impl_->category_label->setPlaceholderText(tr("Category label"));
    category_controls->addWidget(impl_->category_label);
    impl_->category_references = new QLabel(categories_page);
    impl_->category_references->setObjectName(graphFiltersObjectName(QStringLiteral("categoryReferences")));
    impl_->category_references->setWordWrap(true);
    category_controls->addWidget(impl_->category_references);
    category_controls->addStretch(1);
    category_row->addLayout(category_controls);
    categories_layout->addLayout(category_row);
    auto *feature_row = new QHBoxLayout;
    impl_->feature_name = new QLineEdit(categories_page);
    impl_->feature_name->setObjectName(graphFiltersObjectName(QStringLiteral("featureName")));
    impl_->feature_name->setPlaceholderText(tr("Feature name"));
    feature_row->addWidget(impl_->feature_name, 1);
    impl_->add_feature = button(QStringLiteral("addFeature"), tr("Add feature"), categories_page);
    feature_row->addWidget(impl_->add_feature);
    categories_layout->addLayout(feature_row);
    impl_->feature_table = new QTableWidget(categories_page);
    impl_->feature_table->setObjectName(graphFiltersObjectName(QStringLiteral("featureTable")));
    impl_->feature_table->horizontalHeader()->setStretchLastSection(true);
    categories_layout->addWidget(impl_->feature_table, 1);
    impl_->tabs->addTab(categories_page, tr("Feature categories"));

    auto *state_row = new QHBoxLayout;
    impl_->state = new QLabel(this);
    impl_->state->setObjectName(graphFiltersObjectName(QStringLiteral("state")));
    impl_->state->setWordWrap(true);
    state_row->addWidget(impl_->state, 1);
    impl_->load = button(QStringLiteral("load"), tr("Load from disk"), this);
    state_row->addWidget(impl_->load);
    layout->addLayout(state_row);
    auto *bottom = new QHBoxLayout;
    bottom->addStretch(1);
    impl_->save = button(QStringLiteral("save"), tr("Save"), this);
    impl_->undo = button(QStringLiteral("undo"), tr("Undo"), this);
    impl_->discard = button(QStringLiteral("discard"), tr("Discard changes"), this);
    impl_->close = button(QStringLiteral("close"), tr("Close"), this);
    for (auto *control : {impl_->save, impl_->undo, impl_->discard, impl_->close})
        bottom->addWidget(control);
    layout->addLayout(bottom);

    auto &d = *impl_;
    connect(d.filters, &QListWidget::currentRowChanged, this, [this] {
        if (impl_->syncing) return;
        impl_->selected_filter = impl_->currentFilter();
        impl_->syncMatch();
    });
    connect(d.add_filter, &QPushButton::clicked, this, [this] {
        if (impl_->session && impl_->session->addFilter("Filter")) {
            impl_->selected_filter = qtext(impl_->session->draft().filters.back().id);
            impl_->sync();
        }
    });
    connect(d.remove_filter, &QPushButton::clicked, this, [this] {
        if (impl_->session) impl_->session->removeFilter(impl_->currentFilter().toStdString());
    });
    connect(d.up, &QPushButton::clicked, this, [this] {
        if (impl_->session) impl_->session->moveFilter(impl_->currentFilter().toStdString(), -1);
    });
    connect(d.down, &QPushButton::clicked, this, [this] {
        if (impl_->session) impl_->session->moveFilter(impl_->currentFilter().toStdString(), 1);
    });
    connect(d.label, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (impl_->session) impl_->session->setLabel(impl_->currentFilter().toStdString(), text.toStdString());
    });
    for (auto *list : {d.ops, d.node_ids, d.features, d.categories})
        list->edited = [this](const auto &) { impl_->updateMatch(); };
    d.categories->accepts = [this](const std::string &id) {
        return impl_->session && std::ranges::any_of(impl_->session->draft().categories, [&](const auto &category) {
            return category.id == id;
        });
    };
    connect(d.add_selected, &QPushButton::clicked, this, [this] {
        if (!impl_->session || !selection) return;
        const auto picked = selection();
        if (!picked.root) return;
        auto values = impl_->node_ids->strings();
        for (const auto &id : picked.node_ids)
            if (std::ranges::find(values, id) == values.end()) values.push_back(id);
        impl_->node_ids->setStrings(values);
        impl_->updateMatch();
    });
    connect(d.category_list, &QListWidget::currentRowChanged, this, [this] {
        if (impl_->syncing) return;
        impl_->selected_category = impl_->currentCategory();
        impl_->syncCategories();
        impl_->syncFeatures();
    });
    connect(d.add_category, &QPushButton::clicked, this, [this] {
        const auto text = impl_->category_label->text().trimmed();
        if (impl_->session && impl_->session->addCategory(text.isEmpty() ? "Category" : text.toStdString())) {
            impl_->selected_category = qtext(impl_->session->draft().categories.back().id);
            impl_->sync();
        }
    });
    connect(d.remove_category, &QPushButton::clicked, this, [this] {
        if (impl_->session) impl_->session->removeCategory(impl_->currentCategory().toStdString());
    });
    connect(d.category_label, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (impl_->session) impl_->session->setCategoryLabel(impl_->currentCategory().toStdString(), text.toStdString());
    });
    connect(d.add_feature, &QPushButton::clicked, this, [this] { impl_->addFeature(); });
    connect(d.feature_name, &QLineEdit::returnPressed, this, [this] { impl_->addFeature(); });
    connect(d.save, &QPushButton::clicked, this, [this] {
        if (impl_->session && impl_->session->save() && requestReady) requestReady();
    });
    connect(d.undo, &QPushButton::clicked, this, [this] {
        if (impl_->session && impl_->session->undo() && requestReady) requestReady();
    });
    connect(d.discard, &QPushButton::clicked, this, [this] {
        if (impl_->session) impl_->session->discard();
    });
    connect(d.load, &QPushButton::clicked, this, [this] {
        if (impl_->session && impl_->session->loadReplacesDirtyDraft() && requestReady) requestReady();
    });
    connect(d.close, &QPushButton::clicked, this, &QDialog::hide);
    sync();
}

GraphFiltersDialog::~GraphFiltersDialog() {
    if (impl_->session != nullptr) impl_->session->changed = {};
}

void GraphFiltersDialog::bind(GraphFiltersEdit *session) {
    if (impl_->session == session) { sync(); return; }
    if (impl_->session != nullptr) impl_->session->changed = {};
    impl_->session = session;
    impl_->selected_filter.clear();
    impl_->selected_category.clear();
    impl_->typed_features.clear();
    if (session != nullptr) session->changed = [this] { sync(); };
    sync();
}

void GraphFiltersDialog::reject() { hide(); }

void GraphFiltersDialog::sync() { impl_->sync(); }

QString GraphFiltersDialog::Impl::currentFilter() const {
    const auto *item = filters->currentItem();
    return item != nullptr ? item->data(Qt::UserRole).toString() : QString{};
}

QString GraphFiltersDialog::Impl::currentCategory() const {
    const auto *item = category_list->currentItem();
    return item != nullptr ? item->data(Qt::UserRole).toString() : QString{};
}

void GraphFiltersDialog::Impl::sync() {
    if (syncing) return;
    syncing = true;
    syncFilters();
    syncCategories();
    syncFeatures();
    syncMatch();
    const auto status = session != nullptr ? session->state() : GraphFiltersEdit::State{};
    state->setText(qtext(status.text));
    save->setEnabled(status.save_enabled);
    undo->setEnabled(status.undo_enabled);
    discard->setEnabled(status.discard_enabled);
    load->setEnabled(status.load_enabled);
    load->setVisible(status.external_modification || status.dirty);
    for (QWidget *control : {static_cast<QWidget *>(add_filter), static_cast<QWidget *>(label),
                             static_cast<QWidget *>(ops), static_cast<QWidget *>(node_ids),
                             static_cast<QWidget *>(features), static_cast<QWidget *>(categories),
                             static_cast<QWidget *>(add_category), static_cast<QWidget *>(category_label),
                             static_cast<QWidget *>(feature_name), static_cast<QWidget *>(add_feature),
                             static_cast<QWidget *>(feature_table)})
        control->setEnabled(status.editing_enabled);
    add_selected->setEnabled(status.editing_enabled && view.selection && view.selection().root);
    syncing = false;
}

void GraphFiltersDialog::Impl::syncFilters() {
    const QSignalBlocker block{filters};
    filters->clear();
    if (session == nullptr) return;
    int selected_row = 0;
    int row = 0;
    for (const auto &filter : session->draft().filters) {
        auto *item = new QListWidgetItem(qtext(filter.label), filters);
        item->setData(Qt::UserRole, qtext(filter.id));
        item->setToolTip(qtext(filter.id));
        if (qtext(filter.id) == selected_filter) selected_row = row;
        ++row;
    }
    if (!session->draft().filters.empty()) filters->setCurrentRow(selected_row);
    selected_filter = currentFilter();
    const auto id = selected_filter.toStdString();
    const auto index = filters->currentRow();
    remove_filter->setEnabled(session->state().editing_enabled && index >= 0);
    up->setEnabled(session->state().editing_enabled && index > 0);
    down->setEnabled(session->state().editing_enabled && index >= 0 &&
                     index + 1 < static_cast<int>(session->draft().filters.size()));
    const auto descendants = session->descendantCount(id);
    children->setText(descendants == 0 ? QString{} : tr("Removing this filter also removes %1 children.").arg(descendants));
}

void GraphFiltersDialog::Impl::syncCategories() {
    const QSignalBlocker block{category_list};
    category_list->clear();
    if (session == nullptr) return;
    int selected_row = 0;
    int row = 0;
    for (const auto &category : session->draft().categories) {
        auto *item = new QListWidgetItem(qtext(category.label), category_list);
        item->setData(Qt::UserRole, qtext(category.id));
        item->setToolTip(qtext(category.id));
        if (qtext(category.id) == selected_category) selected_row = row;
        ++row;
    }
    if (!session->draft().categories.empty()) category_list->setCurrentRow(selected_row);
    selected_category = currentCategory();
    const auto category = std::ranges::find(session->draft().categories, selected_category.toStdString(),
                                            &Pelican::GraphFilterCategory::id);
    if (category != session->draft().categories.end()) {
        const QSignalBlocker label_block{category_label};
        category_label->setText(qtext(category->label));
        const auto [feature_count, filter_count] = session->categoryReferenceCounts(category->id);
        category_references->setText(tr("%1 features and %2 filters refer to this category.")
                                        .arg(feature_count).arg(filter_count));
    } else {
        const QSignalBlocker label_block{category_label};
        category_label->clear();
        category_references->clear();
    }
    remove_category->setEnabled(session->state().editing_enabled && !selected_category.isEmpty());
    categories->completions.clear();
    for (const auto &entry : session->draft().categories) categories->completions.push_back(qtext(entry.id));
}

void GraphFiltersDialog::Impl::syncMatch() {
    const auto *filter = session != nullptr
                             ? static_cast<const GraphFiltersEdit *>(session)->findFilter(currentFilter().toStdString())
                             : nullptr;
    {
        const QSignalBlocker block{label};
        label->setText(filter != nullptr ? qtext(filter->label) : QString{});
    }
    const auto match = filter != nullptr ? filter->match : Pelican::GraphFilterMatch{};
    ops->setStrings(match.ops);
    node_ids->setStrings(match.node_ids);
    features->setStrings(match.features);
    categories->setStrings(match.feature_categories);
    remove_filter->setEnabled(session != nullptr && session->state().editing_enabled && filter != nullptr);
    std::vector<GraphFilterFacts> facts = view.rootFacts ? view.rootFacts() : std::vector<GraphFilterFacts>{};
    QStringList operation_names;
    QStringList feature_names;
    for (const auto &fact : facts) {
        if (!fact.op.empty()) operation_names.push_back(qtext(fact.op));
        if (fact.kind == GraphFilterFacts::Kind::folded_feature && !fact.feature_name.empty())
            feature_names.push_back(qtext(fact.feature_name));
    }
    operation_names.removeDuplicates();
    feature_names.removeDuplicates();
    operation_names.sort();
    feature_names.sort();
    ops->completions = operation_names;
    features->completions = feature_names;
    ops->setStrings(match.ops);
    features->setStrings(match.features);
    if (filter == nullptr) {
        preview->setText(tr("Matches nothing"));
        return;
    }
    const bool empty = match.ops.empty() && match.node_ids.empty() && match.features.empty() &&
                       match.feature_categories.empty();
    if (empty) {
        preview->setText(tr("Matches nothing"));
        return;
    }
    QStringList names;
    std::size_t count = 0;
    for (const auto &fact : facts) {
        if (!graphFilterMatches(*filter, session->draft(), fact)) continue;
        ++count;
        if (names.size() < 5)
            names.push_back(qtext(fact.kind == GraphFilterFacts::Kind::node ? fact.node_id : fact.feature_name));
    }
    auto text = tr("Matches %1 nodes and features").arg(count);
    if (!names.isEmpty()) text += QStringLiteral(": ") + names.join(QStringLiteral(", ")) + QStringLiteral("...");
    preview->setText(text);
}

void GraphFiltersDialog::Impl::syncFeatures() {
    feature_table->clear();
    if (session == nullptr) return;
    std::vector<std::string> names = typed_features.empty()
                                         ? std::vector<std::string>{}
                                         : std::vector<std::string>(typed_features.begin(), typed_features.end());
    if (view.rootFacts)
        for (const auto &fact : view.rootFacts())
            if (fact.kind == GraphFilterFacts::Kind::folded_feature && !fact.feature_name.empty())
                names.push_back(fact.feature_name);
    for (const auto &[feature, categories_] : session->draft().feature_categories) names.push_back(feature);
    std::ranges::sort(names);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    const auto &declared = session->draft().categories;
    feature_table->setRowCount(static_cast<int>(names.size()));
    feature_table->setColumnCount(static_cast<int>(declared.size()) + 1);
    QStringList headers{tr("Feature")};
    for (const auto &category : declared) headers.push_back(qtext(category.label));
    feature_table->setHorizontalHeaderLabels(headers);
    int row = 0;
    for (const auto &feature : names) {
        auto *name = new QTableWidgetItem(qtext(feature));
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        feature_table->setItem(row, 0, name);
        int column = 1;
        for (const auto &category : declared) {
            bool checked = false;
            const auto entry = std::ranges::find(session->draft().feature_categories, feature,
                                                 &std::pair<std::string, std::vector<std::string>>::first);
            if (entry != session->draft().feature_categories.end())
                checked = std::ranges::find(entry->second, category.id) != entry->second.end();
            auto *box = new QCheckBox(feature_table);
            box->setObjectName(graphFiltersObjectName(QStringLiteral("feature.") + qtext(feature) +
                                          QStringLiteral(".") + qtext(category.id)));
            box->setToolTip(tr("%1 in %2").arg(qtext(feature), qtext(category.label)));
            box->setChecked(checked);
            box->setEnabled(session->state().editing_enabled);
            QObject::connect(box, &QCheckBox::toggled, &view, [this, feature, id = category.id](bool on) {
                if (syncing || session == nullptr) return;
                QMetaObject::invokeMethod(&view, [this, feature, id, on] {
                    if (session == nullptr) return;
                    std::vector<std::string> ids;
                    const auto found = std::ranges::find(session->draft().feature_categories, feature,
                                                         &std::pair<std::string, std::vector<std::string>>::first);
                    if (found != session->draft().feature_categories.end()) ids = found->second;
                    if (on && std::ranges::find(ids, id) == ids.end()) ids.push_back(id);
                    if (!on) std::erase(ids, id);
                    session->setFeatureCategories(feature, ids);
                }, Qt::QueuedConnection);
            });
            feature_table->setCellWidget(row, column++, box);
        }
        ++row;
    }
    feature_table->resizeColumnsToContents();
}

void GraphFiltersDialog::Impl::updateMatch() {
    if (syncing || session == nullptr) return;
    session->setMatch(currentFilter().toStdString(),
                      {ops->strings(), node_ids->strings(), features->strings(), categories->strings()});
}

void GraphFiltersDialog::Impl::addFeature() {
    const auto name = feature_name->text().trimmed();
    if (name.isEmpty()) return;
    typed_features.insert(name.toStdString());
    feature_name->clear();
    syncFeatures();
}

} // namespace PelicanStudio
