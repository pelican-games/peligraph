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

#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString qtext(const std::string &text) { return QString::fromStdString(text); }

// What a combo's entry carries: a material's name, Model default (empty),
// or the placeholder of primitives that carry different ones.
const QString ModelDefault;
const QString Mixed = QStringLiteral("\x01mixed");

} // namespace

struct MaterialAssignWidget::Impl {
    MaterialAssignWidget &owner;
    Hooks hooks;
    MaterialAssignSession session;
    QComboBox *choice = nullptr;
    QLabel *scope = nullptr;
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
          }} {
        session.changed = [this] { present(); };
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
        auto *row = new QHBoxLayout;
        choice = new QComboBox(&owner);
        choice->setObjectName(QStringLiteral("pelican.inspectorMaterialChoice"));
        choice->setToolTip(owner.tr("The material every primitive of this model draws with (bind_material)"));
        QObject::connect(choice, &QComboBox::currentIndexChanged, &owner, [this](int index) {
            if (index < 0) return;
            const auto value = choice->itemData(index).toString();
            if (value == Mixed) return;
            const auto shared = session.sharedMaterial();
            if (shared && *shared == value.toStdString()) return;
            session.choose(value.toStdString());
        });
        undo = new QToolButton(&owner);
        undo->setObjectName(QStringLiteral("pelican.inspectorMaterialUndo"));
        undo->setToolButtonStyle(Qt::ToolButtonTextOnly);
        undo->setText(owner.tr("Undo"));
        QObject::connect(undo, &QToolButton::clicked, &owner, [this] { session.undo(); });
        row->addWidget(choice, 1);
        row->addWidget(undo);
        layout->addLayout(row);
        scope = new QLabel(&owner);
        scope->setObjectName(QStringLiteral("pelican.inspectorMaterialScope"));
        applyPanelNoticeStyle(scope);
        scope->setWordWrap(true);
        layout->addWidget(scope);
        primitives_box = new QWidget(&owner);
        primitives_box->setObjectName(QStringLiteral("pelican.inspectorMaterialPrimitives"));
        primitives_form = new QFormLayout(primitives_box);
        primitives_form->setContentsMargins(0, 0, 0, 0);
        primitives_box->hide();
        layout->addWidget(primitives_box);
        notice = new QLabel(&owner);
        notice->setObjectName(QStringLiteral("pelican.inspectorMaterialNotice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        layout->addWidget(notice);
        owner.hide();
    }

    // Model default, then the project's materials; `extra` (a name the
    // bindings carry that is not listed: a glTF material) after them.
    void fill(QComboBox &combo, const std::optional<std::string> &current, const std::string &slot_hint) {
        const QSignalBlocker block{&combo};
        combo.clear();
        combo.addItem(owner.tr("Model default"), ModelDefault);
        combo.setItemData(0, slot_hint.empty() ? owner.tr("The model's own material")
                                               : owner.tr("The model's own material (%1)").arg(qtext(slot_hint)),
                          Qt::ToolTipRole);
        if (!session.materials().empty()) combo.insertSeparator(combo.count());
        for (const auto &name : session.materials()) combo.addItem(qtext(name), qtext(name));
        if (!current) {
            combo.addItem(owner.tr("(differs per primitive)"), Mixed);
            if (auto *model = qobject_cast<QStandardItemModel *>(combo.model()))
                if (auto *item = model->item(combo.count() - 1)) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            combo.setCurrentIndex(combo.count() - 1);
            return;
        }
        int index = combo.findData(qtext(*current));
        if (index < 0) {
            combo.addItem(qtext(*current), qtext(*current));
            index = combo.count() - 1;
        }
        combo.setCurrentIndex(index);
    }

    void presentPrimitives() {
        const auto &primitives = session.primitives();
        const bool several = primitives.size() > 1;
        primitives_box->setVisible(several);
        if (!several) {
            while (primitives_form->rowCount() > 0) primitives_form->removeRow(0);
            primitive_choices.clear();
            return;
        }
        if (primitive_choices.size() != primitives.size()) {
            while (primitives_form->rowCount() > 0) primitives_form->removeRow(0);
            primitive_choices.clear();
            for (std::size_t index = 0; index < primitives.size(); ++index) {
                auto *combo = new QComboBox(primitives_box);
                combo->setObjectName(QStringLiteral("pelican.inspectorMaterialPrimitive.%1").arg(index));
                QObject::connect(combo, &QComboBox::currentIndexChanged, &owner, [this, index, combo](int row) {
                    if (row < 0 || index >= session.primitives().size()) return;
                    const auto value = combo->itemData(row).toString().toStdString();
                    const auto &primitive = session.primitives()[index];
                    const auto shown = primitive.material == primitive.slot ? std::string{} : primitive.material;
                    if (value != shown) session.choosePrimitive(index, value);
                });
                primitives_form->addRow(owner.tr("Mesh %1 \xC2\xB7 primitive %2")
                                            .arg(primitives[index].mesh)
                                            .arg(primitives[index].primitive),
                                        combo);
                primitive_choices.push_back(combo);
            }
        }
        for (std::size_t index = 0; index < primitives.size(); ++index) {
            const auto &primitive = primitives[index];
            fill(*primitive_choices[index],
                 primitive.material == primitive.slot ? std::string{} : primitive.material, primitive.slot);
            primitive_choices[index]->setEnabled(session.ready() && !session.bindPending());
        }
    }

    void present() {
        const bool shown = !session.model().empty();
        owner.setVisible(shown);
        if (!shown) return;
        const auto &primitives = session.primitives();
        fill(*choice, session.shownMaterial(), primitives.size() == 1 ? primitives.front().slot : std::string{});
        choice->setEnabled(session.ready() && session.bindingsRead() && !session.bindPending());
        setPanelNotice(scope, qtext(modelScopeLine(session.model(), session.objects())),
                       owner.tr("A material is bound to the model (%1), not to this object")
                           .arg(qtext(session.model())));
        presentPrimitives();
        const auto &undo_entries = session.undoEntries();
        undo->setEnabled(!undo_entries.empty() && session.ready() && !session.bindPending());
        undo->setText(undo_entries.empty() ? owner.tr("Undo")
                                           : owner.tr("Undo %1").arg(qtext(undo_entries.back().description)));
        undo->setToolTip(undo->text());
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

void MaterialAssignWidget::setSubject(const std::string &model, const std::string &object) {
    impl_->session.setSubject(model, object);
}

void MaterialAssignWidget::materialsChanged() { impl_->session.materialsChanged(); }
void MaterialAssignWidget::bindingsChanged() { impl_->session.bindingsChanged(); }

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
    const auto fields = Json::parse(QJsonDocument(object.value(QStringLiteral("error_fields")).toObject())
                                        .toJson(QJsonDocument::Compact)
                                        .toStdString(),
                                    nullptr, false);
    return impl_->session.receiveRefusal(request_id, code,
                                         object.value(QStringLiteral("error_code")).toString().toStdString(),
                                         message.toStdString(), fields.is_discarded() ? Json::object() : fields);
}

bool MaterialAssignWidget::receiveFailure(qint64 request_id, const QString &message) {
    return impl_->session.receiveFailure(request_id, message.toStdString());
}

MaterialAssignSession &MaterialAssignWidget::session() noexcept { return impl_->session; }
QComboBox *MaterialAssignWidget::choice() const noexcept { return impl_->choice; }

} // namespace PelicanStudio
