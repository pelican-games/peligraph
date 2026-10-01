#pragma once

#include <nlohmann/json.hpp>

#include <QString>
#include <QWidget>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QComboBox;

namespace PelicanStudio {

// UX-4a. A form that edits one part of a pass declaration in place of a raw
// JSON field. It is given the value the declaration holds (JSON) and gives
// back the value the declaration would hold now; `changed` says the author
// moved something. A form never writes a key it was not asked to and never
// drops one it does not know: what it is given and does not edit comes back
// as it was. Nothing here knows the Pass Editor, so the Graph's detail panel
// can hold the same forms (UX-4b).
//
// Every child is named `<prefix>.<field>`, the prefix being the panel's own
// ("pelican.fullscreenPass" in the Pass Editor), so the automation channel
// reaches the fields of each panel by its names.
class PassForm : public QWidget {
    Q_OBJECT

  public:
    explicit PassForm(QString object_prefix, QWidget *parent = nullptr);

    [[nodiscard]] const QString &objectPrefix() const noexcept {
        return prefix_;
    }

  signals:
    // The author changed a value. Not emitted while a value is loaded.
    void changed();

  protected:
    // `<prefix>.<suffix>`.
    [[nodiscard]] QString childName(std::string_view suffix) const;
    // Emits changed() unless a value is being loaded.
    void notifyChanged();

    // Held while load() fills the fields, so filling them is not a change.
    class Loading {
        PassForm &form_;
        bool previous_;

      public:
        explicit Loading(PassForm &form)
            : form_{form}, previous_{form.loading_} {
            form_.loading_ = true;
        }
        ~Loading() { form_.loading_ = previous_; }
        Loading(const Loading &) = delete;
        Loading &operator=(const Loading &) = delete;
    };
    [[nodiscard]] bool loading() const noexcept { return loading_; }

  private:
    QString prefix_;
    bool loading_ = false;
};

namespace passform {

using Json = nlohmann::json;

QString text(std::string_view value);

// A combo for an optional key: the first entry ("Default") is the key left
// out, the rest are the values the engine reads. Each entry's data is its
// value.
void fillOptionalChoices(QComboBox &combo, const QString &default_label,
                         const std::vector<std::string> &values);
// Shows `value` (nullptr: the key is absent). A string the list does not
// have is added, and a value that is not a string is kept as an entry of its
// own ("custom"), so what the declaration says is what the combo gives back.
void showOptionalValue(QComboBox &combo, const Json *value);
// The value the combo stands for; nullopt for "Default".
std::optional<Json> optionalValue(const QComboBox &combo);
// Selects the entry whose data is `value`, adding it when it is not there.
void selectData(QComboBox &combo, const QString &value);

// "a, b ,c" -> {"a", "b", "c"}.
std::vector<std::string> splitNames(const QString &line);
QString joinNames(const Json &names);

} // namespace passform

} // namespace PelicanStudio
