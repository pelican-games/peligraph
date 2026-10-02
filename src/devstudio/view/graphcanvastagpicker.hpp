#pragma once

#include <QToolButton>

#include <functional>
#include <string>
#include <utility>
#include <vector>

class QAction;
class QLineEdit;

namespace PelicanStudio {

// UX-3, decision 5: a choice of any number of the engine's material tags
// (get_render_authoring_context's material_tags), in place of a line of
// comma-separated names: a button whose menu holds one check per tag. A tag
// the document names that the engine does not list stays, checked. Each
// check sends `changed`. The button is named `object_name`, each check
// `<object_name>.<tag>`.
//
// UX-11, decision 2: the material pass's Tags (material_filter) are the same
// picker, in the Pass Editor and on the Graph. With allowNewTags(), the menu
// ends in a line named `<object_name>.newTag` ("+ new tag"): a name typed
// there and confirmed with Enter becomes a check of its own, checked -- the
// one place a tag is typed.
class GraphCanvasTagPicker final : public QToolButton {
  public:
    GraphCanvasTagPicker(const QString &object_name, const QString &empty_text,
                         QWidget *parent = nullptr);
    void setTags(const std::vector<std::string> &all, const std::vector<std::string> &selected);
    [[nodiscard]] std::vector<std::string> selected() const;
    std::function<void()> changed;

    // UX-11, decision 2: the "+ new tag" line at the end of the menu.
    void allowNewTags(bool allowed);
    // Adds `tag` checked (or checks it when it is there), as Enter in the
    // "+ new tag" line does. False for an empty name.
    bool addTag(const std::string &tag);

  private:
    void showSelection();
    void rebuild(const std::vector<std::string> &tags, const std::vector<std::string> &selected);

    QString empty_text_;
    std::vector<std::pair<std::string, QAction *>> checks_;
    bool new_tags_ = false;
    QLineEdit *new_tag_ = nullptr;
};

} // namespace PelicanStudio
