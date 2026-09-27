#pragma once

#include "../model/runtimeobjects.hpp"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>

class QTimer;
class QTreeWidgetItem;

namespace PelicanStudio {

class EmbeddedViewport;
class OutlinerTree;

// PM-3c (docs/design_play_mode.md 12.6). The Outliner's rows for the objects
// the running game spawned: after the scene's own objects, under the scene the
// engine runs, marked "(runtime)" and greyed, with the prefab they came from
// as their tooltip. They are read with get_runtime_objects (RO-1) when a play
// reading says the game started, stopped, paused, resumed or stepped, and once
// a second while it plays; never in the edit state, where there are none.
//
// A runtime row can be selected (the Inspector shows it read-only) but not
// edited: Duplicate and Delete are off for it, it cannot be dragged, and no
// row can be dropped onto it.
class RuntimeOutliner final : public QObject {
  public:
    // One sentence for everything a runtime row refuses.
    static QString editRefusal();

    // `changed` is called after the rows or the selected runtime object
    // changed; the owner shows the selection again.
    RuntimeOutliner(EmbeddedViewport &viewport, OutlinerTree &tree,
                    std::function<void()> changed, QObject *parent = nullptr);

    // The Outliner rebuilt its rows from the project (and so dropped these):
    // they go back at the end of the running scene's rows.
    void appendRows();
    // The row of `entity`; nullptr when it has none.
    [[nodiscard]] QTreeWidgetItem *rowOf(const RuntimeObjectEntity &entity) const;

    [[nodiscard]] RuntimeObjectsModel &model() noexcept { return model_; }
    [[nodiscard]] const RuntimeObjectsModel &model() const noexcept { return model_; }
    [[nodiscard]] QTimer *readTimer() const noexcept { return read_timer_; }

  private:
    EmbeddedViewport &viewport_;
    OutlinerTree &tree_;
    std::function<void()> changed_;
    RuntimeObjectsModel model_;
    QTimer *read_timer_ = nullptr;
    // transport id -> the model's request id.
    QHash<qint64, std::uint64_t> transport_;
    // Refused as an unknown method (-32601), until the failure that follows.
    QSet<qint64> unknown_method_;
    std::uint64_t shown_revision_ = 0;
    // What the rows on screen were made from.
    QStringList shown_rows_;

    [[nodiscard]] QStringList rowsSignature() const;
    void dispatch();
    void synchronize();
    void removeRows();
};

} // namespace PelicanStudio
