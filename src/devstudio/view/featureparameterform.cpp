#include "featureparameterform.hpp"

#include "studiotheme.hpp"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <system_error>

namespace PelicanStudio {
namespace {

using Range = std::optional<std::pair<double, double>>;

Range rangeOf(const QJsonObject &declaration) {
    const auto range = declaration.value(QStringLiteral("range")).toArray();
    if (range.size() != 2 || !range[0].isDouble() || !range[1].isDouble()) return std::nullopt;
    return std::pair{range[0].toDouble(), range[1].toDouble()};
}

bool numeric(const QJsonObject &declaration) {
    const auto type = declaration.value(QStringLiteral("type")).toString();
    return type == QStringLiteral("float") || type == QStringLiteral("int");
}

struct Group {
    FeatureParameterRow::Kind kind;
    QStringList required;
    QStringList optional;
};

const std::vector<Group> &groups() {
    static const std::vector<Group> all{
        {FeatureParameterRow::Kind::Color,
         {QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b")},
         {QStringLiteral("a")}},
        {FeatureParameterRow::Kind::Vector,
         {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")},
         {QStringLiteral("w"), QStringLiteral("offset")}},
    };
    return all;
}

QString stageOfExtension(const std::string &extension) {
    static const std::map<std::string, QString> stages{
        {".comp", QStringLiteral("compute")},     {".frag", QStringLiteral("fragment")},
        {".vert", QStringLiteral("vertex")},      {".geom", QStringLiteral("geometry")},
        {".tesc", QStringLiteral("tess_control")}, {".tese", QStringLiteral("tess_evaluation")},
        {".mesh", QStringLiteral("mesh")},        {".task", QStringLiteral("task")},
        {".rgen", QStringLiteral("raygen")},      {".rmiss", QStringLiteral("miss")},
        {".rchit", QStringLiteral("closesthit")}, {".rahit", QStringLiteral("anyhit")},
        {".rint", QStringLiteral("intersection")}, {".rcall", QStringLiteral("callable")},
    };
    const auto found = stages.find(extension);
    return found == stages.end() ? QString{} : found->second;
}

bool sameValue(const QJsonValue &a, const QJsonValue &b) {
    if (a.isDouble() && b.isDouble())
        return std::abs(a.toDouble() - b.toDouble()) <=
               1e-9 * std::max({1.0, std::abs(a.toDouble()), std::abs(b.toDouble())});
    return a == b;
}

QString shown(const QJsonValue &value) {
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 6);
    if (value.isBool()) return value.toBool() ? QStringLiteral("on") : QStringLiteral("off");
    if (value.isString()) return value.toString();
    return QStringLiteral("none");
}

} // namespace

std::vector<FeatureParameterRow> featureParameterRows(const QJsonObject &schema) {
    std::vector<FeatureParameterRow> rows;
    std::vector<QJsonObject> scalars;
    for (const auto &value : schema.value(QStringLiteral("scalars")).toArray())
        if (value.isObject()) scalars.push_back(value.toObject());
    std::map<QString, std::size_t> index;
    for (std::size_t i = 0; i < scalars.size(); ++i)
        index.emplace(scalars[i].value(QStringLiteral("name")).toString(), i);
    std::vector<bool> used(scalars.size(), false);
    const auto member = [&](const QString &name) -> std::optional<std::size_t> {
        const auto found = index.find(name);
        if (found == index.end() || used[found->second] || !numeric(scalars[found->second]))
            return std::nullopt;
        return found->second;
    };
    const auto append = [&](FeatureParameterRow &row, std::size_t i) {
        row.names << scalars[i].value(QStringLiteral("name")).toString();
        row.defaults.append(scalars[i].value(QStringLiteral("default")));
        row.engine_defaults.append(scalars[i].value(QStringLiteral("engine_default")));
        row.ranges.push_back(rangeOf(scalars[i]));
        used[i] = true;
    };
    for (std::size_t i = 0; i < scalars.size(); ++i) {
        if (used[i]) continue;
        const auto &declaration = scalars[i];
        const auto name = declaration.value(QStringLiteral("name")).toString();
        bool grouped = false;
        if (numeric(declaration)) {
            for (const auto &group : groups()) {
                for (const auto &suffix : group.required + group.optional) {
                    if (!name.endsWith(QLatin1Char('_') + suffix)) continue;
                    const auto stem = name.chopped(suffix.size() + 1);
                    if (stem.isEmpty()) continue;
                    std::vector<std::size_t> members;
                    bool complete = true;
                    for (const auto &part : group.required) {
                        const auto found = member(stem + QLatin1Char('_') + part);
                        if (!found) {
                            complete = false;
                            break;
                        }
                        members.push_back(*found);
                    }
                    if (!complete) continue;
                    for (const auto &part : group.optional)
                        if (const auto found = member(stem + QLatin1Char('_') + part)) {
                            members.push_back(*found);
                            // A vector takes one fourth: a w or an offset.
                            if (group.kind == FeatureParameterRow::Kind::Vector) break;
                        }
                    FeatureParameterRow row;
                    row.kind = group.kind;
                    row.label = stem;
                    bool all_int = true;
                    for (const auto at : members) {
                        all_int = all_int &&
                                  scalars[at].value(QStringLiteral("type")).toString() ==
                                      QStringLiteral("int");
                        append(row, at);
                    }
                    row.type = all_int ? QStringLiteral("int") : QStringLiteral("float");
                    rows.push_back(std::move(row));
                    grouped = true;
                    break;
                }
                if (grouped) break;
            }
        }
        if (grouped) continue;
        FeatureParameterRow row;
        row.type = declaration.value(QStringLiteral("type")).toString();
        row.kind = row.type == QStringLiteral("bool") ? FeatureParameterRow::Kind::Boolean
                                                      : FeatureParameterRow::Kind::Number;
        row.label = name;
        append(row, i);
        rows.push_back(std::move(row));
    }
    for (const auto *category : {"render_targets", "shader_assets"}) {
        for (const auto &value : schema.value(QString::fromLatin1(category)).toArray()) {
            const auto declaration = value.toObject();
            FeatureParameterRow row;
            row.kind = QString::fromLatin1(category) == QStringLiteral("render_targets")
                           ? FeatureParameterRow::Kind::RenderTarget
                           : FeatureParameterRow::Kind::ShaderAsset;
            row.type = QStringLiteral("string");
            row.label = declaration.value(QStringLiteral("name")).toString();
            row.names << row.label;
            row.defaults.append(declaration.value(QStringLiteral("default")));
            row.engine_defaults.append(declaration.value(QStringLiteral("engine_default")));
            row.ranges.push_back(std::nullopt);
            row.stage = declaration.value(QStringLiteral("stage")).toString();
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::vector<std::pair<QString, QString>>
projectShaderReferences(const std::filesystem::path &project_root) {
    std::vector<std::pair<QString, QString>> shaders;
    if (project_root.empty()) return shaders;
    const auto folder = project_root / "shaders";
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return shaders;
    std::filesystem::recursive_directory_iterator walk{
        folder, std::filesystem::directory_options::skip_permission_denied, error};
    for (auto end = std::filesystem::recursive_directory_iterator{}; !error && walk != end;
         walk.increment(error)) {
        std::error_code kind;
        if (!walk->is_regular_file(kind)) continue;
        const auto stage = stageOfExtension(walk->path().extension().string());
        if (stage.isEmpty()) continue;
        auto relative = walk->path().lexically_relative(folder);
        relative.replace_extension();
        const auto stem = relative.generic_u8string();
        shaders.emplace_back(QStringLiteral("project://shaders/") +
                                 QString::fromUtf8(reinterpret_cast<const char *>(stem.data()),
                                                   static_cast<qsizetype>(stem.size())),
                             stage);
    }
    std::ranges::sort(shaders);
    return shaders;
}

struct FeatureParameterForm::Impl {
    struct RowWidgets {
        std::vector<QDoubleSpinBox *> spins;
        QSlider *slider = nullptr;
        int slider_steps = 1000;
        QCheckBox *check = nullptr;
        QComboBox *combo = nullptr;
        QPushButton *swatch = nullptr;
        QDoubleSpinBox *strength = nullptr;
        QToolButton *reset = nullptr;
    };

    FeatureParameterForm &owner;
    std::vector<FeatureParameterRow> rows;
    std::vector<RowWidgets> widgets;
    QJsonObject authored;
    QJsonObject bound;
    std::function<void(bool)> edited;
    QStringList targets;
    std::vector<std::pair<QString, QString>> shaders;
    bool loading = false;

    Impl(FeatureParameterForm &form, const QJsonObject &schema, const QJsonObject &authored_values,
         const QJsonObject &bound_parameters)
        : owner{form}, rows{featureParameterRows(schema)}, authored{authored_values},
          bound{bound_parameters} {
        auto *layout = new QFormLayout(&owner);
        layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        widgets.resize(rows.size());
        for (std::size_t index = 0; index < rows.size(); ++index) {
            auto *field = new QWidget(&owner);
            auto *line = new QHBoxLayout(field);
            line->setContentsMargins(0, 0, 0, 0);
            line->addWidget(build(index, field), 1);
            auto *reset = new QToolButton(field);
            reset->setText(owner.tr("Reset"));
            reset->setObjectName(name(rows[index].label) + QStringLiteral(".reset"));
            reset->setToolTip(resetTip(rows[index]));
            QObject::connect(reset, &QToolButton::clicked, &owner, [this, index] {
                // SC-1: the engine's own default where the schema names one
                // (a materialized feature); otherwise the override goes.
                const auto &row = rows[index];
                for (int at = 0; at < row.names.size(); ++at) {
                    if (hasEngineDefault(row, at))
                        authored[row.names[at]] = row.engine_defaults[at];
                    else
                        authored.remove(row.names[at]);
                }
                load(index);
                if (edited) edited(false);
            });
            widgets[index].reset = reset;
            line->addWidget(reset, 0, Qt::AlignTop);
            auto *label = new QLabel(rows[index].label, &owner);
            label->setToolTip(rows[index].names.join(QStringLiteral(", ")));
            layout->addRow(label, field);
            load(index);
        }
    }

    static QString name(const QString &parameter) {
        return QStringLiteral("pelican.featureParameters.") + parameter;
    }

    static bool hasEngineDefault(const FeatureParameterRow &row, int at) {
        return at < row.engine_defaults.size() && !row.engine_defaults[at].isNull() &&
               !row.engine_defaults[at].isUndefined();
    }

    // What Reset puts back: the engine's default when the schema names one.
    static QJsonValue resetValue(const FeatureParameterRow &row, int at) {
        return hasEngineDefault(row, at) ? row.engine_defaults[at] : row.defaults[at];
    }

    QString resetTip(const FeatureParameterRow &row) const {
        QStringList targets;
        QStringList cut;
        bool materialized = false;
        for (int at = 0; at < row.names.size(); ++at) {
            targets << shown(resetValue(row, at));
            cut << shown(row.defaults[at]);
            materialized = materialized || hasEngineDefault(row, at);
        }
        auto tip = owner.tr("Back to the engine's default: %1").arg(targets.join(QStringLiteral(", ")));
        const bool from_bound = materialized || std::ranges::any_of(row.names, [this](const QString &key) {
                                    return bound.contains(key);
                                });
        if (from_bound)
            tip += QLatin1Char('\n') +
                   owner.tr("This feature was made editable with %1 (bound_parameters); Reset "
                            "does not go back to that.")
                       .arg(cut.join(QStringLiteral(", ")));
        return tip;
    }

    QJsonValue value(std::size_t row, int at) const {
        const auto &key = rows[row].names[at];
        return authored.contains(key) ? authored.value(key) : rows[row].defaults[at];
    }

    static QDoubleSpinBox *spin(QWidget *parent, const QString &object_name,
                                const FeatureParameterRow &row, int at) {
        auto *editor = new QDoubleSpinBox(parent);
        editor->setObjectName(object_name);
        const bool integer = row.type == QStringLiteral("int");
        const auto &range = row.ranges[static_cast<std::size_t>(at)];
        const double low = range ? range->first : (integer ? -2147483648.0 : -1.0e9);
        const double high = range ? range->second : (integer ? 2147483647.0 : 1.0e9);
        editor->setRange(low, high);
        const double span = high - low;
        editor->setDecimals(integer ? 0 : (range && span <= 1.0 ? 4 : 3));
        editor->setSingleStep(integer ? 1.0 : (range ? std::max(span / 100.0, 0.0001) : 0.1));
        editor->setKeyboardTracking(false);
        editor->setAccelerated(true);
        editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (range)
            editor->setToolTip(QObject::tr("%1 to %2").arg(low).arg(high));
        return editor;
    }

    QWidget *build(std::size_t index, QWidget *parent) {
        const auto &row = rows[index];
        auto &parts = widgets[index];
        using Kind = FeatureParameterRow::Kind;
        if (row.kind == Kind::Boolean) {
            parts.check = new QCheckBox(parent);
            parts.check->setObjectName(name(row.names.front()));
            QObject::connect(parts.check, &QCheckBox::toggled, &owner, [this, index] {
                changed(index, false);
            });
            return parts.check;
        }
        if (row.kind == Kind::RenderTarget || row.kind == Kind::ShaderAsset) {
            parts.combo = new QComboBox(parent);
            parts.combo->setObjectName(name(row.names.front()));
            parts.combo->setToolTip(row.kind == Kind::RenderTarget
                                        ? owner.tr("A render target of the frame")
                                        : owner.tr("A %1 shader of the engine or the project")
                                              .arg(row.stage));
            QObject::connect(parts.combo, &QComboBox::currentIndexChanged, &owner,
                             [this, index] { changed(index, false); });
            return parts.combo;
        }
        auto *box = new QWidget(parent);
        if (row.kind == Kind::Number) {
            auto *line = new QHBoxLayout(box);
            line->setContentsMargins(0, 0, 0, 0);
            auto *editor = spin(box, name(row.names.front()), row, 0);
            parts.spins.push_back(editor);
            line->addWidget(editor, 1);
            if (const auto &range = row.ranges.front()) {
                parts.slider = new QSlider(Qt::Horizontal, box);
                parts.slider->setObjectName(name(row.names.front()) + QStringLiteral(".slider"));
                const double span = range->second - range->first;
                parts.slider_steps = row.type == QStringLiteral("int") && span >= 1.0 && span <= 1000.0
                                         ? static_cast<int>(span)
                                         : 1000;
                parts.slider->setRange(0, parts.slider_steps);
                parts.slider->setSingleStep(1);
                parts.slider->setPageStep(std::max(1, parts.slider_steps / 10));
                parts.slider->setToolTip(editor->toolTip());
                line->addWidget(parts.slider, 2);
                QObject::connect(parts.slider, &QSlider::valueChanged, &owner, [this, index](int) {
                    if (loading) return;
                    auto &own = widgets[index];
                    const QSignalBlocker blocker{own.spins.front()};
                    own.spins.front()->setValue(sliderValue(index));
                    changed(index, true);
                });
            }
            QObject::connect(editor, qOverload<double>(&QDoubleSpinBox::valueChanged), &owner,
                             [this, index](double) {
                                 if (loading) return;
                                 placeSlider(index);
                                 changed(index, true);
                             });
            return box;
        }
        if (row.kind == Kind::Vector) {
            auto *line = new QHBoxLayout(box);
            line->setContentsMargins(0, 0, 0, 0);
            for (int at = 0; at < row.names.size(); ++at) {
                auto *caption = new QLabel(row.names[at].mid(row.label.size() + 1), box);
                caption->setFont(studioSmallFont());
                line->addWidget(caption);
                auto *editor = spin(box, name(row.names[at]), row, at);
                parts.spins.push_back(editor);
                line->addWidget(editor, 1);
                QObject::connect(editor, qOverload<double>(&QDoubleSpinBox::valueChanged), &owner,
                                 [this, index](double) { changed(index, true); });
            }
            return box;
        }
        // A colour: the swatch (and the strength when the range is HDR), the
        // components' numbers under it.
        auto *column = new QVBoxLayout(box);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(2);
        auto *top = new QHBoxLayout;
        parts.swatch = new QPushButton(box);
        parts.swatch->setObjectName(name(row.label) + QStringLiteral(".swatch"));
        parts.swatch->setFixedSize(48, 20);
        parts.swatch->setAutoDefault(false);
        parts.swatch->setToolTip(owner.tr("Pick a colour. The values are stored linear, as picked."));
        top->addWidget(parts.swatch);
        if (hdr(index)) {
            auto *caption = new QLabel(owner.tr("Strength"), box);
            caption->setFont(studioSmallFont());
            top->addWidget(caption);
            parts.strength = new QDoubleSpinBox(box);
            parts.strength->setObjectName(name(row.label) + QStringLiteral(".strength"));
            parts.strength->setRange(0.01, maxOfRange(index));
            parts.strength->setDecimals(3);
            parts.strength->setSingleStep(0.1);
            parts.strength->setKeyboardTracking(false);
            parts.strength->setToolTip(
                owner.tr("Multiplies the colour: the stored components are colour x strength."));
            top->addWidget(parts.strength, 1);
            QObject::connect(parts.strength, qOverload<double>(&QDoubleSpinBox::valueChanged),
                             &owner, [this, index](double) { strengthChanged(index); });
        }
        top->addStretch(1);
        column->addLayout(top);
        auto *numbers = new QHBoxLayout;
        for (int at = 0; at < row.names.size(); ++at) {
            auto *caption =
                new QLabel(row.names[at].mid(row.label.size() + 1).toUpper(), box);
            caption->setFont(studioSmallFont());
            numbers->addWidget(caption);
            auto *editor = spin(box, name(row.names[at]), row, at);
            parts.spins.push_back(editor);
            numbers->addWidget(editor, 1);
            QObject::connect(editor, qOverload<double>(&QDoubleSpinBox::valueChanged), &owner,
                             [this, index](double) {
                                 if (loading) return;
                                 showColour(index, true);
                                 changed(index, true);
                             });
        }
        column->addLayout(numbers);
        QObject::connect(parts.swatch, &QPushButton::clicked, &owner, [this, index] { pick(index); });
        return box;
    }

    double maxOfRange(std::size_t index) const {
        double high = 1.0;
        for (const auto &range : rows[index].ranges)
            if (range) high = std::max(high, range->second);
        return high;
    }

    bool hdr(std::size_t index) const { return maxOfRange(index) > 1.0; }

    double sliderValue(std::size_t index) const {
        const auto &range = *rows[index].ranges.front();
        const auto &parts = widgets[index];
        const double fraction = static_cast<double>(parts.slider->value()) / parts.slider_steps;
        const double result = range.first + fraction * (range.second - range.first);
        return rows[index].type == QStringLiteral("int") ? std::round(result) : result;
    }

    void placeSlider(std::size_t index) {
        auto &parts = widgets[index];
        if (parts.slider == nullptr) return;
        const auto &range = *rows[index].ranges.front();
        const double span = range.second - range.first;
        const double fraction = span > 0.0 ? (parts.spins.front()->value() - range.first) / span : 0.0;
        const QSignalBlocker blocker{parts.slider};
        parts.slider->setValue(static_cast<int>(std::lround(fraction * parts.slider_steps)));
    }

    // The swatch shows the colour without its strength; `measure` takes the
    // strength from the components again (they were typed).
    void showColour(std::size_t index, bool measure) {
        auto &parts = widgets[index];
        double strength = 1.0;
        if (parts.strength != nullptr) {
            double largest = 0.0;
            for (int at = 0; at < std::min<int>(3, static_cast<int>(parts.spins.size())); ++at)
                largest = std::max(largest, parts.spins[at]->value());
            if (measure || parts.strength->value() <= 0.0) {
                const QSignalBlocker blocker{parts.strength};
                parts.strength->setValue(std::max(1.0, largest));
                parts.swatch->setProperty("pelican_strength", parts.strength->value());
            }
            strength = std::max(parts.strength->value(), 1e-6);
        }
        const auto channel = [&](int at) {
            if (at >= static_cast<int>(parts.spins.size())) return 1.0;
            return std::clamp(parts.spins[at]->value() / strength, 0.0, 1.0);
        };
        const auto colour = QColor::fromRgbF(static_cast<float>(channel(0)),
                                             static_cast<float>(channel(1)),
                                             static_cast<float>(channel(2)));
        parts.swatch->setStyleSheet(
            QStringLiteral("background-color: %1; border: 1px solid #808080;").arg(colour.name()));
    }

    void strengthChanged(std::size_t index) {
        if (loading) return;
        auto &parts = widgets[index];
        // The colour as the swatch shows it, at the new strength.
        const double previous =
            std::max(parts.swatch->property("pelican_strength").toDouble(), 1e-6);
        const double now = parts.strength->value();
        for (int at = 0; at < std::min<int>(3, static_cast<int>(parts.spins.size())); ++at) {
            const QSignalBlocker blocker{parts.spins[at]};
            parts.spins[at]->setValue(parts.spins[at]->value() / previous * now);
        }
        parts.swatch->setProperty("pelican_strength", now);
        showColour(index, false);
        changed(index, true);
    }

    void pick(std::size_t index) {
        auto &parts = widgets[index];
        const double strength =
            parts.strength != nullptr ? std::max(parts.strength->value(), 1e-6) : 1.0;
        const auto channel = [&](int at) {
            return at < static_cast<int>(parts.spins.size())
                       ? std::clamp(parts.spins[at]->value() / (at < 3 ? strength : 1.0), 0.0, 1.0)
                       : 1.0;
        };
        const bool alpha = parts.spins.size() > 3;
        const auto initial = QColor::fromRgbF(static_cast<float>(channel(0)),
                                              static_cast<float>(channel(1)),
                                              static_cast<float>(channel(2)),
                                              static_cast<float>(channel(3)));
        const auto picked = QColorDialog::getColor(
            initial, &owner, rows[index].label,
            alpha ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions{});
        if (!picked.isValid()) return;
        const std::array<double, 4> components{picked.redF(), picked.greenF(), picked.blueF(),
                                               picked.alphaF()};
        for (int at = 0; at < static_cast<int>(parts.spins.size()); ++at) {
            const QSignalBlocker blocker{parts.spins[at]};
            parts.spins[at]->setValue(components[static_cast<std::size_t>(at)] *
                                      (at < 3 ? strength : 1.0));
        }
        showColour(index, false);
        changed(index, false);
    }

    void changed(std::size_t index, bool continuous) {
        if (loading) return;
        const auto &row = rows[index];
        const auto &parts = widgets[index];
        using Kind = FeatureParameterRow::Kind;
        if (row.kind == Kind::Boolean) {
            authored[row.names.front()] = parts.check->isChecked();
        } else if (row.kind == Kind::RenderTarget || row.kind == Kind::ShaderAsset) {
            const auto text = parts.combo->currentData().toString();
            if (text.isEmpty()) return;
            authored[row.names.front()] = text;
        } else {
            for (int at = 0; at < row.names.size(); ++at) {
                const double number = parts.spins[static_cast<std::size_t>(at)]->value();
                authored[row.names[at]] =
                    row.type == QStringLiteral("int")
                        ? QJsonValue(static_cast<qint64>(std::llround(number)))
                        : QJsonValue(number);
            }
        }
        showReset(index);
        if (edited) edited(continuous);
    }

    void showReset(std::size_t index) {
        const auto &row = rows[index];
        bool differs = false;
        for (int at = 0; at < row.names.size(); ++at)
            differs = differs || ((authored.contains(row.names[at]) || hasEngineDefault(row, at)) &&
                                  !sameValue(value(index, at), resetValue(row, at)));
        widgets[index].reset->setVisible(differs);
    }

    void fillCombo(std::size_t index) {
        const auto &row = rows[index];
        auto &combo = *widgets[index].combo;
        QStringList choices;
        if (row.kind == FeatureParameterRow::Kind::RenderTarget) {
            choices = targets;
        } else {
            for (const auto &[reference, stage] : shaders)
                if (row.stage.isEmpty() || stage == row.stage) choices << reference;
        }
        const auto fallback = row.defaults.first().toString();
        if (!fallback.isEmpty() && !choices.contains(fallback)) choices.prepend(fallback);
        const auto current = value(index, 0).toString();
        if (!current.isEmpty() && !choices.contains(current)) choices.prepend(current);
        combo.clear();
        for (const auto &choice : choices)
            combo.addItem(choice == fallback ? owner.tr("%1 (default)").arg(choice) : choice, choice);
        combo.setCurrentIndex(combo.findData(current));
    }

    void load(std::size_t index) {
        loading = true;
        const auto &row = rows[index];
        auto &parts = widgets[index];
        using Kind = FeatureParameterRow::Kind;
        if (row.kind == Kind::Boolean) {
            const QSignalBlocker blocker{parts.check};
            parts.check->setChecked(value(index, 0).toBool());
        } else if (row.kind == Kind::RenderTarget || row.kind == Kind::ShaderAsset) {
            const QSignalBlocker blocker{parts.combo};
            fillCombo(index);
        } else {
            for (int at = 0; at < row.names.size(); ++at) {
                auto *editor = parts.spins[static_cast<std::size_t>(at)];
                const QSignalBlocker blocker{editor};
                editor->setValue(value(index, at).toDouble());
            }
            placeSlider(index);
            if (row.kind == Kind::Color) showColour(index, true);
        }
        showReset(index);
        loading = false;
    }
};

FeatureParameterForm::FeatureParameterForm(const QJsonObject &schema, const QJsonObject &authored,
                                           const QJsonObject &bound_parameters, QWidget *parent)
    : QWidget{parent} {
    setObjectName(QStringLiteral("pelican.featureParameters.form"));
    impl_ = std::make_unique<Impl>(*this, schema, authored, bound_parameters);
}

FeatureParameterForm::~FeatureParameterForm() = default;

const QJsonObject &FeatureParameterForm::authored() const noexcept { return impl_->authored; }

void FeatureParameterForm::setAuthored(const QJsonObject &authored) {
    impl_->authored = authored;
    for (std::size_t index = 0; index < impl_->rows.size(); ++index) impl_->load(index);
}

void FeatureParameterForm::setEditedCallback(std::function<void(bool continuous)> edited) {
    impl_->edited = std::move(edited);
}

void FeatureParameterForm::setRenderTargets(const QStringList &names) {
    impl_->targets = names;
    for (std::size_t index = 0; index < impl_->rows.size(); ++index)
        if (impl_->rows[index].kind == FeatureParameterRow::Kind::RenderTarget) impl_->load(index);
}

void FeatureParameterForm::setShaders(const std::vector<std::pair<QString, QString>> &shaders) {
    impl_->shaders = shaders;
    for (std::size_t index = 0; index < impl_->rows.size(); ++index)
        if (impl_->rows[index].kind == FeatureParameterRow::Kind::ShaderAsset) impl_->load(index);
}

} // namespace PelicanStudio
