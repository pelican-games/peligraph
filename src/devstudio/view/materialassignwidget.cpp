#include "materialassignwidget.hpp"

#include "panelhelp.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;
using Scope = MaterialAssignSession::Scope;

QString qtext(const std::string &text) { return QString::fromStdString(text); }

Json toJson(const QJsonObject &object) {
    const auto parsed =
        Json::parse(QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString(), nullptr, false);
    return parsed.is_discarded() ? Json::object() : parsed;
}

// What a combo's entry carries: a material's name, Model default (empty),
// the placeholder of primitives that carry different ones, or (This object)
// the entry that says what is drawn now.
const QString ModelDefault;
const QString Mixed = QStringLiteral("\x01mixed");
const QString Current = QStringLiteral("\x01current");

void disableItem(QComboBox &combo, int index) {
    if (auto *model = qobject_cast<QStandardItemModel *>(combo.model()))
        if (auto *item = model->item(index)) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
}

} // namespace

struct MaterialAssignWidget::Impl {
    MaterialAssignWidget &owner;
    Hooks hooks;
    MaterialAssignSession session;
    QComboBox *target = nullptr;
    QComboBox *choice = nullptr;
    QToolButton *use_default = nullptr;
    QLabel *scope = nullptr;
    QLabel *missing = nullptr;
    QWidget *primitives_box = nullptr;
    QFormLayout *primitives_form = nullptr;
    std::vector<QComboBox *> primitive_choices;
    QToolButton *undo = nullptr;
    QLabel *notice = nullptr;

