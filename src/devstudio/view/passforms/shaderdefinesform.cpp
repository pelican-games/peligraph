#include "shaderdefinesform.hpp"

#include <QBrush>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>

namespace PelicanStudio {

using passform::Json;
using passform::text;

namespace {

// A row's entry as authored, when it is not a string: kept until the row is
// edited.
constexpr int RawRole = Qt::UserRole + 1;
// Whether the entry was written NAME=VALUE (so an empty value keeps its '=').
constexpr int EqualsRole = Qt::UserRole + 2;

bool identifier(std::string_view name) {
    if (name.empty()) return false;
    const auto first = static_cast<unsigned char>(name.front());
    if (!std::isalpha(first) && name.front() != '_') return false;
    return std::all_of(name.begin(), name.end(), [](char ch) {
        const auto value = static_cast<unsigned char>(ch);
        return std::isalnum(value) || ch == '_';
    });
}

const QColor refused_colour{0xc0, 0x39, 0x2b};

} // namespace

bool ShaderDefinesForm::engineReserved(std::string_view name) {
    // passshaderdefines.cpp, isEngineReservedShaderDefine.
    static constexpr std::array exact{
        std::string_view{"PELICAN_PASS_TEXTURE_INPUT_COUNT"},
        std::string_view{"PELICAN_VARIADIC_INPUT_COUNT"},
        std::string_view{"PELICAN_VARIADIC_INPUTS_LIMIT"},
        std::string_view{"PELICAN_MULTIVIEW"},
        std::string_view{"PELICAN_VIEW_COUNT"},
        std::string_view{"PELICAN_OUTPUT_UNORM_FALLBACK"},
    };
    if (std::find(exact.begin(), exact.end(), name) != exact.end()) return true;
    return name.starts_with("PELICAN_INPUT_") || name.starts_with("PELICAN_FEATURE_");
}

std::string ShaderDefinesForm::entryRefusal(const Json &defines, std::size_t index,
                                            const std::string &pass_name) {
    if (!defines.is_array() || index >= defines.size()) return {};
    const auto &entry = defines.at(index);
    const std::string authored =
        entry.is_string() ? entry.get<std::string>() : entry.dump();
    // passshaderdefines.cpp, defineErrorMessage.
    const std::string head = "Pass '" + pass_name + "' shader.defines entry " +
                             std::to_string(index) + " ('" + authored + "') ";
    const std::string malformed =
        head + "is not NAME or NAME=VALUE with NAME a C identifier";
    if (!entry.is_string()) return malformed;
    const auto name = authored.substr(0, authored.find('='));
    if (!identifier(name) || authored.find_first_of("\r\n") != std::string::npos)
        return malformed;
    if (engineReserved(name))
        return head + "defines " + name +
               ", which the engine defines itself when it compiles the pass";
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
        const auto &other = defines.at(earlier);
        if (other.is_string() &&
            other.get<std::string>().substr(0, other.get<std::string>().find('=')) == name)
            return head + "defines " + name + " a second time";
    }
    return {};
}

ShaderDefinesForm::ShaderDefinesForm(const QString &object_prefix, QWidget *parent)
    : PassForm(object_prefix, parent) {
    setObjectName(childName("shaderDefines"));
    setToolTip(tr("shader.defines: one define a row, NAME or NAME=VALUE. A red "
                  "row is one the engine refuses."));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableWidget(0, 2, this);
    table_->setObjectName(childName("shaderDefines.table"));
    table_->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setMaximumHeight(110);
    layout->addWidget(table_);
    auto *buttons = new QHBoxLayout;
    // N-7: a name, as the Inputs' Add is; what it adds is on the tooltip.
    add_ = new QPushButton(tr("Add"), this);
    add_->setObjectName(childName("shaderDefines.add"));
    add_->setToolTip(tr("Add a define: a row, NAME or NAME=VALUE."));
    remove_ = new QPushButton(tr("Remove"), this);
    remove_->setObjectName(childName("shaderDefines.remove"));
    buttons->addWidget(add_);
    buttons->addWidget(remove_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    QObject::connect(add_, &QPushButton::clicked, this, [this] {
        addRow({}, {}, false, nullptr);
        const int row = table_->rowCount() - 1;
        table_->setCurrentCell(row, 0);
        table_->editItem(table_->item(row, 0));
        showRefusals();
        notifyChanged();
    });
    QObject::connect(remove_, &QPushButton::clicked, this, [this] {
        const int row = table_->currentRow();
        if (row < 0) return;
        table_->removeRow(row);
        showRefusals();
        notifyChanged();
    });
    QObject::connect(table_, &QTableWidget::itemChanged, this,
                     [this](QTableWidgetItem *item) {
                         if (filling_) return;
                         // Edited: the row is its text now.
                         const QSignalBlocker blocker{table_};
                         if (auto *name = table_->item(item->row(), 0))
                             name->setData(RawRole, QVariant{});
                         showRefusals();
                         notifyChanged();
                     });
    reset();
}

void ShaderDefinesForm::reset() { load(nullptr); }

void ShaderDefinesForm::setRenderingSection(bool rendering_section) {
    rendering_section_ = rendering_section;
    setToolTip(rendering_section
                   ? tr("rendering.shader_defines: added to every shader's compile, one "
                        "define a row, NAME or NAME=VALUE. A red row is one that cannot "
                        "be a define.")
                   : tr("shader.defines: one define a row, NAME or NAME=VALUE. A red "
                        "row is one the engine refuses."));
    showRefusals();
}

std::string ShaderDefinesForm::refusalAt(const Json &defines, std::size_t index) const {
    if (!rendering_section_) return entryRefusal(defines, index, pass_name_);
    if (!defines.is_array() || index >= defines.size()) return {};
    const auto &entry = defines.at(index);
    const std::string authored = entry.is_string() ? entry.get<std::string>() : entry.dump();
    const std::string head =
        "rendering.shader_defines entry " + std::to_string(index) + " ('" + authored + "') ";
    if (!entry.is_string()) return head + "is not a string";
    const auto name = authored.substr(0, authored.find('='));
    if (!identifier(name) || authored.find_first_of("\r\n") != std::string::npos)
        return head + "is not NAME or NAME=VALUE with NAME a C identifier";
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
        const auto &other = defines.at(earlier);
        if (other.is_string() &&
            other.get<std::string>().substr(0, other.get<std::string>().find('=')) == name)
            return head + "defines " + name + " a second time";
    }
    return {};
}

