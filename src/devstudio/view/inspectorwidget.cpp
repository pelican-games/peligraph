#include "inspectorwidget.hpp"

#include "../model/inspectormodel.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

constexpr int PendingPollIntervalMs = 40;
constexpr int WatchPollIntervalMs = 500;
constexpr double MaximumExactJsonInteger = 9007199254740991.0;

QString fieldLabel(const InspectorWidgetDescriptor &descriptor) {
    QString label = QString::fromStdString(descriptor.field_name);
    if (!descriptor.unit.empty()) {
        label += QStringLiteral(" (%1)").arg(
            QString::fromStdString(descriptor.unit));
    }
    return label;
}

QLabel *plainLabel(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

} // namespace

struct InspectorWidget::Impl {
    struct NumericBinding {
        std::string field_key;
        InspectorWidgetKind kind = InspectorWidgetKind::FloatingPointDrag;
        std::vector<QDoubleSpinBox *> editors;
        bool active = false;
    };

    struct TextBinding {
        std::string field_key;
        QLineEdit *editor = nullptr;
        bool active = false;
    };

    InspectorWidget &owner;
    EmbeddedViewport &viewport;
    InspectorModel model;
    QLabel *selection_label = nullptr;
    QLabel *notice_label = nullptr;
    QScrollArea *scroll = nullptr;
    QPushButton *refresh_button = nullptr;
    QPushButton *undo_button = nullptr;
    QPushButton *redo_button = nullptr;
    QPushButton *cancel_preview_button = nullptr;
    QTimer *pending_timer = nullptr;
    QTimer *watch_timer = nullptr;
    QHash<qint64, quint64> transport_requests;
    std::uint64_t rendered_content_revision =
        std::numeric_limits<std::uint64_t>::max();
    bool rebuilding = false;
    std::vector<std::unique_ptr<NumericBinding>> numeric_bindings;
    std::vector<std::unique_ptr<TextBinding>> text_bindings;
    std::vector<QWidget *> direct_editors;
    std::unordered_map<QObject *, NumericBinding *> numeric_events;
    std::unordered_map<QObject *, TextBinding *> text_events;