    Impl(MaterialAssignWidget &widget, Hooks widget_hooks)
        : owner{widget}, hooks{std::move(widget_hooks)},
          session{MaterialAssignSession::Transport{
              .request = [this](const std::string &method, const Json &params,
                                std::string *error) -> std::int64_t {
                  if (!hooks.send) return 0;
                  const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(params.dump())).object();
                  QString message;
                  const qint64 id = hooks.send(qtext(method), object, &message);
                  if (id == 0 && error != nullptr) *error = message.toStdString();
                  return id;
              },
              .edit_object = [this](const Json &params, const std::string &description,
                                    std::string *error) -> bool {
                  if (!hooks.edit_object) return false;
                  const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(params.dump())).object();
                  QString message;
                  const bool sent = hooks.edit_object(object, qtext(description), &message);
                  if (!sent && error != nullptr) *error = message.toStdString();
                  return sent;
              },
          }} {
        session.changed = [this] {
            present();
            emit owner.rowsPresented();
        };
        session.restartRequested = [this](const std::string &reason) {
            if (hooks.restart) hooks.restart(qtext(reason));
        };
        build();
    }

    void build() {
        owner.setTitle(owner.tr("Material"));
        owner.setObjectName(QStringLiteral("pelican.inspectorMaterial"));
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(3);
        // MT-2s, decision 1: what a choice applies to, This object first.
        target = new QComboBox(&owner);
        target->setObjectName(QStringLiteral("pelican.inspectorMaterialTarget"));
        target->addItem(owner.tr("This object"), static_cast<int>(Scope::Object));
        target->addItem(owner.tr("Every model"), static_cast<int>(Scope::Model));
        target->setItemData(0, owner.tr("Only this object's materials change (set_object_material); "
                                        "the scene's Undo takes it back"),
                            Qt::ToolTipRole);
        target->setItemData(1, owner.tr("The model's material changes for every object that shows it "
                                        "(bind_material)"),
                            Qt::ToolTipRole);
        QObject::connect(target, &QComboBox::currentIndexChanged, &owner, [this](int index) {
            if (index < 0) return;
            session.setScope(static_cast<Scope>(target->itemData(index).toInt()));
        });
        layout->addWidget(target);
        auto *row = new QHBoxLayout;
        choice = new QComboBox(&owner);
        choice->setObjectName(QStringLiteral("pelican.inspectorMaterialChoice"));
        QObject::connect(choice, &QComboBox::currentIndexChanged, &owner, [this](int index) {
            // The first object entry describes the readback, even when it carries a real reference.
            if (index < 0 || (index == 0 && session.scope() == Scope::Object)) return;
            const auto value = choice->itemData(index).toString();
            if (value == Mixed || value == Current) return;
            if (session.scope() == Scope::Model) {
                const auto shared = session.sharedMaterial();
                if (shared && *shared == value.toStdString()) return;
            }
            session.choose(value.toStdString());
        });
        use_default = new QToolButton(&owner);
        use_default->setObjectName(QStringLiteral("pelican.inspectorMaterialUseModelDefault"));
        use_default->setToolButtonStyle(Qt::ToolButtonTextOnly);
        use_default->setText(owner.tr("Use model default"));
        use_default->setToolTip(owner.tr("Take this object's own materials away: it draws the model's again"));
        QObject::connect(use_default, &QToolButton::clicked, &owner, [this] { session.useModelDefault(); });
        undo = new QToolButton(&owner);
        undo->setObjectName(QStringLiteral("pelican.inspectorMaterialUndo"));
        undo->setToolButtonStyle(Qt::ToolButtonTextOnly);
        undo->setText(owner.tr("Undo"));
        QObject::connect(undo, &QToolButton::clicked, &owner, [this] { session.undo(); });
        row->addWidget(choice, 1);
        row->addWidget(use_default);
        row->addWidget(undo);
        layout->addLayout(row);
        scope = new QLabel(&owner);
        scope->setObjectName(QStringLiteral("pelican.inspectorMaterialScope"));
        applyPanelNoticeStyle(scope);
        scope->setWordWrap(true);
        layout->addWidget(scope);
        missing = new QLabel(&owner);
        missing->setObjectName(QStringLiteral("pelican.inspectorMaterialMissing"));
        missing->setStyleSheet(QStringLiteral("color: #d9a23d;"));
        missing->setWordWrap(true);
        missing->hide();
        layout->addWidget(missing);
        primitives_box = new QWidget(&owner);
        primitives_box->setObjectName(QStringLiteral("pelican.inspectorMaterialPrimitives"));
        primitives_form = new QFormLayout(primitives_box);
        primitives_form->setContentsMargins(0, 0, 0, 0);
        primitives_box->hide();
        layout->addWidget(primitives_box);
        notice = new QLabel(&owner);
        notice->setObjectName(QStringLiteral("pelican.inspectorMaterialNotice"));
        applyPanelNoticeStyle(notice);
        // A pass refusal must fit the dock instead of squeezing out the viewport.
        notice->setWordWrap(true);
        notice->hide();
        layout->addWidget(notice);
        owner.hide();
    }

    void addMaterials(QComboBox &combo) {
        if (!session.materials().empty()) combo.insertSeparator(combo.count());
        for (const auto &name : session.materials()) combo.addItem(qtext(name), qtext(name));
        for (const auto &[reference, label] : session.embeddedMaterials()) combo.addItem(qtext(label), qtext(reference));
    }

    // Every <model>: Model default, then the project's materials; a name the
    // bindings carry that is not listed (a glTF material) after them.
    void fill(QComboBox &combo, const std::optional<std::string> &current, const std::string &slot_hint) {
        const QSignalBlocker block{&combo};
        combo.clear();
        combo.addItem(owner.tr("Model default"), ModelDefault);
        combo.setItemData(0, slot_hint.empty() ? owner.tr("The model's own material")
                                               : owner.tr("The model's own material (%1)").arg(qtext(session.materialLabel(slot_hint))),
                          Qt::ToolTipRole);
        addMaterials(combo);
        if (!current) {
            combo.addItem(owner.tr("(differs per primitive)"), Mixed);
            disableItem(combo, combo.count() - 1);
            combo.setCurrentIndex(combo.count() - 1);
            return;
        }
        int index = combo.findData(qtext(*current));
        if (index < 0) {
            combo.addItem(qtext(session.materialLabel(*current)), qtext(*current));
            index = combo.count() - 1;
        }
        combo.setCurrentIndex(index);
    }

    // This object (decision 2): first what is drawn now and where it comes
    // from, then -- for one primitive that carries its own -- the model's
    // again, then the project's materials to choose from. While the edit of
    // a choice is out, that choice stays the entry held.
    void fillObject(QComboBox &combo, const std::optional<std::string> &current,
                    const std::optional<std::string> &model_material,
                    const std::optional<std::string> &chosen = std::nullopt) {
        const QSignalBlocker block{&combo};
        combo.clear();
        combo.addItem(current ? qtext(session.materialLabel(*current)) : owner.tr("(differs per primitive)"), current ? Current : Mixed);
        disableItem(combo, 0);
        if (model_material)
            combo.addItem(owner.tr("Model default (%1)").arg(qtext(session.materialLabel(*model_material))), ModelDefault);
        addMaterials(combo);
        const int held = chosen ? combo.findData(qtext(*chosen)) : -1;
        combo.setCurrentIndex(held > 0 ? held : 0);
    }

    void presentPrimitives() {
        const bool object_scope = session.scope() == Scope::Object;
        const auto count = object_scope ? session.objectMaterials().primitives.size() : session.primitives().size();
        const bool several = count > 1;
        primitives_box->setVisible(several);
        if (!several) {
            while (primitives_form->rowCount() > 0) primitives_form->removeRow(0);
            primitive_choices.clear();
            return;
        }
        if (primitive_choices.size() != count) {
            while (primitives_form->rowCount() > 0) primitives_form->removeRow(0);
            primitive_choices.clear();
            for (std::size_t index = 0; index < count; ++index) {
                auto *combo = new QComboBox(primitives_box);
                combo->setObjectName(QStringLiteral("pelican.inspectorMaterialPrimitive.%1").arg(index));
                QObject::connect(combo, &QComboBox::currentIndexChanged, &owner, [this, index, combo](int row) {
                    if (row < 0) return;
                    const auto value = combo->itemData(row).toString();
                    if (value == Current || value == Mixed) return;
                    if (session.scope() == Scope::Model) {
                        if (index >= session.primitives().size()) return;
                        const auto &primitive = session.primitives()[index];
                        const auto shown = primitive.material == primitive.slot ? std::string{} : primitive.material;
                        if (value.toStdString() == shown) return;
                    }
                    session.choosePrimitive(index, value.toStdString());
                });
                primitives_form->addRow(QString{}, combo);
                primitive_choices.push_back(combo);
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            std::uint64_t mesh = 0;
            std::uint64_t primitive_index = 0;
            if (object_scope) {
                const auto &primitive = session.objectMaterials().primitives[index];
                mesh = primitive.mesh;
                primitive_index = primitive.primitive;
                fillObject(*primitive_choices[index], objectMaterialLabel([&] { auto shown = primitive;
                    shown.material = session.materialLabel(shown.material);
                    shown.drawn = session.materialLabel(shown.drawn); return shown; }()),
                           primitive.source == "object" ? std::optional{primitive.model_material} : std::nullopt);
            } else {
                const auto &primitive = session.primitives()[index];
                mesh = primitive.mesh;
                primitive_index = primitive.primitive;
                fill(*primitive_choices[index],
                     primitive.material == primitive.slot ? std::string{} : primitive.material, primitive.slot);
            }
            if (auto *label = qobject_cast<QLabel *>(
                    primitives_form->itemAt(static_cast<int>(index), QFormLayout::LabelRole)
                        ? primitives_form->itemAt(static_cast<int>(index), QFormLayout::LabelRole)->widget()
                        : nullptr))
                label->setText(owner.tr("Mesh %1 \xC2\xB7 primitive %2").arg(mesh).arg(primitive_index));
            primitive_choices[index]->setEnabled(session.choosable());
        }
    }

    void present() {
        const bool shown = !session.model().empty();
        owner.setVisible(shown);
        const auto &undo_entries = session.undoEntries();
        undo->setVisible(!undo_entries.empty());
        undo->setEnabled(!undo_entries.empty() && session.ready() && !session.bindPending());
        undo->setText(undo_entries.empty() ? owner.tr("Undo")
                                           : owner.tr("Undo %1").arg(qtext(undo_entries.back().description)));
        undo->setToolTip(undo->text());
        if (!shown) {
            // QA-1: no object, nothing to choose. The rows go with the panel
            // switched off and naming no model: left as they were, they kept
            // the last object's entries ("Every Cube") and said they could be
            // chosen in while the next object's were still being read, so a
            // wait for "the rows are read" was answered by the previous
            // object's.
            const QSignalBlocker block{target};
            target->setItemText(1, owner.tr("Every model"));
            target->setEnabled(false);
            choice->setEnabled(false);
            return;
        }
        const bool object_scope = session.scope() == Scope::Object;
        const auto model_name = qtext(modelShortName(session.model()));
        {
            const QSignalBlocker block{target};
            target->setItemText(1, owner.tr("Every %1").arg(model_name));
            target->setCurrentIndex(object_scope ? 0 : 1);
            target->setEnabled(session.ready());
        }
        if (object_scope) {
            fillObject(*choice, session.shownObjectLabel(), std::nullopt, session.objectRequested());
            // Keep the display label (including its source) separate from the material reference.
            // Mixed and unread rows retain their sentinel and cannot open a hit-group assignment.
            if (choice->itemData(0).toString() == Current && session.objectRead() &&
                !session.objectEditPending() && !session.objectMaterials().primitives.empty()) {
                const auto &primitive = session.objectMaterials().primitives.front();
                const auto &material = primitive.drawn.empty() ? primitive.material : primitive.drawn;
                const bool shared = std::ranges::all_of(session.objectMaterials().primitives, [&](const auto &p) {
                    return (p.drawn.empty() ? p.material : p.drawn) == material;
                });
                if (shared && !material.empty()) choice->setItemData(0, qtext(material));
            }
            choice->setToolTip(owner.tr("The material this object draws with; a choice gives this object its own "
                                        "(set_object_material)"));
            setPanelNotice(scope, qtext(objectScopeLine(session.subject().name)),
                           owner.tr("Every other %1 keeps the model's material").arg(model_name));
        } else {
            const auto &primitives = session.primitives();
            fill(*choice, session.shownMaterial(),
                 primitives.size() == 1 ? primitives.front().slot : std::string{});
            choice->setToolTip(
                owner.tr("The material every primitive of this model draws with (bind_material)"));
            setPanelNotice(scope, qtext(modelScopeLine(session.model(), session.counts())),
                           owner.tr("A material is bound to the model (%1); an object with its own keeps it")
                               .arg(qtext(session.model())));
        }
        choice->setEnabled(session.choosable());
        use_default->setVisible(object_scope);
        use_default->setEnabled(session.canUseModelDefault());
        // Decision 7: an override whose material is not loaded. EB-6s,
        // decision 5 (engine EB-6): Every <model>, a bound name no registered
        // material holds and what is drawn in its place.
        const auto missing_line = object_scope ? objectMaterialMissingLine(session.objectMaterials())
                                               : materialBindingUnresolvedLine(session.primitives());
        missing->setVisible(!missing_line.empty());
        missing->setText(missing_line.empty() ? QString{} : QStringLiteral("\xE2\x9A\xA0 ") + qtext(missing_line));
        missing->setToolTip(qtext(missing_line));
        presentPrimitives();
        setPanelNotice(notice, oneLine(qtext(session.notice()), 140), qtext(session.noticeDetail()));
    }
};

