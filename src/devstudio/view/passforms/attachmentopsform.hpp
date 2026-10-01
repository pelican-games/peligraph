#pragma once

#include "passform.hpp"

#include <array>
#include <map>
#include <string>
#include <vector>

class QComboBox;
class QGridLayout;
class QLabel;

namespace PelicanStudio {

// UX-4a (G-3's rows, made a form of their own). Load (Default, Load, Clear,
// Don't care) and Store (Default, Store, Don't care) of each colour output
// and of the depth output, and the clear colour of each colour output.
//
// The form holds `output.color` itself: one entry per colour output, a bare
// name until a Load or Store is chosen for it, then {target, load_op,
// store_op} -- and any other field the entry had (a subresource, say) stays.
// The clear colour is the pass's `clear_colors`, keyed by output name. The
// depth output is a name, or {target, load_op, store_op} once its Load or
// Store is chosen.
class AttachmentOpsForm final : public PassForm {
    Q_OBJECT

    struct Row;

  public:
    explicit AttachmentOpsForm(const QString &object_prefix,
                               QWidget *parent = nullptr);
    ~AttachmentOpsForm() override;

    void reset();
    // output.color, clear_colors and output.depth as the declaration holds
    // them. The colour outputs it names become the form's outputs.
    void load(const nlohmann::json &color, const nlohmann::json &clear_colors,
              const nlohmann::json &depth);
    // The colour outputs, in order: an entry whose target is still among
    // them keeps its fields; a new one is the bare name.
    void setColorTargets(const std::vector<std::string> &targets);
    // The depth output's target ("" for none). Its fields stay.
    void setDepthTarget(const std::string &target);
    // A fullscreen pass has no depth attachment to load or store.
    void setDepthOpsShown(bool shown);

    // output.color: the outputs' names when none has a field besides its
    // target, otherwise the entries.
    [[nodiscard]] nlohmann::json color() const;
    // clear_colors for the current outputs, as [r, g, b, a]; null when none.
    // Throws for a colour that is not three or four numbers.
    [[nodiscard]] nlohmann::json clearColors() const;
    // output.depth; null when there is no depth target.
    [[nodiscard]] nlohmann::json depth() const;

    // T-1. The colour button beside a clear colour: what a picked colour is
    // written into the field as ("r, g, b, a", each 0..1 to three places, a
    // whole number without them), and the colour the picker opens on for the
    // field's text (black for text that is not three or four numbers).
    static QString clearColorText(double red, double green, double blue,
                                  double alpha);
    static std::array<double, 4> clearColorComponents(const QString &text);

  private:
    void rebuild();
    void showRows();
    void rowChanged(std::size_t index, const char *key);
    void depthChanged(const char *key, const QComboBox &combo);
    void showClearColor(std::size_t index);
    void pickClearColor(std::size_t index);
    void showDepth();

    QGridLayout *grid_ = nullptr;
    std::vector<Row> rows_;
    std::vector<std::string> targets_;
    nlohmann::json entries_ = nlohmann::json::array();
    // A declaration whose output.color was one object, not a list of them.
    bool single_entry_ = false;
    std::map<std::string, QString, std::less<>> clear_colors_;
    nlohmann::json depth_ = nullptr;
    std::string depth_target_;
    bool depth_ops_shown_ = false;
    QLabel *depth_label_ = nullptr;
    QComboBox *depth_load_ = nullptr;
    QComboBox *depth_store_ = nullptr;
};

} // namespace PelicanStudio