    explicit Impl(InspectorWidget &widget, EmbeddedViewport &embedded_viewport)
        : owner{widget}, viewport{embedded_viewport} {
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        auto *selection_form = new QFormLayout;
        selection_label = new QLabel(owner.tr("None"), &owner);
        selection_label->setObjectName(
            QStringLiteral("pelican.inspectorSelection"));
        selection_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        selection_label->setWordWrap(true);
        selection_form->addRow(owner.tr("Selection"), selection_label);
        layout->addLayout(selection_form);

        auto *toolbar = new QHBoxLayout;
        refresh_button = new QPushButton(owner.tr("Refresh"), &owner);
        undo_button = new QPushButton(owner.tr("Undo"), &owner);
        redo_button = new QPushButton(owner.tr("Redo"), &owner);
        cancel_preview_button =
            new QPushButton(owner.tr("Cancel Preview"), &owner);
        refresh_button->setObjectName(
            QStringLiteral("pelican.inspectorRefresh"));
        undo_button->setObjectName(QStringLiteral("pelican.inspectorUndo"));
        redo_button->setObjectName(QStringLiteral("pelican.inspectorRedo"));
        cancel_preview_button->setObjectName(
            QStringLiteral("pelican.inspectorCancelPreview"));
        toolbar->addWidget(refresh_button);
        toolbar->addWidget(undo_button);
        toolbar->addWidget(redo_button);
        toolbar->addWidget(cancel_preview_button);
        toolbar->addStretch();
        layout->addLayout(toolbar);

        notice_label = new QLabel(&owner);
        notice_label->setObjectName(QStringLiteral("pelican.inspectorNotice"));
        notice_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        notice_label->setWordWrap(true);
        notice_label->hide();
        layout->addWidget(notice_label);

        scroll = new QScrollArea(&owner);
        scroll->setObjectName(QStringLiteral("pelican.inspectorProperties"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        layout->addWidget(scroll, 1);

        pending_timer = new QTimer(&owner);
        pending_timer->setInterval(PendingPollIntervalMs);
        watch_timer = new QTimer(&owner);
        watch_timer->setInterval(WatchPollIntervalMs);

        QObject::connect(refresh_button, &QPushButton::clicked, &owner,
                         [this] {
                             model.requestRefresh();
                             synchronize();
                             dispatchRequests();
                         });
        QObject::connect(undo_button, &QPushButton::clicked, &owner, [this] {
            model.undo();
            synchronize();
            dispatchRequests();
        });
        QObject::connect(redo_button, &QPushButton::clicked, &owner, [this] {
            model.redo();
            synchronize();
            dispatchRequests();
        });
        QObject::connect(cancel_preview_button, &QPushButton::clicked, &owner,
                         [this] {
                             model.abortActivePreview();
                             synchronize();
                             dispatchRequests();
                         });
        QObject::connect(pending_timer, &QTimer::timeout, &owner, [this] {
            model.pollPendingOperations();
            synchronize();
            dispatchRequests();
        });
        QObject::connect(watch_timer, &QTimer::timeout, &owner, [this] {
            model.pollExternalChanges();
            synchronize();
            dispatchRequests();
        });
        QObject::connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable,
                         &owner, [this] {
                             transport_requests.clear();
                             model.startSession();
                             synchronize();
                             dispatchRequests();
                         });
        QObject::connect(
            &viewport, &EmbeddedViewport::engineRpcBecameUnavailable, &owner,
            [this](const QString &message) {
                transport_requests.clear();
                model.stopSession(message.toStdString());
                synchronize();
            });
        QObject::connect(
            &viewport, &EmbeddedViewport::inspectorRpcSucceeded, &owner,
            [this](qint64 transport_request_id,
                   const QByteArray &result_json) {
                const auto pending =
                    transport_requests.find(transport_request_id);
                if (pending == transport_requests.end()) {
                    return;
                }
                const std::uint64_t model_request_id = pending.value();
                transport_requests.erase(pending);
                model.receiveRpcResult(
                    model_request_id,
                    std::string_view{
                        result_json.constData(),
                        static_cast<std::size_t>(result_json.size())});
                synchronize();
                dispatchRequests();
            });
        QObject::connect(
            &viewport, &EmbeddedViewport::inspectorRpcFailed, &owner,
            [this](qint64 transport_request_id, const QString &message) {
                const auto pending =
                    transport_requests.find(transport_request_id);
                if (pending == transport_requests.end()) {
                    return;
                }
                const std::uint64_t model_request_id = pending.value();
                transport_requests.erase(pending);
                model.receiveRpcFailure(model_request_id,
                                        message.toStdString());
                synchronize();
                dispatchRequests();
            });

        pending_timer->start();
        watch_timer->start();
        rebuildProperties();
        synchronize();
        if (viewport.rpcReady()) {
            model.startSession();
            dispatchRequests();
        }
    }

    void setSelection(std::optional<OutlinerObjectKey> selection,
                      const QString &display_text) {
        selection_label->setText(display_text);
        model.selectObject(std::move(selection));
        synchronize();
        dispatchRequests();
    }

    std::optional<GizmoTransformBinding> gizmoTransformBinding() const {
        if (model.refreshBlocked() || model.busy() || !model.snapshot()) {
            return std::nullopt;
        }
        const InspectorObjectSnapshot &snapshot = *model.snapshot();
        GizmoTransformBinding binding{
            .selection = {.scene_id = snapshot.scene_id,
                          .declaration_index = snapshot.declaration_index},
        };
        const auto component = std::find_if(
            snapshot.components.begin(), snapshot.components.end(),
            [](const InspectorComponentSnapshot &candidate) {
                return candidate.name == "transform" && candidate.editable &&
                       !candidate.pending;
            });
        if (component == snapshot.components.end()) {
            return binding;
        }
        for (const InspectorWidgetDescriptor &widget : component->widgets) {
            if (!widget.authored || !widget.value_matches_schema ||
                widget.component_slot != "transform") {
                continue;
            }
            GizmoEditableField field{.field_key = widget.field_key,
                                     .value = widget.value};
            if (widget.field_name == "pos" && widget.columns == 3) {
                binding.position = std::move(field);
            } else if (widget.field_name == "rotation" &&
                       widget.columns == 4) {
                binding.rotation = std::move(field);
            } else if (widget.field_name == "scale" && widget.columns == 3) {
                binding.scale = std::move(field);
            }
        }
        return binding;
    }

    bool beginGizmoEdit(std::string_view field_key) {
        if (model.refreshBlocked() || model.busy()) return false;
        const bool started = model.beginWidgetEdit(field_key);
        synchronize();
        dispatchRequests();
        return started;
    }

    void reflectGizmoValue(std::string_view field_key, const Json &value) {
        for (const auto &binding : numeric_bindings) {
            if (binding->field_key != field_key) continue;
            if (binding->editors.size() == 1 && value.is_number()) {
                const QSignalBlocker blocker{binding->editors.front()};
                binding->editors.front()->setValue(value.get<double>());
            } else if (value.is_array() &&
                       value.size() == binding->editors.size()) {
                for (std::size_t index = 0; index < value.size(); ++index) {
                    if (!value[index].is_number()) return;
                    const QSignalBlocker blocker{binding->editors[index]};
                    binding->editors[index]->setValue(
                        value[index].get<double>());
                }
            }
            return;
        }
    }

    bool previewGizmoEdit(std::string_view field_key, Json value) {
        const Json reflected = value;
        const bool accepted =
            model.previewWidgetValue(field_key, std::move(value));
        if (accepted) reflectGizmoValue(field_key, reflected);
        synchronize();
        dispatchRequests();
        return accepted;
    }

    void finishGizmoEdit(std::string_view field_key, bool commit) {
        model.finishWidgetEdit(field_key, commit);
        synchronize();
        dispatchRequests();
    }

    bool saveScene() {
        const bool started = model.saveScene();
        synchronize();
        dispatchRequests();
        return started;
    }

    void dispatchRequests() {
        for (;;) {
            auto requests = model.takeRpcRequests();
            if (requests.empty()) {
                return;
            }
            for (auto &request : requests) {
                QJsonParseError parse_error;
                const QJsonDocument params_document =
                    QJsonDocument::fromJson(
                        QByteArray::fromStdString(request.params.dump()),
                        &parse_error);
                if (parse_error.error != QJsonParseError::NoError ||
                    !params_document.isObject()) {
                    model.receiveRpcFailure(
                        request.request_id,
                        "Inspector generated invalid RPC parameters.");
                    continue;
                }

                QString error;
                const qint64 transport_request_id = viewport.requestRpc(
                    QString::fromStdString(request.method),
                    params_document.object(), &error);
                if (transport_request_id == 0) {
                    model.receiveRpcFailure(request.request_id,
                                            error.toStdString());
                    continue;
                }
                transport_requests.insert(
                    transport_request_id,
                    static_cast<quint64>(request.request_id));
            }
            synchronize();
        }
    }

    void synchronize() {
        const InspectorNotice &notice = model.notice();
        notice_label->setText(QString::fromStdString(notice.message));
        notice_label->setVisible(!notice.message.empty());
        switch (notice.kind) {
        case InspectorNoticeKind::Error:
            notice_label->setStyleSheet(
                QStringLiteral("color: #d94c3d;"));
            break;
        case InspectorNoticeKind::Success:
            notice_label->setStyleSheet(
                QStringLiteral("color: #388e3c;"));
            break;
        case InspectorNoticeKind::Information:
            notice_label->setStyleSheet(
                QStringLiteral("color: #4f7cac;"));
            break;
        case InspectorNoticeKind::None:
            notice_label->setStyleSheet({});
            break;
        }

        refresh_button->setEnabled(model.selection().has_value() &&
                                   !model.refreshBlocked() && !model.busy());
        undo_button->setEnabled(model.canUndoRedo());
        redo_button->setEnabled(model.canUndoRedo());
        cancel_preview_button->setEnabled(model.busy() &&
                                          model.refreshBlocked());

        const bool field_interaction_locked =
            model.refreshBlocked() || model.busy();
        for (QWidget *editor : direct_editors) {
            editor->setEnabled(!field_interaction_locked);
        }
        for (const auto &binding : numeric_bindings) {
            for (QDoubleSpinBox *editor : binding->editors) {
                editor->setEnabled(!field_interaction_locked ||
                                   binding->active);
            }
        }
        for (const auto &binding : text_bindings) {
            binding->editor->setEnabled(!field_interaction_locked ||
                                        binding->active);
        }

        if (rendered_content_revision != model.contentRevision()) {
            rebuildProperties();
        }
    }

    void clearBindings() {
        numeric_events.clear();
        text_events.clear();
        direct_editors.clear();
        numeric_bindings.clear();
        text_bindings.clear();
    }

    void rebuildProperties() {
        rebuilding = true;
        clearBindings();
        QWidget *previous = scroll->takeWidget();
        delete previous;

        auto *contents = new QWidget(scroll);
        auto *layout = new QVBoxLayout(contents);
        layout->setContentsMargins(2, 2, 2, 2);
        layout->setSpacing(8);

        if (!model.selection()) {
            layout->addWidget(
                plainLabel(owner.tr("No object selected."), contents));
        } else if (!model.snapshot()) {
            layout->addWidget(plainLabel(
                owner.tr("Loading editable properties..."), contents));
        } else if (model.snapshot()->components.empty()) {
            layout->addWidget(plainLabel(
                owner.tr("No editable properties."), contents));
        } else {
            if (model.snapshot()->prefab_instance) {
                auto *instance = plainLabel(
                    QString::fromStdString(
                        model.snapshot()->prefab_instance->dump(2)), contents);
                instance->setObjectName(
                    QStringLiteral("pelican.inspectorPrefabInstance"));
                layout->addWidget(instance);
            }
            for (const InspectorComponentSnapshot &component :
                 model.snapshot()->components) {
                addComponent(*layout, contents, component);
            }
        }
        layout->addStretch();
        scroll->setWidget(contents);
        rendered_content_revision = model.contentRevision();
        rebuilding = false;
    }

    void addComponent(QVBoxLayout &layout, QWidget *parent,
                      const InspectorComponentSnapshot &component) {
        auto *group =
            new QGroupBox(QString::fromStdString(component.title), parent);
        auto *form = new QFormLayout(group);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        if (!component.read_only_reason.empty()) {
            auto *reason = plainLabel(
                QString::fromStdString(component.read_only_reason), group);
            reason->setStyleSheet(QStringLiteral("color: #b36b00;"));
            form->addRow(reason);
        }
        for (const InspectorWidgetDescriptor &descriptor : component.widgets) {
            addField(*form, group, descriptor);
        }
        if (component.widgets.empty()) {
            auto *json = plainLabel(
                QString::fromStdString(component.authored_json.dump(2)), group);
            json->setObjectName(QStringLiteral("pelican.inspectorReadOnlyJson"));
            form->addRow(component.generated ? owner.tr("Resolved JSON")
                                             : owner.tr("Authored JSON"),
                         json);
        }
        if (component.generated) {
            auto *source = plainLabel(
                QString::fromStdString(component.generated_source.dump(2)), group);
            source->setObjectName(
                QStringLiteral("pelican.inspectorGeneratedSource"));
            form->addRow(owner.tr("Prefab source"), source);
        }
        if (component.runtime_json &&
            *component.runtime_json != component.authored_json) {
            auto *runtime = plainLabel(
                QString::fromStdString(component.runtime_json->dump(2)), group);
            runtime->setObjectName(
                QStringLiteral("pelican.inspectorRuntimeJson"));
            form->addRow(owner.tr("Runtime (read-only)"), runtime);
        }
        layout.addWidget(group);
    }

    void addField(QFormLayout &form, QWidget *parent,
                  const InspectorWidgetDescriptor &descriptor) {
        if (!descriptor.authored) {
            form.addRow(fieldLabel(descriptor),
                        plainLabel(owner.tr("Not authored"), parent));
            return;
        }
        if (!descriptor.value_matches_schema) {
            auto *invalid = plainLabel(
                owner.tr("Authored value does not match schema"), parent);
            invalid->setStyleSheet(QStringLiteral("color: #d94c3d;"));
            form.addRow(fieldLabel(descriptor), invalid);
            return;
        }

        switch (descriptor.kind) {
        case InspectorWidgetKind::SignedIntegerDrag:
        case InspectorWidgetKind::UnsignedIntegerDrag:
        case InspectorWidgetKind::FloatingPointDrag:
            form.addRow(fieldLabel(descriptor),
                        makeNumericEditor(parent, descriptor));
            break;
        case InspectorWidgetKind::VectorDrag:
        case InspectorWidgetKind::QuaternionDrag:
            form.addRow(fieldLabel(descriptor),
                        makeVectorEditor(parent, descriptor));
            break;
        case InspectorWidgetKind::BooleanCheckbox: {
            auto *editor = new QCheckBox(parent);
            editor->setChecked(descriptor.value.get<bool>());
            editor->setObjectName(
                QStringLiteral("pelican.inspectorBoolean"));
            direct_editors.push_back(editor);
            QObject::connect(editor, &QCheckBox::toggled, &owner,
                             [this, key = descriptor.field_key](bool value) {
                                 if (rebuilding) {
                                     return;
                                 }
                                 model.commitWidgetValue(key, value);
                                 synchronize();
                                 dispatchRequests();
                             });
            form.addRow(fieldLabel(descriptor), editor);
            break;
        }
        case InspectorWidgetKind::EnumCombo: {
            auto *editor = new QComboBox(parent);
            editor->setObjectName(QStringLiteral("pelican.inspectorEnum"));
            direct_editors.push_back(editor);
            for (const std::string &candidate : descriptor.enum_values) {
                editor->addItem(QString::fromStdString(candidate));
            }
            editor->setCurrentText(
                QString::fromStdString(descriptor.value.get<std::string>()));
            QObject::connect(
                editor, &QComboBox::currentTextChanged, &owner,
                [this, key = descriptor.field_key](const QString &value) {
                    if (rebuilding) {
                        return;
                    }
                    model.commitWidgetValue(key, value.toStdString());
                    synchronize();
                    dispatchRequests();
                });
            form.addRow(fieldLabel(descriptor), editor);
            break;
        }
        case InspectorWidgetKind::StringInput: {
            auto binding = std::make_unique<TextBinding>();
            binding->field_key = descriptor.field_key;
            binding->editor = new QLineEdit(
                QString::fromStdString(descriptor.value.get<std::string>()),
                parent);
            binding->editor->setObjectName(
                QStringLiteral("pelican.inspectorString"));
            TextBinding *binding_pointer = binding.get();
            text_bindings.push_back(std::move(binding));
            registerTextEvents(binding_pointer->editor, binding_pointer);
            QObject::connect(
                binding_pointer->editor, &QLineEdit::textEdited, &owner,
                [this, binding_pointer](const QString &value) {
                    if (rebuilding) {
                        return;
                    }
                    beginText(*binding_pointer);
                    model.stageWidgetValue(binding_pointer->field_key,
                                           value.toStdString());
                    synchronize();
                });
            QObject::connect(binding_pointer->editor,
                             &QLineEdit::editingFinished, &owner,
                             [this, binding_pointer] {
                                 finishText(*binding_pointer, true);
                             });
            form.addRow(fieldLabel(descriptor), binding_pointer->editor);
            break;
        }
        }
    }

    void configureNumeric(QDoubleSpinBox &editor,
                          const InspectorWidgetDescriptor &descriptor,
                          bool vector_component) {
        const bool integer =
            descriptor.kind == InspectorWidgetKind::SignedIntegerDrag ||
            descriptor.kind == InspectorWidgetKind::UnsignedIntegerDrag;
        const bool unsigned_integer =
            descriptor.kind == InspectorWidgetKind::UnsignedIntegerDrag;
        const double fallback_minimum =
            unsigned_integer ? 0.0
                             : (integer ? -MaximumExactJsonInteger : -1.0e100);
        const double fallback_maximum =
            integer ? MaximumExactJsonInteger : 1.0e100;
        editor.setRange(descriptor.range_min.value_or(fallback_minimum),
                        descriptor.range_max.value_or(fallback_maximum));
        editor.setDecimals(integer ? 0 : (vector_component ? 4 : 6));
        if (integer) {
            editor.setSingleStep(1.0);
        } else if (descriptor.range_min && descriptor.range_max) {
            editor.setSingleStep(std::max(
                (*descriptor.range_max - *descriptor.range_min) / 200.0,
                0.0001));
        } else {
            editor.setSingleStep(vector_component ? 0.01 : 0.1);
        }
        editor.setAccelerated(true);
        editor.setKeyboardTracking(true);
        editor.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (!vector_component && !descriptor.unit.empty()) {
            editor.setSuffix(
                QStringLiteral(" %1").arg(QString::fromStdString(descriptor.unit)));
        }
    }

    QWidget *makeNumericEditor(QWidget *parent,
                               const InspectorWidgetDescriptor &descriptor) {
        auto binding = std::make_unique<NumericBinding>();
        binding->field_key = descriptor.field_key;
        binding->kind = descriptor.kind;
        auto *editor = new QDoubleSpinBox(parent);
        editor->setObjectName(QStringLiteral("pelican.inspectorNumeric"));
        configureNumeric(*editor, descriptor, false);
        editor->setValue(descriptor.value.get<double>());
        binding->editors.push_back(editor);
        NumericBinding *binding_pointer = binding.get();
        numeric_bindings.push_back(std::move(binding));
        registerNumericEvents(editor, binding_pointer);
        QObject::connect(
            editor, qOverload<double>(&QDoubleSpinBox::valueChanged), &owner,
            [this, binding_pointer](double) {
                numericChanged(*binding_pointer);
            });
        QObject::connect(editor, &QDoubleSpinBox::editingFinished, &owner,
                         [this, binding_pointer] {
                             finishNumeric(*binding_pointer, true);
                         });
        return editor;
    }

    QWidget *makeVectorEditor(QWidget *parent,
                              const InspectorWidgetDescriptor &descriptor) {
        auto binding = std::make_unique<NumericBinding>();
        binding->field_key = descriptor.field_key;
        binding->kind = descriptor.kind;
        auto *container = new QWidget(parent);
        auto *layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(3);
        for (std::size_t index = 0; index < descriptor.columns; ++index) {
            auto *editor = new QDoubleSpinBox(container);
            editor->setObjectName(
                QStringLiteral("pelican.inspectorVector%1").arg(index));
            configureNumeric(*editor, descriptor, true);
            editor->setValue(descriptor.value.at(index).get<double>());
            binding->editors.push_back(editor);
            layout->addWidget(editor, 1);
        }
        NumericBinding *binding_pointer = binding.get();
        numeric_bindings.push_back(std::move(binding));
        for (QDoubleSpinBox *editor : binding_pointer->editors) {
            registerNumericEvents(editor, binding_pointer);
            QObject::connect(
                editor, qOverload<double>(&QDoubleSpinBox::valueChanged),
                &owner, [this, binding_pointer](double) {
                    numericChanged(*binding_pointer);
                });
            QObject::connect(editor, &QDoubleSpinBox::editingFinished, &owner,
                             [this, binding_pointer] {
                                 finishNumeric(*binding_pointer, true);
                             });
        }
        return container;
    }

    void registerNumericEvents(QObject *object, NumericBinding *binding) {
        numeric_events.insert_or_assign(object, binding);
        object->installEventFilter(&owner);
        for (QObject *child : object->children()) {
            numeric_events.insert_or_assign(child, binding);
            child->installEventFilter(&owner);
        }
    }

    void registerTextEvents(QObject *object, TextBinding *binding) {
        text_events.insert_or_assign(object, binding);
        object->installEventFilter(&owner);
        for (QObject *child : object->children()) {
            text_events.insert_or_assign(child, binding);
            child->installEventFilter(&owner);
        }
    }

    Json numericValue(const NumericBinding &binding) const {
        if (binding.editors.size() > 1) {
            Json result = Json::array();
            for (const QDoubleSpinBox *editor : binding.editors) {
                result.push_back(editor->value());
            }
            return result;
        }
        const double value = binding.editors.front()->value();
        if (binding.kind == InspectorWidgetKind::SignedIntegerDrag) {
            return static_cast<std::int64_t>(std::llround(value));
        }
        if (binding.kind == InspectorWidgetKind::UnsignedIntegerDrag) {
            return static_cast<std::uint64_t>(std::llround(value));
        }
        return value;
    }

    void beginNumeric(NumericBinding &binding) {
        if (binding.active || rebuilding) {
            return;
        }
        binding.active = model.beginWidgetEdit(binding.field_key);
        synchronize();
    }

    void numericChanged(NumericBinding &binding) {
        if (rebuilding) {
            return;
        }
        beginNumeric(binding);
        if (!binding.active) {
            return;
        }
        model.previewWidgetValue(binding.field_key, numericValue(binding));
        synchronize();
        dispatchRequests();
    }

    void finishNumeric(NumericBinding &binding, bool commit) {
        if (!binding.active || rebuilding) {
            return;
        }
        binding.active = false;
        model.finishWidgetEdit(binding.field_key, commit);
        synchronize();
        dispatchRequests();
    }

    void beginText(TextBinding &binding) {
        if (binding.active || rebuilding) {
            return;
        }
        binding.active = model.beginWidgetEdit(binding.field_key);
        synchronize();
    }

    void finishText(TextBinding &binding, bool commit) {
        if (!binding.active || rebuilding) {
            return;
        }
        binding.active = false;
        model.finishWidgetEdit(binding.field_key, commit);
        synchronize();
        dispatchRequests();
    }

    bool handleEvent(QObject *watched, QEvent *event) {
        if (const auto numeric = numeric_events.find(watched);
            numeric != numeric_events.end()) {
            NumericBinding &binding = *numeric->second;
            switch (event->type()) {
            case QEvent::FocusIn:
            case QEvent::MouseButtonPress:
                beginNumeric(binding);
                break;
            case QEvent::FocusOut:
            case QEvent::MouseButtonRelease:
                finishNumeric(binding, true);
                break;
            case QEvent::KeyPress:
                if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
                    model.abortActivePreview();
                    finishNumeric(binding, false);
                }
                break;
            default:
                break;
            }
        }
        if (const auto text = text_events.find(watched);
            text != text_events.end()) {
            TextBinding &binding = *text->second;
            switch (event->type()) {
            case QEvent::FocusIn:
                beginText(binding);
                break;
            case QEvent::FocusOut:
                finishText(binding, true);
                break;
            case QEvent::KeyPress:
                if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
                    finishText(binding, false);
                }
                break;
            default:
                break;
            }
        }
        return false;
    }
};

