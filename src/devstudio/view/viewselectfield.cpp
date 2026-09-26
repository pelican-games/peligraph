#include "viewselectfield.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace PelicanStudio {

namespace {

QString text(const std::string &value) { return QString::fromStdString(value); }

QLabel *warningLabel(const QString &object_name, const std::string &line,
                     QWidget *parent) {
    auto *warning = new QLabel(text(line), parent);
    warning->setObjectName(object_name);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral("color: #d98c3d;"));
    return warning;
}

} // namespace

ViewSelectField::ViewSelectField(QString object_prefix, QWidget *parent)
    : QWidget(parent), prefix_(std::move(object_prefix)) {
    setObjectName(prefix_ + QStringLiteral(".view"));
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(2);
    setToolTip(tr("Which views of the pass's view family it runs for."));
}

void ViewSelectField::clear() {
    for (auto &row : rows_) {
        // A row can be cleared from inside its own combo's signal (a task
        // draft read again): the widgets go once the signal has returned,
        // and until then they are no longer the field's.
        row.widget->hide();
        row.widget->setParent(nullptr);
        row.widget->deleteLater();
    }
    rows_.clear();
}

bool ViewSelectField::load(
    const std::vector<FramePlanViewAxis> &axes,
    const std::vector<FramePlanViewSelectAxis> &view_select,
    const std::string &family) {
    clear();
    for (const auto &field : framePlanViewFieldRows(axes, view_select, family)) {
        Row row;
        row.axis = field.axis;
        row.kind = field.kind;
        row.kept = field.selected;
        const QString name = prefix_ + QStringLiteral(".view.") + text(field.axis);
        row.widget = new QWidget(this);
        auto *lines = new QVBoxLayout(row.widget);
        lines->setContentsMargins(0, 0, 0, 0);
        lines->setSpacing(1);
        auto *line = new QHBoxLayout;
        line->setContentsMargins(0, 0, 0, 0);
        lines->addLayout(line);
        switch (field.kind) {
        case FramePlanViewFieldRow::Kind::choice: {
            line->addWidget(new QLabel(text(field.axis), row.widget));
            row.combo = new QComboBox(row.widget);
            row.combo->setObjectName(name);
            row.combo->addItem(text(std::string{FramePlanAllViewsText}));
            for (std::size_t value = 0; value < field.values.size(); ++value)
                row.combo->addItem(text(field.values[value]),
                                   QVariant::fromValue<qulonglong>(value));
            // A value past the axis stays as written until it is changed.
            if (field.selected && *field.selected >= field.values.size())
                row.combo->addItem(
                    QString::number(static_cast<qulonglong>(*field.selected)),
                    QVariant::fromValue<qulonglong>(*field.selected));
            row.combo->setCurrentIndex(
                field.selected
                    ? row.combo->findData(
                          QVariant::fromValue<qulonglong>(*field.selected))
                    : 0);
            line->addWidget(row.combo, 1);
            QObject::connect(row.combo, &QComboBox::currentIndexChanged, this,
                             [this] {
                                 if (changed) changed();
                             });
            break;
        }
        case FramePlanViewFieldRow::Kind::runtime: {
            auto *label = new QLabel(text(field.text), row.widget);
            label->setObjectName(name);
            label->setEnabled(false);
            label->setToolTip(tr("The engine decides this axis each frame, so "
                                 "it cannot be selected."));
            line->addWidget(label, 1);
            break;
        }
        case FramePlanViewFieldRow::Kind::unknown: {
            auto *label = new QLabel(
                text(field.axis) + QStringLiteral(" ") +
                    QString::number(static_cast<qulonglong>(
                        field.selected.value_or(0))),
                row.widget);
            label->setObjectName(name);
            line->addWidget(label, 1);
            auto *remove = new QPushButton(tr("Remove"), row.widget);
            remove->setObjectName(name + QStringLiteral(".remove"));
            remove->setToolTip(tr("Take %1 out of view_select.").arg(text(field.axis)));
            line->addWidget(remove);
            const std::size_t index = rows_.size();
            QObject::connect(remove, &QPushButton::clicked, this, [this, index] {
                if (index >= rows_.size()) return;
                rows_[index].removed = true;
                rows_[index].widget->hide();
                if (changed) changed();
            });
            break;
        }
        }
        if (!field.text.empty() &&
            field.kind != FramePlanViewFieldRow::Kind::runtime)
            lines->addWidget(
                warningLabel(name + QStringLiteral(".warning"), field.text,
                             row.widget));
        layout_->addWidget(row.widget);
        rows_.push_back(std::move(row));
    }
    return !rows_.empty();
}

std::vector<FramePlanViewSelectAxis> ViewSelectField::selection() const {
    std::vector<FramePlanViewSelectAxis> axes;
    for (const auto &row : rows_) {
        if (row.removed) continue;
        std::optional<std::uint64_t> value = row.kept;
        if (row.combo != nullptr) {
            const QVariant data = row.combo->currentData();
            value = data.isValid()
                        ? std::optional<std::uint64_t>{data.toULongLong()}
                        : std::nullopt;
        }
        if (value) axes.push_back(FramePlanViewSelectAxis{row.axis, *value});
    }
    return axes;
}

} // namespace PelicanStudio