MaterialAssignWidget::MaterialAssignWidget(Hooks hooks, QWidget *parent) : QGroupBox(parent) {
    impl_ = std::make_unique<Impl>(*this, std::move(hooks));
}

MaterialAssignWidget::~MaterialAssignWidget() {
    impl_->session.changed = nullptr;
    impl_->session.restartRequested = nullptr;
    for (QObject *child : findChildren<QObject *>()) QObject::disconnect(child, nullptr, this, nullptr);
}

void MaterialAssignWidget::engineAvailable() { impl_->session.engineReady(); }
void MaterialAssignWidget::engineUnavailable() { impl_->session.engineGone(); }

void MaterialAssignWidget::setSubject(const MaterialAssignSession::Subject &subject) {
    impl_->session.setSubject(subject);
}

void MaterialAssignWidget::setSceneBusy(bool busy) { impl_->session.setSceneBusy(busy); }

void MaterialAssignWidget::materialsChanged() { impl_->session.materialsChanged(); }
void MaterialAssignWidget::bindingsChanged() { impl_->session.bindingsChanged(); }

QString MaterialAssignWidget::objectEditRefused(const QString &error_code, const QString &message,
                                                const QJsonValue &data) {
    impl_->session.objectEditRefused(error_code.toStdString(), message.toStdString(),
                                     toJson(data.toObject().value(QStringLiteral("error_fields")).toObject()));
    return qtext(impl_->session.notice());
}

