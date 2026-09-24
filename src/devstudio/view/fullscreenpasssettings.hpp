#pragma once

#include <nlohmann/json_fwd.hpp>

#include <QString>
#include <QWidget>

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
    explicit FullscreenPassInterfaceRow(QWidget *parent = nullptr);

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

// G-3. Load, Clear (with its colour) or Don't care, and Store or Don't care,
// for each colour output. The `output.color` JSON field of the Pass Editor is
// the source of truth for load_op and store_op: this editor shows what that
// field says and writes a change back into it, so the two never disagree.
// The clear colour is not an attachment field in the engine's schema; it is
// the pass's `clear_colors`, keyed by output name, and this editor holds it.
class FullscreenOutputOpsEditor final : public QWidget {
    struct Row;
    QGridLayout *grid_ = nullptr;
    std::vector<Row> rows_;
    std::vector<std::string> targets_;
    std::map<std::string, QString, std::less<>> clear_colors_;
    std::function<void(const QString &)> write_json_;
    std::function<void()> changed_;
    QString json_text_;
    bool syncing_ = false;

    void rebuild(const std::vector<std::string> &targets);
    void rowChanged(std::size_t index);
    void showClearColor(std::size_t index);

  public:
    explicit FullscreenOutputOpsEditor(QWidget *parent = nullptr);
    ~FullscreenOutputOpsEditor() override;

    // Where a Load or Store change goes: the output.color JSON text.
    void setJsonWriter(std::function<void(const QString &)> writer);
    // Called when a clear colour changes (it is not in the JSON field).
    void setChangedAction(std::function<void()> action);
    void reset();
    // Re-reads the rows from the selected colour outputs and the JSON text.
    // Text that does not parse or does not match the outputs leaves the rows
    // disabled; the draft already says what is wrong with it.
    void sync(const std::vector<std::string> &colors, const QString &json_text);
    void loadClearColors(const nlohmann::json &declaration);
    // `clear_colors` for the current outputs: every output with a colour
    // typed, as [r, g, b, a]. Null when there is none. Throws for a colour
    // that is not three or four numbers.
    nlohmann::json clearColors() const;

    // The JSON text for a new list of colour outputs: an entry whose target
    // is still selected keeps its fields, a new output is a plain name, and a
    // list of plain names is no text at all. Text that does not parse is
    // returned unchanged.
    static QString reconciledJson(const std::vector<std::string> &colors,
                                  const QString &json_text);
};

} // namespace PelicanStudio
