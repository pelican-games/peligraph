#pragma once

#include "../model/graphcanvasmodel.hpp"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace PelicanStudio {

class GraphCanvasHeldEdits;

// UX-12a: where a parameter editor finds what it offers -- the nodes of the
// scope (node_ref), the project's files (asset, shader), the engine's
// shaders -- and, for a string the table types only as a string, the values
// Studio knows (pose.clip_sample's clip: the animations of the rig's file),
// given the parameters around it.
struct GraphParameterChoices {
    const GraphCanvasModel *model = nullptr;
    std::string scope;
    std::string node;
    std::string op;
    std::filesystem::path project_root;
    std::function<std::optional<std::vector<std::string>>(const std::string &name,
                                                          const nlohmann::json &around)>
        strings;
};

// UX-12a, decision 1: one parameter's editor, made from one entry of the
// catalog's `parameters` (engine CT-3) by its kind:
//   - number / integer: a spin (UX-10's), with a slider when min and max are
//     both given and near; the spin is not held to the range -- the engine
//     says what is wrong with a value outside it (decision 4);
//   - bool: a check; enum: a combo; string: a line (a combo when Studio
//     knows the values); color: a swatch and R / G / B / A; vec2..4: spins;
//   - asset: the project's files (by what the name and description say it
//     is: a model, a sound, an image; else all) and Import...; shader: the
//     project's and the engine's shaders; node_ref: the scope's nodes;
//   - object with fields: a heading that folds, the fields under it; object
//     with items: key / value rows, + and x; array: rows, + and x;
//   - expression: the f line ({"$expr": ...}); an entry marked `expression`
//     has the f button beside its own field, as the loop's Count;
//   - json: the value as a JSON line.
// value() is null while the parameter is not written (the engine's default
// applies); an entry with a default offers Reset while the value differs.
// Widgets are named `<name>` (the main one), `<name>.slider`, `.swatch`,
// `.reset`, `.import`, `.f`, `.add`, `.<field>` for an object's field and
// `.<index>` / `.<index>.remove` for an array's or a map's row (the key of a
// map's row is `.<index>.key`).
class GraphParameterEditor final : public QWidget {
  public:
    GraphParameterEditor(nlohmann::json schema, std::shared_ptr<const GraphParameterChoices> choices,
                         const QString &name, const std::string &hint, QWidget *parent = nullptr);
    ~GraphParameterEditor() override;

    [[nodiscard]] const nlohmann::json &schema() const noexcept { return schema_; }
    [[nodiscard]] std::string kind() const;
    // Null: not written.
    void setValue(const nlohmann::json &value);
    [[nodiscard]] nlohmann::json value() const;
    // `continuous`: a spin or a slider moving (the owner gathers those).
    void setEdited(std::function<void(bool continuous)> edited);
    // The parameters around this one, for a string Studio knows the values
    // of: its list is read again.
    void refreshChoices(const nlohmann::json &around);
    // Decision 4: the path the engine named under this parameter ("" for
    // itself, "tile_width", "[0].interpolation"); the field there is red.
    void setInvalid(std::optional<std::string> path);
    [[nodiscard]] bool invalid() const noexcept { return invalid_; }
    // Whether a field of it has the keyboard (a document read leaves it be).
    [[nodiscard]] bool typing() const;
    void setEditable(bool editable);

  private:
    struct Parts;
    void build();
    void buildNumber();
    void buildVector(int count, bool colour);
    void buildCombo();
    void buildObject();
    void buildRows();
    void addRow(const nlohmann::json &value, const std::string &key);
    void load();
    void fillCombo();
    void changed(bool continuous);
    void showReset();
    void showColour();
    void showExpression(bool expression);
    void importAsset();
    [[nodiscard]] QString child(const QString &suffix) const;
    [[nodiscard]] nlohmann::json widgetValue() const;
    [[nodiscard]] std::vector<std::string> comboChoices() const;

    nlohmann::json schema_;
    std::shared_ptr<const GraphParameterChoices> choices_;
    QString name_;
    std::string hint_;
    nlohmann::json value_;
    nlohmann::json around_ = nlohmann::json::object();
    bool set_ = false;
    // Changed here since the value was last given (a combo that had
    // "(choose)" keeps it while it is picked from).
    bool picked_ = false;
    bool loading_ = false;
    bool invalid_ = false;
    std::function<void(bool)> edited_;
    std::unique_ptr<Parts> parts_;
};

