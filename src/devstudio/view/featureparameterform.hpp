#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QDoubleSpinBox;

namespace PelicanStudio {

// UX-10, decision 1. How a feature's parameter_schema (get_render_features'
// feature_instances[].parameter_schema) is laid out as rows, so nothing whose
// type the schema already says is typed:
//   - a bool is a check box;
//   - an int or a float is a spin, with a slider beside it when the schema
//     gives a range;
//   - scalars named <stem>_r, <stem>_g, <stem>_b (and <stem>_a) are one
//     colour row: a swatch that opens the colour dialog, the components'
//     spins under it, and a strength spin when the range goes past 1 (the
//     values stay linear and inside the range);
//   - scalars named <stem>_x, <stem>_y, <stem>_z (and <stem>_w or
//     <stem>_offset) are one vector row of 3 or 4 spins;
//   - render_targets[] are a combo of the authoring context's render
//     targets, shader_assets[] a combo of the engine's and the project's
//     shaders of the declared stage.
// A row sits where its first scalar is declared.
struct FeatureParameterRow {
    enum class Kind { Number, Boolean, Color, Vector, RenderTarget, ShaderAsset };
    Kind kind = Kind::Number;
    // The scalar's name, or the stem a colour or a vector shares.
    QString label;
    // The parameters the row writes, in the row's order.
    QStringList names;
    // Number: "int" or "float".
    QString type;
    // The schema's default for each name (null when it gives none).
    QJsonArray defaults;
    // SC-1: a materialized feature's declarations carry `engine_default`
    // beside `default` (which is then the value it was cut with); null where
    // the schema gives none. Reset goes back to it when it is there.
    QJsonArray engine_defaults;
    // The schema's range for each name.
    std::vector<std::optional<std::pair<double, double>>> ranges;
    // ShaderAsset: the stage the shader is declared for.
    QString stage;
};

[[nodiscard]] std::vector<FeatureParameterRow> featureParameterRows(const QJsonObject &schema);

// The spin a number row is made of (UX-12a: the Graph's parameter fields use
// it too): no decimals for an integer, bounded by `range` (and saying so in
// its tooltip) or wide without one, keyboard tracking off.
[[nodiscard]] QDoubleSpinBox *makeParameterSpin(QWidget *parent, const QString &object_name,
                                                bool integer,
                                                const std::optional<std::pair<double, double>> &range);

// The project's own shaders, as project://shaders/<stem> with the stage their
// extension says (.comp compute, .frag fragment, .vert vertex, ...).
[[nodiscard]] std::vector<std::pair<QString, QString>>
projectShaderReferences(const std::filesystem::path &project_root);

// The rows of one feature instance, as widgets. `authored` is the instance's
// parameters (the overrides); a row shows the authored value or the schema's
// default. A change writes the row's names into authored() and calls the
// edited callback: `continuous` for a spin or a slider (the owner gathers
// those), false for a pick, a check box or a reset. A row whose value differs
// from the default offers Reset, which removes its names from authored().
// Widgets are named pelican.featureParameters.<name> (one per scalar), with
// .slider, and pelican.featureParameters.<label>.swatch / .strength / .reset.
class FeatureParameterForm final : public QWidget {
  public:
    FeatureParameterForm(const QJsonObject &schema, const QJsonObject &authored,
                         const QJsonObject &bound_parameters, QWidget *parent = nullptr);
    ~FeatureParameterForm() override;

    [[nodiscard]] const QJsonObject &authored() const noexcept;
    // The values read again (another commit, an Undo): no callback.
    void setAuthored(const QJsonObject &authored);
    void setEditedCallback(std::function<void(bool continuous)> edited);
    // The render target names the authoring context lists.
    void setRenderTargets(const QStringList &names);
    // (reference, stage) of every shader that can be picked.
    void setShaders(const std::vector<std::pair<QString, QString>> &shaders);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
