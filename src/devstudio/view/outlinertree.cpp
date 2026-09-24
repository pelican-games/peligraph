#include "outlinertree.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QTreeWidgetItemIterator>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <utility>

namespace PelicanStudio {
namespace {

QString sceneOf(const QTreeWidgetItem *item) {
    return item == nullptr ? QString{} : item->data(0, OutlinerSceneIdRole).toString();
}

bool isObjectRow(const QTreeWidgetItem *item) {
    return item != nullptr && item->data(0, OutlinerAuthoringIdRole).isValid();
}

} // namespace

std::optional<OutlinerObjectKey> outlinerObjectKey(const QTreeWidgetItem *item) {
    if (item == nullptr) return std::nullopt;
    const QVariant scene_id = item->data(0, OutlinerSceneIdRole);
    const QVariant declaration_index = item->data(0, OutlinerDeclarationIndexRole);
    const QVariant authoring_id = item->data(0, OutlinerAuthoringIdRole);
    if (!scene_id.isValid() || !declaration_index.isValid() || !authoring_id.isValid())
        return std::nullopt;
    bool valid_index = false;
    const qulonglong index = declaration_index.toULongLong(&valid_index);
    bool valid_id = false;
    const qulonglong id = authoring_id.toULongLong(&valid_id);
    if (!valid_index || !valid_id || id == 0 || index > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    return OutlinerObjectKey{
        .scene_id = scene_id.toString().toStdString(),
        .declaration_index = static_cast<std::size_t>(index),
        .authoring_object_id = static_cast<std::uint64_t>(id),
    };
}

OutlinerTree::OutlinerTree(QWidget *parent) : QTreeWidget(parent) {
    // The drag below is the tree's own; Qt's would start a QDrag.
    setDragDropMode(QAbstractItemView::NoDragDrop);
    setSelectionMode(QAbstractItemView::SingleSelection);

    // While the tree has focus: Del in an Inspector field deletes a
    // character, never an object.
    duplicate_ = new QAction(tr("Duplicate"), this);
    duplicate_->setObjectName(QStringLiteral("pelican.outliner.duplicate"));
    duplicate_->setShortcut(QKeySequence{Qt::CTRL | Qt::Key_D});
    duplicate_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    duplicate_->setToolTip(tr("Copy the selected object and everything under it"));
    delete_ = new QAction(tr("Delete"), this);
    delete_->setObjectName(QStringLiteral("pelican.outliner.delete"));
    delete_->setShortcut(QKeySequence{QKeySequence::Delete});
    delete_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    delete_->setToolTip(tr("Delete the selected object and everything under it"));
    addAction(duplicate_);
    addAction(delete_);
}

void OutlinerTree::setDropAction(std::function<void(const Drop &)> action) {
    drop_action_ = std::move(action);
}

void OutlinerTree::setRefusedDropAction(std::function<void(const Drop &)> action) {
    refused_drop_action_ = std::move(action);
}

OutlinerTree::DropTarget OutlinerTree::dropTargetAt(const QTreeWidgetItem *dragged,
                                                     const QPoint &position) const {
    if (!isObjectRow(dragged)) return {};
    QTreeWidgetItem *row = itemAt(position);
    // Below the last row: the root, as a line between rows is.
    if (row == nullptr) return {.place = DropPlace::Root};
    if (sceneOf(row) != sceneOf(dragged)) return {.place = DropPlace::Refused, .row = row};
    if (!isObjectRow(row)) return {.place = DropPlace::Root, .row = row};
    // The quarter of a row nearest each of its edges is the line between two
    // rows; the middle half is the row.
    const QRect rect = visualItemRect(row);
    const int band = std::max(2, rect.height() / 4);
    if (position.y() < rect.top() + band || position.y() > rect.bottom() - band)
        return {.place = DropPlace::Root, .row = row};
    if (row == dragged) return {.row = row};
    for (const QTreeWidgetItem *ancestor = row->parent(); ancestor != nullptr;
         ancestor = ancestor->parent()) {
        if (ancestor == dragged) return {.place = DropPlace::Refused, .row = row};
    }
    return {.place = DropPlace::Onto, .row = row};
}

QTreeWidgetItem *OutlinerTree::pressedRow() const {
    // By key, not by pointer: the Outliner rebuilds its rows whenever the
    // engine's scene moves, and that can happen in the middle of a drag.
    if (!pressed_) return nullptr;
    for (QTreeWidgetItemIterator row{const_cast<OutlinerTree *>(this)}; *row != nullptr; ++row) {
        const auto key = outlinerObjectKey(*row);
        if (key && key->scene_id == pressed_->scene_id &&
            key->authoring_object_id == pressed_->authoring_object_id)
            return *row;
    }
    return nullptr;
}

void OutlinerTree::mousePressEvent(QMouseEvent *event) {
    QTreeWidget::mousePressEvent(event);
    endDrag();
    if (event->button() != Qt::LeftButton) return;
    pressed_ = outlinerObjectKey(itemAt(event->position().toPoint()));
    press_position_ = event->position().toPoint();
}

void OutlinerTree::mouseMoveEvent(QMouseEvent *event) {
    if (!pressed_ || !(event->buttons() & Qt::LeftButton)) {
        QTreeWidget::mouseMoveEvent(event);
        return;
    }
    // Held on a row: this is a drag or nothing. The tree's own handling would
    // drag the selection along to whatever row the pointer crosses.
    const QPoint position = event->position().toPoint();
    if (!dragging_ &&
        (position - press_position_).manhattanLength() < QApplication::startDragDistance())
        return;
    dragging_ = true;
    hover_position_ = position;
    viewport()->setCursor(hoveredDropTarget().place == DropPlace::Refused
                              ? Qt::ForbiddenCursor
                              : Qt::DragMoveCursor);
    viewport()->update();
}

void OutlinerTree::mouseReleaseEvent(QMouseEvent *event) {
    if (!dragging_ || event->button() != Qt::LeftButton) {
        endDrag();
        QTreeWidget::mouseReleaseEvent(event);
        return;
    }
    const DropTarget target = dropTargetAt(pressedRow(), event->position().toPoint());
    const auto dragged = outlinerObjectKey(pressedRow());
    const bool preserve_local = event->modifiers().testFlag(Qt::AltModifier);
    endDrag();
    // The press is over: the tree must not treat a later move as a
    // selection drag.
    setState(QAbstractItemView::NoState);
    if (!dragged) return;
    Drop drop{.dragged = *dragged, .onto = outlinerObjectKey(target.row),
              .preserve_local = preserve_local};
    switch (target.place) {
    case DropPlace::Onto:
        if (drop_action_) drop_action_(drop);
        break;
    case DropPlace::Root:
        drop.onto.reset();
        if (drop_action_) drop_action_(drop);
        break;
    case DropPlace::Refused:
        if (refused_drop_action_) refused_drop_action_(drop);
        break;
    case DropPlace::None:
        break;
    }
}

void OutlinerTree::keyPressEvent(QKeyEvent *event) {
    if (dragging_ && event->key() == Qt::Key_Escape) {
        endDrag();
        setState(QAbstractItemView::NoState);
        event->accept();
        return;
    }
    QTreeWidget::keyPressEvent(event);
}

void OutlinerTree::paintEvent(QPaintEvent *event) {
    QTreeWidget::paintEvent(event);
    const DropTarget hover = hoveredDropTarget();
    if (hover.place == DropPlace::None) return;
    QPainter painter(viewport());
    const QColor accent = hover.place == DropPlace::Refused
                              ? QColor{0xd9, 0x4c, 0x3d}
                              : palette().color(QPalette::Highlight);
    painter.setPen(QPen{accent, 2});
    if (hover.place == DropPlace::Root) {
        // A full-width line: out from under any row, onto the scene's root.
        const int y = hover.row != nullptr ? visualItemRect(hover.row).bottom() : 1;
        painter.drawLine(0, y, viewport()->width(), y);
        return;
    }
    // A box: under this row (Onto), or a red one it cannot go under.
    painter.drawRect(visualItemRect(hover.row).adjusted(1, 0, -2, -1));
}

OutlinerTree::DropTarget OutlinerTree::hoveredDropTarget() const {
    if (!dragging_) return {};
    return dropTargetAt(pressedRow(), hover_position_);
}

void OutlinerTree::endDrag() {
    const bool was_dragging = dragging_;
    pressed_.reset();
    dragging_ = false;
    if (was_dragging) {
        viewport()->unsetCursor();
        viewport()->update();
    }
}

} // namespace PelicanStudio
