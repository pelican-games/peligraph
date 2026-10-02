#include "graphcanvastagpicker.hpp"

#include <QAction>
#include <QLineEdit>
#include <QMenu>
#include <QStringList>
#include <QWidgetAction>

#include <algorithm>

namespace PelicanStudio {
namespace {

QString qtext(const std::string &value) { return QString::fromStdString(value); }

} // namespace

GraphCanvasTagPicker::GraphCanvasTagPicker(const QString &object_name, const QString &empty_text,
                                           QWidget *parent)
    : QToolButton{parent}, empty_text_{empty_text} {
    setObjectName(object_name);
    setPopupMode(QToolButton::InstantPopup);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *list = new QMenu(this);
    list->setObjectName(object_name + QStringLiteral(".menu"));
    setMenu(list);
    showSelection();
}

void GraphCanvasTagPicker::setTags(const std::vector<std::string> &all,
                                   const std::vector<std::string> &selected) {
    auto tags = all;
    // What the document names stays visible, even a tag no material has.
    for (const auto &tag : selected)
        if (std::ranges::find(tags, tag) == tags.end()) tags.push_back(tag);
    rebuild(tags, selected);
}

void GraphCanvasTagPicker::rebuild(const std::vector<std::string> &tags,
                                   const std::vector<std::string> &selected) {
    menu()->clear();
    checks_.clear();
    new_tag_ = nullptr;
    for (const auto &tag : tags) {
        auto *check = menu()->addAction(qtext(tag));
        check->setObjectName(objectName() + QLatin1Char{'.'} + qtext(tag));
        check->setCheckable(true);
        check->setChecked(std::ranges::find(selected, tag) != selected.end());
        connect(check, &QAction::triggered, this, [this] {
            showSelection();
            if (changed) changed();
        });
        checks_.emplace_back(tag, check);
    }
    if (new_tags_) {
        // UX-11, decision 2: the one place a tag is typed.
        if (!tags.empty()) menu()->addSeparator();
        auto *line_action = new QWidgetAction(menu());
        new_tag_ = new QLineEdit(menu());
        new_tag_->setObjectName(objectName() + QStringLiteral(".newTag"));
        new_tag_->setPlaceholderText(tr("+ new tag"));
        new_tag_->setToolTip(tr("A tag no material lists yet: type it and press Enter"));
        line_action->setDefaultWidget(new_tag_);
        menu()->addAction(line_action);
        connect(new_tag_, &QLineEdit::returnPressed, this, [this] {
            if (new_tag_ == nullptr) return;
            const auto typed = new_tag_->text().trimmed().toStdString();
            if (typed.empty()) return;
            new_tag_->clear();
            // The line goes with the menu's rebuild: not from inside its own
            // signal.
            QMetaObject::invokeMethod(
                this, [this, typed] {
                    if (addTag(typed)) menu()->close();
                },
                Qt::QueuedConnection);
        });
    }
    showSelection();
}

void GraphCanvasTagPicker::allowNewTags(bool allowed) {
    if (new_tags_ == allowed) return;
    new_tags_ = allowed;
    std::vector<std::string> tags;
    for (const auto &[tag, check] : checks_) tags.push_back(tag);
    rebuild(tags, selected());
}

bool GraphCanvasTagPicker::addTag(const std::string &tag) {
    if (tag.empty()) return false;
    std::vector<std::string> tags;
    for (const auto &[name, check] : checks_) tags.push_back(name);
    auto chosen = selected();
    if (std::ranges::find(tags, tag) == tags.end()) tags.push_back(tag);
    if (std::ranges::find(chosen, tag) == chosen.end()) chosen.push_back(tag);
    rebuild(tags, chosen);
    if (changed) changed();
    return true;
}

std::vector<std::string> GraphCanvasTagPicker::selected() const {
    std::vector<std::string> tags;
    for (const auto &[tag, check] : checks_)
        if (check->isChecked()) tags.push_back(tag);
    return tags;
}

void GraphCanvasTagPicker::showSelection() {
    QStringList names;
    for (const auto &tag : selected()) names << qtext(tag);
    setText(names.isEmpty() ? empty_text_ : names.join(QStringLiteral(", ")));
}

} // namespace PelicanStudio