bool MaterialAssignWidget::receiveResult(qint64 request_id, const QByteArray &result_json) {
    if (!impl_->session.owns(request_id)) return false;
    const auto result =
        Json::parse(result_json.constData(), result_json.constData() + result_json.size(), nullptr, false);
    return impl_->session.receiveResult(request_id, result.is_discarded() ? Json{} : result);
}

bool MaterialAssignWidget::receiveRefusal(qint64 request_id, int code, const QString &message,
                                          const QJsonValue &data) {
    if (!impl_->session.owns(request_id)) return false;
    const QJsonObject object = data.toObject();
    return impl_->session.receiveRefusal(request_id, code,
                                         object.value(QStringLiteral("error_code")).toString().toStdString(),
                                         message.toStdString(),
                                         toJson(object.value(QStringLiteral("error_fields")).toObject()));
}

bool MaterialAssignWidget::receiveFailure(qint64 request_id, const QString &message) {
    return impl_->session.receiveFailure(request_id, message.toStdString());
}

MaterialAssignSession &MaterialAssignWidget::session() noexcept { return impl_->session; }
QComboBox *MaterialAssignWidget::choice() const noexcept { return impl_->choice; }

QString MaterialAssignWidget::assignableMaterial() const {
    if (!impl_->session.choosable() || impl_->session.readPending()) return {};
    const auto value = impl_->choice->currentData().toString();
    return value == Mixed || value == Current ? QString{} : value;
}

} // namespace PelicanStudio
