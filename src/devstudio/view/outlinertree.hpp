#pragma once

#include "../model/project.hpp"

#include <QPoint>
#include <QTreeWidget>

#include <cstdint>
#include <functional>
#include <optional>

class QAction;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;

namespace PelicanStudio {

// What an Outliner row carries. A scene row has only the scene id; an object
// row has all three.
inline constexpr int OutlinerSceneIdRole = Qt::UserRole;
inline constexpr int OutlinerDeclarationIndexRole = Qt::UserRole + 1;
inline constexpr int OutlinerAuthoringIdRole = Qt::UserRole + 2;

// The object a row stands for; empty for a scene row or no row.
[[nodiscard]] std::optional<OutlinerObjectKey> outlinerObjectKey(const QTreeWidgetItem *item);

// OL-1: the Outliner's tree. It is a QTreeWidget with two things of its own:
// Duplicate (Ctrl+D) and Delete (Del) as actions that answer while the tree
// has focus, and a drag that moves a row under another one. The drag is the
// tree's own press / move / release, not Qt's drag and drop: a QDrag runs a
// modal loop of the desktop's, which neither a test nor the automation channel
// can drive, and a drop only ever lands inside this tree.
//
// Let go over the middle of a row, the dragged object goes under that row's
// object. Let go on the line between two rows, on the scene's row, or below
// the last row, it goes to its scene's root. Alt held keeps the object's own
// transform (it moves with the new parent) instead of keeping it where it is.
// A row under the dragged one, and a row of another scene, cannot take it: the
// cursor says so while it is over one, and letting go there sends nothing.
class OutlinerTree final : public QTreeWidget {
  public:
    enum class DropPlace : std::uint8_t {
        None,     // nothing happens (no drag, or over the dragged row itself)
        Onto,     // under `row`'s object
        Root,     // to the dragged object's scene root
        Refused,  // under a row that cannot take it
    };
    struct DropTarget {
        DropPlace place = DropPlace::None;
        QTreeWidgetItem *row = nullptr;
    };
    struct Drop {
        OutlinerObjectKey dragged;
        // Empty: to the scene root.
        std::optional<OutlinerObjectKey> onto;
        bool preserve_local = false;
    };

    explicit OutlinerTree(QWidget *parent = nullptr);

    [[nodiscard]] QAction *duplicateAction() const noexcept { return duplicate_; }
    [[nodiscard]] QAction *deleteAction() const noexcept { return delete_; }
    void setDropAction(std::function<void(const Drop &)> action);
    // A drag let go over a row that cannot take it.
    void setRefusedDropAction(std::function<void(const Drop &)> action);

    // What letting `dragged` go at `position` (viewport coordinates) does.
    [[nodiscard]] DropTarget dropTargetAt(const QTreeWidgetItem *dragged,
                                          const QPoint &position) const;
    [[nodiscard]] bool dragging() const noexcept { return dragging_; }
    // Where the drag in progress would land now; None with no drag.
    [[nodiscard]] DropTarget hoveredDropTarget() const;

  protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    QAction *duplicate_ = nullptr;
    QAction *delete_ = nullptr;
    std::function<void(const Drop &)> drop_action_;
    std::function<void(const Drop &)> refused_drop_action_;
    std::optional<OutlinerObjectKey> pressed_;
    QPoint press_position_;
    QPoint hover_position_;
    bool dragging_ = false;

    [[nodiscard]] QTreeWidgetItem *pressedRow() const;
    void endDrag();
};

} // namespace PelicanStudio
