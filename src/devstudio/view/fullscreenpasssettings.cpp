#include "fullscreenpasssettings.hpp"

#include <nlohmann/json.hpp>

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStringList>
#include <QToolButton>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <optional>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString text(std::string_view value) {
    return QString::fromUtf8(value.data(),
                             static_cast<qsizetype>(value.size()));
}

// Selects the entry whose value is `value`; a spelling the list does not
// have is added, so what the JSON says is what the row shows.
void selectValue(QComboBox &combo, const std::string &value) {
    int found = combo.findData(text(value));
    if (found < 0) {
        combo.addItem(text(value), text(value));
        found = combo.count() - 1;
    }
    combo.setCurrentIndex(found);
}

} // namespace

// --- FullscreenPassInterfaceRow

FullscreenPassInterfaceRow::FullscreenPassInterfaceRow(QWidget *parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("pelican.fullscreenPass.interface"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    uses_light_data_ = new QCheckBox(tr("Uses scene lights"), this);
    uses_light_data_->setObjectName(
        QStringLiteral("pelican.fullscreenPass.usesLightData"));
    uses_light_data_->setToolTip(
        tr("uses_light_data: the pass reads the scene lights "
           "(pelicanLights, pelican_lighting_v1.glsl)."));
    push_constants_ = new QComboBox(this);
    push_constants_->setObjectName(
        QStringLiteral("pelican.fullscreenPass.pushConstants"));
    push_constants_->addItem(tr("No push constants"), QString{});
    push_constants_->addItem(tr("Camera position"),
                             QStringLiteral("camera_position"));
    push_constants_->addItem(tr("Projection and view"),
                             QStringLiteral("projection_view"));
    push_constants_->setToolTip(
        tr("push_constants: what the engine declares for the pass. A "
           "lighting pass asks for the camera position."));
    layout->addWidget(uses_light_data_);
    layout->addWidget(push_constants_, 1);
    QObject::connect(uses_light_data_, &QCheckBox::toggled, this, [this] {
        if (changed_) changed_();
    });
    QObject::connect(push_constants_,
                     qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this] {
                         if (changed_) changed_();
                     });
}

void FullscreenPassInterfaceRow::setChangedAction(
    std::function<void()> action) {
    changed_ = std::move(action);
}

void FullscreenPassInterfaceRow::reset() {
    const QSignalBlocker lights{uses_light_data_};
    const QSignalBlocker push{push_constants_};
    uses_light_data_->setChecked(false);
    while (push_constants_->count() > 3) push_constants_->removeItem(3);
    push_constants_->setCurrentIndex(0);
}

void FullscreenPassInterfaceRow::load(const Json &declaration) {
    reset();
    const QSignalBlocker lights{uses_light_data_};
    const QSignalBlocker push{push_constants_};
    const auto uses = declaration.find("uses_light_data");
    uses_light_data_->setChecked(uses != declaration.end() &&
                                 uses->is_boolean() && uses->get<bool>());
    const auto push_value = declaration.find("push_constants");
    if (push_value != declaration.end() && push_value->is_string() &&
        push_value->get<std::string>() != "none")
        selectValue(*push_constants_, push_value->get<std::string>());
}

void FullscreenPassInterfaceRow::require(bool uses_light_data,
                                         std::string_view push_constants) {
    if (uses_light_data) uses_light_data_->setChecked(true);
    if (!push_constants.empty())
        selectValue(*push_constants_, std::string{push_constants});
}

bool FullscreenPassInterfaceRow::usesLightData() const {
    return uses_light_data_->isChecked();
}

std::string FullscreenPassInterfaceRow::pushConstants() const {
    return push_constants_->currentData().toString().toStdString();
}

void FullscreenPassInterfaceRow::apply(Json &draft) const {
    if (usesLightData())
        draft["uses_light_data"] = true;
    else
        draft.erase("uses_light_data");
    const auto push = pushConstants();
    if (push.empty())
        draft.erase("push_constants");
    else
        draft["push_constants"] = push;
}

void FullscreenPassInterfaceRow::addEditParams(const Json &original,
                                               Json &params) const {
    const auto uses = original.find("uses_light_data");
    const bool was_using =
        uses != original.end() && uses->is_boolean() && uses->get<bool>();
    if (was_using != usesLightData())
        params["uses_light_data"] = usesLightData() ? Json(true) : Json(nullptr);
    auto was_pushing = original.value("push_constants", std::string{});
    if (was_pushing == "none") was_pushing.clear();
    if (was_pushing != pushConstants())
        params["push_constants"] =
            pushConstants().empty() ? Json(nullptr) : Json(pushConstants());
}

} // namespace PelicanStudio