InspectorWidget::InspectorWidget(EmbeddedViewport *viewport, QWidget *parent)
    : QWidget(parent) {
    if (viewport == nullptr) {
        throw std::invalid_argument("InspectorWidget requires an embedded viewport");
    }
    setObjectName(QStringLiteral("pelican.inspector"));
    impl_ = std::make_unique<Impl>(*this, *viewport);
}

InspectorWidget::~InspectorWidget() = default;

void InspectorWidget::setSelection(
    std::optional<OutlinerObjectKey> selection,
    const QString &display_text) {
    impl_->setSelection(std::move(selection), display_text);
}

std::optional<GizmoTransformBinding>
InspectorWidget::gizmoTransformBinding() const {
    return impl_->gizmoTransformBinding();
}

bool InspectorWidget::beginGizmoEdit(std::string_view field_key) {
    return impl_->beginGizmoEdit(field_key);
}

bool InspectorWidget::previewGizmoEdit(std::string_view field_key,
                                       nlohmann::json value) {
    return impl_->previewGizmoEdit(field_key, std::move(value));
}

void InspectorWidget::finishGizmoEdit(std::string_view field_key,
                                      bool commit) {
    impl_->finishGizmoEdit(field_key, commit);
}

bool InspectorWidget::saveScene() { return impl_->saveScene(); }

bool InspectorWidget::eventFilter(QObject *watched, QEvent *event) {
    return impl_->handleEvent(watched, event) ||
           QWidget::eventFilter(watched, event);
}

} // namespace PelicanStudio
