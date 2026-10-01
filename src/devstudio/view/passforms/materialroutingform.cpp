#include "materialroutingform.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QSignalBlocker>

namespace PelicanStudio {

using passform::Json;
using passform::text;

MaterialRoutingForm::MaterialRoutingForm(const QString &object_prefix, QWidget *parent)
    : PassForm(object_prefix, parent) {
    setObjectName(childName("materialRouting"));
    setToolTip(tr("Preset-wide routing shared by all graphs: the material pass "
                  "each kind of material is drawn by (hybrid_auto_v1)."));
    auto *form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    enabled_ = new QCheckBox(tr("Route materials to passes by kind"), this);
    enabled_->setObjectName(childName("materialRouting.enabled"));
    form->addRow(enabled_);
    const std::array labels{tr("Deferred"), tr("Forward opaque"), tr("Forward transparent")};
    for (std::size_t index = 0; index < routes.size(); ++index) {
        auto *combo = new QComboBox(this);
        combo->setObjectName(childName("materialRouting.") + text(routes[index]));
        combo->setToolTip(tr("routes.%1").arg(text(routes[index])));
        form->addRow(labels[index], combo);
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this] { notifyChanged(); });
        rows_[index] = combo;
    }
    QObject::connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
        for (auto *combo : rows_) combo->setEnabled(on);
        if (on && !loading()) fillEmptyRoutes();
        notifyChanged();
    });
    reset();
}

void MaterialRoutingForm::reset() { load(nullptr); }

void MaterialRoutingForm::setPassCandidates(
    const std::vector<std::pair<std::string, std::string>> &passes) {
    passes_ = passes;
    for (auto *combo : rows_) {
        const QSignalBlocker blocker{combo};
        const QString current = combo->currentData().toString();
        combo->clear();
        combo->addItem(tr("(none)"), QString{});
        for (const auto &[name, contract] : passes_) combo->addItem(text(name), text(name));
        passform::selectData(*combo, current);
    }
}

void MaterialRoutingForm::load(const Json &routing) {
    const Loading loading{*this};
    original_ = routing.is_object() ? routing : Json(nullptr);
    enabled_->setChecked(original_.is_object());
    const Json routes_json = original_.is_object()
                                 ? original_.value("routes", Json::object())
                                 : Json::object();
    for (std::size_t index = 0; index < routes.size(); ++index) {
        auto *combo = rows_[index];
        const QSignalBlocker blocker{combo};
        if (combo->count() == 0) combo->addItem(tr("(none)"), QString{});
        const auto found = routes_json.find(routes[index]);
        passform::selectData(*combo, found != routes_json.end() && found->is_string()
                                         ? text(found->get<std::string>())
                                         : QString{});
        combo->setEnabled(enabled_->isChecked());
    }
}

void MaterialRoutingForm::fillEmptyRoutes() {
    for (std::size_t index = 0; index < routes.size(); ++index) {
        auto *combo = rows_[index];
        if (!combo->currentData().toString().isEmpty()) continue;
        const std::string contract = std::string{routes[index]} + "_v1";
        for (const auto &[name, pass_contract] : passes_) {
            if (pass_contract == contract || name == routes[index]) {
                passform::selectData(*combo, text(name));
                break;
            }
        }
    }
}

Json MaterialRoutingForm::routing() const {
    if (!enabled_->isChecked()) return nullptr;
    Json result = original_.is_object() ? original_ : Json::object();
    if (!result.contains("policy")) result["policy"] = "hybrid_auto_v1";
    Json routes_json = result.value("routes", Json::object());
    if (!routes_json.is_object()) routes_json = Json::object();
    for (std::size_t index = 0; index < routes.size(); ++index) {
        const auto pass = rows_[index]->currentData().toString();
        if (pass.isEmpty())
            routes_json.erase(routes[index]);
        else
            routes_json[routes[index]] = pass.toStdString();
    }
    result["routes"] = std::move(routes_json);
    return result;
}

} // namespace PelicanStudio