// The files of a project an asset parameter can name, as project://<path>,
// sorted (dot directories skipped): those with one of `extensions`
// (lower-case, with the dot), or every file for none.
std::vector<std::string> projectAssetReferences(const std::filesystem::path &project_root,
                                                const std::vector<std::string> &extensions);
// The extensions an asset parameter takes, from the op, its name and what
// its description (and its parent's) say: a model's (.gltf / .glb / .vrm),
// a sound's (.wav / .ogg), an image's (.ktx2 / .png); none when nothing says.
std::vector<std::string> assetParameterExtensions(std::string_view op, std::string_view name,
                                                  std::string_view hint);

// UX-12a, decisions 1, 2 and 4: the selected node's fields made from its op's
// table, beside the Graph tab's canvas. An op with fields of its own
// (render.image / fullscreen / raster / compute / material_pass,
// value.curve / value.playback, render.scalar, audio.*, ui.*) keeps them, and
// these list under More parameters only the names those do not show; the
// place and view fields' names are left to them too. A required parameter's
// name is marked, and red while it is not written; the parameter the engine
// refused (graph.render_node_parameter_invalid@1) is red with the refusal's
// sentence under it (`<prefix>.<name>.problem`). Each change is one
// set_node_parameters of the node's whole parameters (a drag gathered over
// 150 ms); a parameter an edge feeds is off and says so. Named
// `<prefix>` (`pelican.graphCanvasParameters`), `<prefix>.<name>...` per
// GraphParameterEditor, `<prefix>.more` for the fold.
class GraphCanvasParameterFields final : public QWidget {
  public:
    GraphCanvasParameterFields(GraphCanvasModel &model, std::function<void()> submitted,
                               QWidget *parent = nullptr,
                               const QString &prefix = QStringLiteral("pelican.graphCanvasParameters"));
    ~GraphCanvasParameterFields() override;

    void showNode(const GraphCanvasNode *node, const std::string &scope);
    void followEditable();
    void setProjectRoot(const std::filesystem::path &root);
    // Which ops these fields are for (all with a table, by default).
    void setOps(std::function<bool(std::string_view op)> shows) { shows_ = std::move(shows); }
    // GraphParameterChoices::strings.
    void setStrings(std::function<std::optional<std::vector<std::string>>(
                        const std::string &name, const nlohmann::json &around)>
                        strings) {
        strings_ = std::move(strings);
    }
    // Called after the fields were loaded or changed, with the parameters.
    void setFollow(std::function<void(const nlohmann::json &parameters)> follow) {
        follow_ = std::move(follow);
    }
    [[nodiscard]] bool valuePending() const noexcept;
    void setPendingChanged(std::function<void()> changed);
    // The parameters as the fields hold them now.
    [[nodiscard]] nlohmann::json parameters() const;

    // An op whose fields of its own show part of its parameters, and the
    // names those show.
    [[nodiscard]] static bool hasOwnFields(std::string_view op);
    [[nodiscard]] static std::vector<std::string> ownFieldNames(std::string_view op);

  private:
    struct Row {
        std::string name;
        QLabel *label = nullptr;
        QLabel *problem = nullptr;
        GraphParameterEditor *editor = nullptr;
        bool required = false;
        std::vector<std::string> alternatives;
        // An input edge of the same name feeds it.
        bool connected = false;
    };
    void rebuild(const GraphCanvasNode &node, const nlohmann::json &table);
    void load(const GraphCanvasNode &node);
    void showProblems();
    void edited(bool continuous);
    void send();
    void release();

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    QString prefix_;
    std::filesystem::path project_root_;
    std::function<bool(std::string_view)> shows_;
    std::function<std::optional<std::vector<std::string>>(const std::string &, const nlohmann::json &)>
        strings_;
    std::function<void(const nlohmann::json &)> follow_;
    std::string node_;
    std::string scope_;
    std::string op_;
    std::string shown_;
    nlohmann::json loaded_ = nlohmann::json::object();
    std::vector<std::string> connected_inputs_;
    std::vector<Row> rows_;
    QLabel *heading_ = nullptr;
    QToolButton *more_ = nullptr;
    QWidget *body_ = nullptr;
    QFormLayout *form_ = nullptr;
    QLabel *note_ = nullptr;
    QTimer *gather_ = nullptr;
    std::unique_ptr<GraphCanvasHeldEdits> held_;
};

// The value a parameter shows when it is not written: its default, else
// nothing (null).
[[nodiscard]] nlohmann::json graphParameterDefault(const nlohmann::json &schema);

} // namespace PelicanStudio
