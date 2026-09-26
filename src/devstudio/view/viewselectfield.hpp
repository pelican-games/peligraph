#pragma once

#include "frameplanedit.hpp"

#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QVBoxLayout;

namespace PelicanStudio {

// LP-4b. The View field of the Pass Editor and the Task Editor: one row per
// axis of the node's view family (the engine's view_axes), which views of the
// family the pass or task runs for (view_select). A selectable axis is a
// combo -- "All views", then its values ("2 (+Y)" when the engine names
// them); an axis decided each frame is a line that says so; an axis
// view_select names but the family does not have keeps its value under a
// warning, with Remove. Nothing is typed.
//
// Object names hang off the prefix: "<prefix>.view.<axis>" (the combo, or the
// line of an axis decided each frame), "<prefix>.view.<axis>.warning",
// "<prefix>.view.<axis>.remove".
class ViewSelectField final : public QWidget {
  public:
    explicit ViewSelectField(QString object_prefix, QWidget *parent = nullptr);

    // Shows the rows framePlanViewFieldRows gives. False -- and no row --
    // when the family has no axes; the owner then hides the field.
    bool load(const std::vector<FramePlanViewAxis> &axes,
              const std::vector<FramePlanViewSelectAxis> &view_select,
              const std::string &family = {});
    void clear();
    [[nodiscard]] bool hasRows() const noexcept { return !rows_.empty(); }
    // The view_select the rows say: the chosen value of each selectable
    // axis, the kept value of any other axis that was not removed, in row
    // order.
    [[nodiscard]] std::vector<FramePlanViewSelectAxis> selection() const;

    // A person changed a row (a pick, or Remove).
    std::function<void()> changed;

  private:
    struct Row {
        std::string axis;
        FramePlanViewFieldRow::Kind kind = FramePlanViewFieldRow::Kind::choice;
        QComboBox *combo = nullptr;
        std::optional<std::uint64_t> kept;
        QWidget *widget = nullptr;
        bool removed = false;
    };
    QString prefix_;
    QVBoxLayout *layout_ = nullptr;
    std::vector<Row> rows_;
};

} // namespace PelicanStudio
