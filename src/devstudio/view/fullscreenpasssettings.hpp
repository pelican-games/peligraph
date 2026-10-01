#pragma once

#include <nlohmann/json_fwd.hpp>

#include <QString>
#include <QWidget>

#include <array>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

class QCheckBox;
class QComboBox;
class QGridLayout;

namespace PelicanStudio {

// G-3. What a fullscreen pass asks the engine for besides its shaders: the
// scene lights (`uses_light_data`) and a push-constant block
// (`push_constants`). The Pass Editor shows it as one row; the draft and the
// edit_preset_pass request take it from here.
class FullscreenPassInterfaceRow final : public QWidget {
    QCheckBox *uses_light_data_ = nullptr;
    QComboBox *push_constants_ = nullptr;
    std::function<void()> changed_;

  public:
    // UX-5b: the Graph's Pass fields hold one too, named under their own
    // prefix (`<prefix>.usesLightData`, `<prefix>.pushConstants`).
    explicit FullscreenPassInterfaceRow(
        QWidget *parent = nullptr,
        const QString &prefix = QStringLiteral("pelican.fullscreenPass"));

    void setChangedAction(std::function<void()> action);
    // Back to a pass that asks for nothing.
    void reset();
    // Shows what a declaration asks for. A push_constants value this row does
    // not name is kept as an entry of its own, so reading it loses nothing.
    void load(const nlohmann::json &declaration);
    // What a template's shader needs: turns the settings on, never off.
    void require(bool uses_light_data, std::string_view push_constants);

    bool usesLightData() const;
    // "" when the pass asks for no push constants.
    std::string pushConstants() const;
    // Writes the two fields into a draft; a field left at its default is
    // absent, as the engine reads an absent one.
    void apply(nlohmann::json &draft) const;
    // An edit_preset_pass request carries a field only when it differs from
    // the declaration it edits; null removes it.
    void addEditParams(const nlohmann::json &original,
                       nlohmann::json &params) const;
};

} // namespace PelicanStudio