void ShaderDefinesForm::setPassName(const std::string &pass_name) {
    pass_name_ = pass_name;
    showRefusals();
}

void ShaderDefinesForm::addRow(const QString &name, const QString &value, bool equals,
                               const Json &raw) {
    filling_ = true;
    const int row = table_->rowCount();
    table_->insertRow(row);
    auto *name_item = new QTableWidgetItem(name);
    name_item->setData(EqualsRole, equals);
    if (!raw.is_null()) name_item->setData(RawRole, text(raw.dump()));
    table_->setItem(row, 0, name_item);
    table_->setItem(row, 1, new QTableWidgetItem(value));
    filling_ = false;
}

void ShaderDefinesForm::load(const Json &defines) {
    const Loading loading{*this};
    filling_ = true;
    table_->setRowCount(0);
    filling_ = false;
    if (defines.is_array()) {
        for (const auto &entry : defines) {
            if (!entry.is_string()) {
                addRow(text(entry.dump()), {}, false, entry);
                continue;
            }
            const auto authored = text(entry.get<std::string>());
            const auto separator = authored.indexOf(QLatin1Char('='));
            if (separator < 0)
                addRow(authored, {}, false, nullptr);
            else
                addRow(authored.left(separator), authored.mid(separator + 1), true,
                       nullptr);
        }
    }
    showRefusals();
}

Json ShaderDefinesForm::defines() const {
    Json result = Json::array();
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto *name_item = table_->item(row, 0);
        const auto *value_item = table_->item(row, 1);
        const auto raw = name_item ? name_item->data(RawRole).toString() : QString{};
        if (!raw.isEmpty()) {
            result.push_back(Json::parse(raw.toStdString()));
            continue;
        }
        const QString name = name_item ? name_item->text().trimmed() : QString{};
        const QString value = value_item ? value_item->text().trimmed() : QString{};
        // A row left empty is not a define yet.
        if (name.isEmpty() && value.isEmpty()) continue;
        const bool equals = !value.isEmpty() ||
                            (name_item && name_item->data(EqualsRole).toBool());
        result.push_back((equals ? name + QLatin1Char('=') + value : name).toStdString());
    }
    return result.empty() ? Json(nullptr) : result;
}

QString ShaderDefinesForm::rowRefusal(int row) const {
    const auto *item = table_->item(row, 0);
    return item ? item->toolTip() : QString{};
}

QString ShaderDefinesForm::refusal() const {
    const Json entries = defines();
    if (entries.is_null()) return {};
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto sentence = refusalAt(entries, index);
        if (!sentence.empty()) return text(sentence);
    }
    return {};
}

void ShaderDefinesForm::showRefusals() {
    const QSignalBlocker blocker{table_};
    filling_ = true;
    // The rows that are defines, in the order defines() gives them.
    const Json entries = defines();
    std::size_t index = 0;
    for (int row = 0; row < table_->rowCount(); ++row) {
        auto *name_item = table_->item(row, 0);
        auto *value_item = table_->item(row, 1);
        const bool empty = (!name_item || name_item->text().trimmed().isEmpty()) &&
                           (!value_item || value_item->text().trimmed().isEmpty()) &&
                           (!name_item || name_item->data(RawRole).toString().isEmpty());
        const auto sentence = empty ? std::string{} : refusalAt(entries, index++);
        for (auto *item : {name_item, value_item}) {
            if (!item) continue;
            item->setToolTip(text(sentence));
            item->setForeground(sentence.empty() ? QBrush{} : QBrush{refused_colour});
        }
    }
    filling_ = false;
}

} // namespace PelicanStudio
