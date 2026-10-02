#pragma once

#include "passform.hpp"

#include <string>
#include <string_view>

class QTableWidget;
class QPushButton;

namespace PelicanStudio {

// UX-4a. A fullscreen or raster pass's `shader.defines`: one define a row,
// its name and its value ("NAME" or "NAME=VALUE"), with Add and Remove. A row
// the engine would refuse -- a name it defines itself when it compiles the
// pass, a name that is not a C identifier, a name given twice -- is shown red
// with the engine's sentence (passshaderdefines.cpp's rule, kept the same
// here because Studio does not link the engine).
class ShaderDefinesForm final : public PassForm {
    Q_OBJECT

  public:
    explicit ShaderDefinesForm(const QString &object_prefix,
                               QWidget *parent = nullptr);

    // shader.defines as the declaration holds it (null: absent).
    void load(const nlohmann::json &defines);
    void reset();
    // The name the refusal sentences use for the pass.
    void setPassName(const std::string &pass_name);
    // RSX. The rendering section's own shader_defines (added to every
    // shader's compile) rather than a pass's: the names the engine defines
    // when it compiles a pass are not refused there (a feature's
    // PELICAN_FEATURE_* defines live in that list), and the sentences name
    // rendering.shader_defines. A name that is not a C identifier, or a name
    // given twice, is still refused.
    void setRenderingSection(bool rendering_section);
    // UX-9, decision 2: a cell being typed in takes what it holds now (its
    // change is announced as any edit of a row is), without closing it.
    void finishEditing();

    // shader.defines, or null when there is none.
    [[nodiscard]] nlohmann::json defines() const;
    // The sentence for the first row the engine would refuse; empty when it
    // takes them all.
    [[nodiscard]] QString refusal() const;
    // The refusal of one row; empty when it is taken.
    [[nodiscard]] QString rowRefusal(int row) const;

    // VI-1's rule: whether the engine defines `name` itself when it compiles
    // a fullscreen or raster pass.
    static bool engineReserved(std::string_view name);
    // Why the engine refuses entry `index` of `defines`; empty when it takes
    // it. The sentence is the engine's.
    static std::string entryRefusal(const nlohmann::json &defines,
                                    std::size_t index,
                                    const std::string &pass_name);

  private:
    void addRow(const QString &name, const QString &value, bool equals,
                const nlohmann::json &raw);
    void showRefusals();
    [[nodiscard]] std::string refusalAt(const nlohmann::json &defines,
                                        std::size_t index) const;

    QTableWidget *table_ = nullptr;
    QPushButton *add_ = nullptr;
    QPushButton *remove_ = nullptr;
    std::string pass_name_;
    bool rendering_section_ = false;
    bool filling_ = false;
};

} // namespace PelicanStudio
