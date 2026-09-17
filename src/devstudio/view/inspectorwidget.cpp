#include "inspectorwidget.hpp"

#include "panelhelp.hpp"
#include "graphattachmentwidget.hpp"

#include "../model/inspectormodel.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QPointer>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QTableWidget>
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
    GraphAttachmentWidget *graph_attachments = nullptr;
    QHash<qint64, quint64> graph_transport_requests;
    QLabel *selection_label = nullptr;
    QLabel *notice_label = nullptr;
    QScrollArea *scroll = nullptr;
    QPushButton *refresh_button = nullptr;
    QPushButton *add_component_button = nullptr;
    QPushButton *ray_occurrences_button = nullptr;
    QPointer<QDialog> ray_dialog;
    QTableWidget *ray_entries = nullptr;
    QTableWidget *ray_overrides = nullptr;
    QTableWidget *ray_settings = nullptr;
    QPlainTextEdit *ray_rejections = nullptr;
    QComboBox *ray_destination = nullptr;
    QPushButton *ray_add = nullptr;
    QLabel *ray_notice = nullptr;
    QPushButton *ray_tasks = nullptr;
    std::function<void(const QString &)> ray_group_action;
    std::function<void(const QString &, const QString &)> graph_source_reveal_action;
    std::optional<Json> displayed_ray_payload;
    QPointer<QDialog> add_component_dialog;
    QComboBox *component_types = nullptr;
    QLabel *component_description = nullptr;
    QPlainTextEdit *component_declaration = nullptr;
    QListWidget *component_assets = nullptr;
    QPushButton *use_component_assets = nullptr;
    QLabel *component_assets_notice = nullptr;
    std::optional<Json> displayed_asset_selection;
    QString asset_selection_component;
    QHash<QString, QString> component_drafts;
    QString displayed_component;
    QPushButton *confirm_add_component = nullptr;
    QPushButton *refresh_component_types = nullptr;
    std::optional<Json> displayed_candidates;
    std::string adding_scene;
    std::uint64_t adding_object = 0;
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
    std::vector<std::pair<QPushButton *, std::size_t>> remove_buttons;
    std::vector<std::pair<QWidget *, std::string>> list_controls;
    std::vector<std::pair<QPushButton *, std::string>> default_buttons;
    std::unordered_map<QObject *, NumericBinding *> numeric_events;
    std::unordered_map<QObject *, TextBinding *> text_events;

    explicit Impl(InspectorWidget &widget, EmbeddedViewport &embedded_viewport)
        : owner{widget}, viewport{embedded_viewport} {
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        owner.setFont(studioPanelFont());

        auto *selection_row = new QHBoxLayout;
        auto *selection_form = new QFormLayout;
        selection_label = new QLabel(owner.tr("None"), &owner);
        selection_label->setObjectName(
            QStringLiteral("pelican.inspectorSelection"));
        selection_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        selection_form->addRow(owner.tr("Selection"), selection_label);
        selection_row->addLayout(selection_form, 1);
        selection_row->addWidget(makePanelHelpButton(
            &owner, "pelican.inspector.help", ManualInspectorAnchor,
            owner.tr("Edit the declaration, then Add. Use registered asset "
                     "names for asset fields; the engine validates the "
                     "values.")));
        layout->addLayout(selection_row);

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
        add_component_button = new QPushButton(owner.tr("Add component"), &owner);
        add_component_button->setObjectName(QStringLiteral("pelican.inspectorAddComponent"));
        layout->addWidget(add_component_button);
        QObject::connect(add_component_button, &QPushButton::clicked, &owner, [this] {
            openComponentAddDialog();
        });
        ray_occurrences_button = new QPushButton(owner.tr("Ray geometry"), &owner);
        ray_occurrences_button->setObjectName(QStringLiteral("pelican.inspectorRayGeometry"));
        layout->addWidget(ray_occurrences_button);
        QObject::connect(ray_occurrences_button, &QPushButton::clicked, &owner, [this] {
            openRayOccurrences();
        });
        graph_attachments = new GraphAttachmentWidget(&owner);
        layout->addWidget(graph_attachments);
        QObject::connect(graph_attachments, &GraphAttachmentWidget::rpcRequested, &owner,
            [this](quint64 id, const QString &method, const QJsonObject &params) {
                QString error;
                const auto transport_id = viewport.requestRpc(method, params, &error);
                if (transport_id)
                    graph_transport_requests.insert(transport_id, id);
                else
                    graph_attachments->receiveRpcFailure(id, error);
            });
        QObject::connect(graph_attachments,
                         &GraphAttachmentWidget::sourceRevealRequested, &owner,
                         [this](const QString &node, const QString &port) {
                             if (graph_source_reveal_action)
                                 graph_source_reveal_action(node, port);
                         });

        notice_label = new QLabel(&owner);
        notice_label->setObjectName(QStringLiteral("pelican.inspectorNotice"));
        notice_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        applyPanelNoticeStyle(notice_label);
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
                             graph_transport_requests.clear();
                             graph_attachments->setConnected(true);
                             model.startSession();
                             synchronize();
                             dispatchRequests();
                         });
        QObject::connect(
            &viewport, &EmbeddedViewport::engineRpcBecameUnavailable, &owner,
            [this](const QString &message) {
                transport_requests.clear();
                graph_transport_requests.clear();
                graph_attachments->setConnected(false);
                model.stopSession(message.toStdString());
                synchronize();
            });
        QObject::connect(
            &viewport, &EmbeddedViewport::inspectorRpcSucceeded, &owner,
            [this](qint64 transport_request_id,
                   const QByteArray &result_json) {
                const auto graph_pending = graph_transport_requests.find(transport_request_id);
                if (graph_pending != graph_transport_requests.end()) {
                    const auto id = graph_pending.value();
                    graph_transport_requests.erase(graph_pending);
                    graph_attachments->receiveRpcResult(id, result_json);
                    return;
                }
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
                const auto graph_pending = graph_transport_requests.find(transport_request_id);
                if (graph_pending != graph_transport_requests.end()) {
                    const auto id = graph_pending.value();
                    graph_transport_requests.erase(graph_pending);
                    graph_attachments->receiveRpcFailure(id, message);
                    return;
                }
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

        // Q-2 looked at stopping these two while there is nothing to poll for,
        // and left them running: they are not only polls. Beginning a widget
        // edit, previewing a value, asking for the component-add candidates
        // and editing a schema list all queue requests on the model and none
        // of them flushes the queue itself -- the ticks do, through
        // dispatchRequests(). Idle they ask the engine for nothing (the model
        // guards both polls), so the cost is UI-thread wakeups, not RPC;
        // making them stoppable means first giving every one of those paths a
        // flush of its own, which is a change of its own.
        pending_timer->start();
        watch_timer->start();
        rebuildProperties();
        synchronize();
        if (viewport.rpcReady()) {
            graph_attachments->setConnected(true);
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

    void openComponentAddDialog() {
        if (add_component_dialog) {
            add_component_dialog->raise();
            return;
        }
        if (!model.requestComponentAddCandidates()) return;
        adding_scene = model.snapshot()->scene_id;
        adding_object = model.snapshot()->authoring_object_id;
        displayed_candidates.reset();
        component_drafts.clear();
        displayed_component.clear();
        displayed_asset_selection.reset();
        asset_selection_component.clear();
        auto *dialog = new QDialog(&owner);
        add_component_dialog = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(owner.tr("Add component"));
        dialog->setObjectName(QStringLiteral("pelican.componentAddDialog"));
        dialog->setWindowModality(Qt::WindowModal);
        dialog->resize(560, 440);
        auto *layout = new QVBoxLayout(dialog);
        component_types = new QComboBox(dialog);
        component_types->setObjectName(QStringLiteral("pelican.componentAddTypes"));
        layout->addWidget(component_types);
        component_description = plainLabel(owner.tr("Loading component types..."), dialog);
        layout->addWidget(component_description);
        component_declaration = new QPlainTextEdit(dialog);
        component_declaration->setObjectName(QStringLiteral("pelican.componentAddDeclaration"));
        layout->addWidget(component_declaration);
        QObject::connect(component_declaration, &QPlainTextEdit::textChanged, dialog, [this] {
            if (!displayed_component.isEmpty()) component_drafts[displayed_component] = component_declaration->toPlainText();
        });
        component_assets_notice = plainLabel({}, dialog);
        layout->addWidget(component_assets_notice);
        component_assets = new QListWidget(dialog);
        component_assets->setObjectName(QStringLiteral("pelican.componentAddAssets"));
        component_assets->setMaximumHeight(140);
        layout->addWidget(component_assets);
        use_component_assets = new QPushButton(owner.tr("Use selected assets"), dialog);
        use_component_assets->setObjectName(QStringLiteral("pelican.componentAddUseAssets"));
        layout->addWidget(use_component_assets);
        QObject::connect(use_component_assets, &QPushButton::clicked, dialog, [this] {
            useSelectedComponentAssets();
        });
        auto *buttons = new QHBoxLayout;
        refresh_component_types = new QPushButton(owner.tr("Refresh types"), dialog);
        confirm_add_component = new QPushButton(owner.tr("Add"), dialog);
        confirm_add_component->setObjectName(QStringLiteral("pelican.componentAddConfirm"));
        buttons->addWidget(refresh_component_types);
        buttons->addWidget(confirm_add_component);
        layout->addLayout(buttons);
        QObject::connect(component_types, &QComboBox::currentIndexChanged, dialog, [this] {
            synchronizeComponentAddDialog();
        });
        QObject::connect(refresh_component_types, &QPushButton::clicked, dialog, [this] {
            model.requestComponentAddCandidates();
            synchronize();
            dispatchRequests();
        });
        QObject::connect(confirm_add_component, &QPushButton::clicked, dialog, [this] {
            const auto declaration = Json::parse(component_declaration->toPlainText().toStdString(), nullptr, false);
            if (declaration.is_discarded() || !declaration.is_object()) {
                component_description->setText(owner.tr("Component JSON must be a valid object."));
                return;
            }
            if (!model.addComponent(component_types->currentData().toString().toStdString(), declaration)) {
                component_description->setText(owner.tr("Keep the selected component name unchanged and refresh types if the scene changed."));
                return;
            }
            synchronize();
            dispatchRequests();
        });
        synchronizeComponentAddDialog();
        dialog->show();
        dispatchRequests();
    }

    void useSelectedComponentAssets() {
        if (!model.canRequestComponentAddCandidates() || !displayed_asset_selection) return;
        const auto &catalog = model.componentAddCandidates();
        if (!catalog || component_types->currentData().toString() != asset_selection_component) return;
        auto declaration = Json::parse(component_declaration->toPlainText().toStdString(), nullptr, false);
        if (!declaration.is_object()) {
            component_assets_notice->setText(owner.tr("Fix the component JSON before choosing assets; the draft is unchanged."));
            return;
        }
        const auto selected = component_assets->selectedItems();
        if (selected.isEmpty()) {
            component_assets_notice->setText(owner.tr("Select an asset first."));
            return;
        }
        const auto &selection = *displayed_asset_selection;
        const auto field = selection.at("field").get<std::string>();
        if (selection.at("multiple").get<bool>()) {
            const auto single = selection.value("single_field", std::string{});
            declaration.erase(field);
            if (!single.empty()) declaration.erase(single);
            if (selected.size() == 1 && !single.empty()) {
                declaration[single] = selected.front()->data(Qt::UserRole).toString().toStdString();
            } else {
                auto values = Json::array();
                for (const auto *item : selected) values.push_back(item->data(Qt::UserRole).toString().toStdString());
                declaration[field] = std::move(values);
            }
        } else {
            declaration[field] = selected.front()->data(Qt::UserRole).toString().toStdString();
        }
        component_declaration->setPlainText(QString::fromStdString(declaration.dump(2)));
        component_assets_notice->setText(owner.tr("Asset selection copied to the draft. Press Add to submit it."));
    }

    void synchronizeComponentAssets(const Json &candidate, bool ready) {
        const auto selection = candidate.find("asset_selection");
        const bool present = selection != candidate.end();
        component_assets->setVisible(present);
        component_assets_notice->setVisible(present);
        use_component_assets->setVisible(present);
        if (!present) {
            displayed_asset_selection.reset();
            asset_selection_component.clear();
            return;
        }
        const auto key = QString::fromStdString(candidate.at("key").get<std::string>());
        if (asset_selection_component != key || !displayed_asset_selection || *displayed_asset_selection != *selection) {
            asset_selection_component = key;
            displayed_asset_selection = *selection;
            component_assets->clear();
            component_assets->setSelectionMode(selection->at("multiple").get<bool>()
                ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
            for (const auto &choice : selection->at("choices")) {
                const auto id = QString::fromStdString(choice.at("id").get<std::string>());
                const auto path = QString::fromStdString(choice.at("path").get<std::string>());
                const auto status = QString::fromStdString(choice.at("status").get<std::string>());
                auto *item = new QListWidgetItem(QStringLiteral("%1 [%2]").arg(id, status), component_assets);
                item->setToolTip(path);
                item->setData(Qt::UserRole, id);
            }
            component_assets_notice->setText(component_assets->count() == 0
                ? owner.tr("No registered assets are available for this component.")
                : owner.tr("Choose registered assets, then use the selection to update only the asset field in the JSON draft."));
        }
        component_assets->setEnabled(ready);
        use_component_assets->setEnabled(ready && component_assets->count() > 0);
    }

    void synchronizeComponentAddDialog() {
        if (!add_component_dialog) return;
        if (!model.snapshot() || model.snapshot()->scene_id != adding_scene ||
            model.snapshot()->authoring_object_id != adding_object) {
            add_component_dialog->close();
            return;
        }
        const auto &catalog = model.componentAddCandidates();
        if (catalog != displayed_candidates) {
            displayed_candidates = catalog;
            const QSignalBlocker blocker{component_types};
            component_types->clear();
            if (catalog) {
                for (const auto &candidate : catalog->at("candidates")) {
                    const auto key = QString::fromStdString(candidate.at("key").get<std::string>());
                    const auto label = candidate.contains("behavior_type")
                        ? owner.tr("Behavior: %1").arg(QString::fromStdString(candidate.at("behavior_type").get<std::string>()))
                        : QString::fromStdString(candidate.at("name").get<std::string>());
                    component_types->addItem(label, key);
                }
                const int previous = component_types->findData(displayed_component);
                if (previous >= 0) component_types->setCurrentIndex(previous);
            }
        }
        const bool ready = model.canRequestComponentAddCandidates();
        refresh_component_types->setEnabled(ready);
        component_types->setEnabled(ready && catalog.has_value());
        confirm_add_component->setEnabled(false);
        component_assets->setEnabled(false);
        use_component_assets->setEnabled(false);
        component_declaration->setReadOnly(!ready || !catalog);
        if (!catalog) {
            component_description->setText(QString::fromStdString(model.notice().message));
            return;
        }
        const int index = component_types->currentIndex();
        if (index < 0) {
            component_description->setText(owner.tr("No additional registered component types."));
            return;
        }
        const auto &candidate = catalog->at("candidates").at(static_cast<std::size_t>(index));
        synchronizeComponentAssets(candidate, ready);
        const auto name = QString::fromStdString(candidate.at("key").get<std::string>());
        if (displayed_component != name || !component_drafts.contains(name)) {
            displayed_component = name;
            if (!component_drafts.contains(name)) {
                const auto initial = candidate.at("component_template");
                component_drafts.insert(name, QString::fromStdString(initial.dump(2)));
            }
            const QSignalBlocker blocker{component_declaration};
            component_declaration->setPlainText(component_drafts.value(name));
        }
        QStringList fields;
        for (const auto &field : candidate.at("schema_fields")) {
            fields.push_back(QStringLiteral("%1 (%2)").arg(
                QString::fromStdString(field.value("name", "")),
                QString::fromStdString(field.value("type", ""))));
        }
        confirm_add_component->setEnabled(ready);
        component_declaration->setToolTip(owner.tr("Edit the declaration, then Add. Use registered asset names for asset fields; the engine validates the values."));
        QString description;
        if (!fields.isEmpty()) description = (candidate.contains("behavior_type")
            ? owner.tr("Params: %1") : owner.tr("Fields: %1")).arg(fields.join(", "));
        if (!candidate.value("available", false))
            description += "\n" + QString::fromStdString(candidate.value("unavailable_reason", ""));
        if (model.notice().kind == InspectorNoticeKind::Error)
            description += "\n" + QString::fromStdString(model.notice().message);
        setPanelNotice(component_description, oneLine(description),
                       description);
    }

    void openRayOccurrences() {
        if (ray_dialog) { ray_dialog->raise(); return; }
        auto *dialog = new QDialog(&owner);
        ray_dialog = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(owner.tr("Ray geometry and overrides"));
        dialog->resize(1000, 820);
        auto *layout = new QVBoxLayout(dialog);
        ray_notice = plainLabel({}, dialog);
        layout->addWidget(ray_notice);
        ray_entries = new QTableWidget(0, 7, dialog);
        ray_entries->setObjectName(QStringLiteral("pelican.rayGeometry.entries"));
        ray_entries->setHorizontalHeaderLabels({owner.tr("Geometry"), owner.tr("Asset"),
            owner.tr("Material"), owner.tr("Eligibility"), owner.tr("Effective values"), owner.tr("Occurrence ID"), owner.tr("Resolved binding")});
        ray_entries->setSelectionBehavior(QAbstractItemView::SelectRows);
        ray_entries->setSelectionMode(QAbstractItemView::SingleSelection);
        ray_entries->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ray_entries->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(ray_entries, 2);
        ray_settings = new QTableWidget(0, 4, dialog);
        ray_settings->setObjectName(QStringLiteral("pelican.rayGeometry.settingSources"));
        ray_settings->setHorizontalHeaderLabels({owner.tr("Setting"), owner.tr("Resolved value"),
            owner.tr("Applied source"), owner.tr("Priority (low → high)")});
        ray_settings->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ray_settings->setSelectionMode(QAbstractItemView::NoSelection);
        ray_settings->horizontalHeader()->setStretchLastSection(true);
        ray_settings->setMaximumHeight(170);
        layout->addWidget(ray_settings);
        // N-7 rule 1: three paragraphs, each on the control it is about.
        ray_entries->setToolTip(owner.tr(
            "Select geometry to inspect its settings. Omitted fields inherit; an explicit zero overrides. Layout v1 uses row q as custom index."));
        ray_settings->setToolTip(owner.tr(
            "Record fields show resolved scene-plan values. Per-frame dynamic record updates are not shown."));
        auto *actions = new QHBoxLayout;
        ray_destination = new QComboBox(dialog);
        ray_destination->setToolTip(owner.tr(
            "Select geometry and a ray_instance destination to add an override. Edit its values in the Inspector list."));
        ray_destination->setObjectName(QStringLiteral("pelican.rayGeometry.destination"));
        actions->addWidget(ray_destination, 1);
        ray_add = new QPushButton(owner.tr("Add override"), dialog);
        ray_add->setObjectName(QStringLiteral("pelican.rayGeometry.addOverride"));
        actions->addWidget(ray_add);
        ray_tasks = new QPushButton(owner.tr("RT tasks"), dialog);
        ray_tasks->setObjectName(QStringLiteral("pelican.rayGeometry.tasks"));
        actions->addWidget(ray_tasks);
        QObject::connect(ray_tasks, &QPushButton::clicked, dialog, [this] {
            if (ray_group_action) ray_group_action(ray_tasks->property("hitGroup").toString());
        });
        auto *refresh = new QPushButton(owner.tr("Refresh"), dialog);
        actions->addWidget(refresh);
        layout->addLayout(actions);
        layout->addWidget(plainLabel(owner.tr("Override sources and applied values"), dialog));
        ray_overrides = new QTableWidget(0, 5, dialog);
        ray_overrides->setObjectName(QStringLiteral("pelican.rayGeometry.overrides"));
        ray_overrides->setHorizontalHeaderLabels({owner.tr("Row ID"), owner.tr("Component source"),
            owner.tr("Occurrence ID"), owner.tr("Status"), owner.tr("Effective values")});
        ray_overrides->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ray_overrides->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(ray_overrides, 1);
        ray_rejections = new QPlainTextEdit(dialog);
        ray_rejections->setReadOnly(true);
        ray_rejections->setMaximumHeight(120);
        ray_rejections->setPlaceholderText(owner.tr("No rejected overrides."));
        layout->addWidget(ray_rejections);
        QObject::connect(refresh, &QPushButton::clicked, dialog, [this] {
            model.requestRayOccurrences(); synchronize(); dispatchRequests();
        });
        QObject::connect(ray_entries, &QTableWidget::itemSelectionChanged, dialog, [this] { synchronizeRayDialog(); });
        QObject::connect(ray_destination, qOverload<int>(&QComboBox::currentIndexChanged), dialog,
                         [this](int) { synchronizeRayDialog(); });
        QObject::connect(ray_add, &QPushButton::clicked, dialog, [this] {
            const auto *item = ray_entries->item(ray_entries->currentRow(), 0);
            if (!item) return;
            model.addRayOccurrenceOverride(ray_destination->currentData().toString().toStdString(),
                                            item->data(Qt::UserRole).toString().toStdString());
            synchronize(); dispatchRequests();
        });
        displayed_ray_payload.reset();
        model.requestRayOccurrences();
        synchronize(); dispatchRequests();
        dialog->open();
    }

    void synchronizeRayDialog() {
        if (!ray_dialog) return;
        const auto &payload = model.rayOccurrences();
        if (!payload || displayed_ray_payload != payload) {
            const QSignalBlocker entry_block{ray_entries}, destination_block{ray_destination};
            displayed_ray_payload = payload;
            ray_entries->setRowCount(0);
            ray_overrides->setRowCount(0);
            ray_destination->clear();
            ray_rejections->clear();
            if (payload) {
                const auto display = [](const Json &value) {
                    return QString::fromStdString(value.is_string() ? value.get<std::string>()
                                                                   : value.is_null() ? "—" : value.dump());
                };
                for (const auto &entry : payload->at("entries")) {
                    const int row = ray_entries->rowCount();
                    ray_entries->insertRow(row);
                    int column = 0;
                    for (const auto *key : {"display_name", "asset", "material", "eligibility", "effective", "occurrence_id", "binding"})
                        ray_entries->setItem(row, column++, new QTableWidgetItem(display(entry.value(key, Json{}))));
                    ray_entries->item(row, 0)->setData(Qt::UserRole, display(entry.value("occurrence_id", Json{})));
                    ray_entries->item(row, 0)->setData(Qt::UserRole + 1, entry.value("eligibility", Json{}) == "ok");
                }
                for (const auto &entry : payload->at("overrides")) {
                    const int row = ray_overrides->rowCount();
                    ray_overrides->insertRow(row);
                    const auto address = entry.value("address", Json::object());
                    const Json values = Json::array({address.value("row_id", Json{}), address.value("component", Json{}),
                        entry.value("occurrence_id", Json{}), entry.value("status", Json{}), entry.value("effective", Json{})});
                    for (int column = 0; column < 5; ++column)
                        ray_overrides->setItem(row, column, new QTableWidgetItem(display(values.at(column))));
                }
                const auto rejected = payload->value("rejected_rows", Json::array());
                if (!rejected.empty()) ray_rejections->setPlainText(QString::fromStdString(rejected.dump(2)));
                if (model.snapshot()) for (const auto &component : model.snapshot()->components) {
                    if (component.name != "pelican_ray_instance") continue;
                    for (const auto &list : component.lists) if (list.path == "/overrides" && model.canEditList(list.key))
                        ray_destination->addItem(QString::fromStdString(component.title) +
                            QStringLiteral(" [%1]").arg(component.component_index), QString::fromStdString(list.key));
                }
            }
        }
        const auto *selected = ray_entries->item(ray_entries->currentRow(), 0);
        ray_settings->setRowCount(0);
        ray_tasks->setEnabled(false);
        ray_tasks->setProperty("hitGroup", QString{});
        const auto selected_row = ray_entries->currentRow();
        if (payload && selected_row >= 0 && static_cast<std::size_t>(selected_row) < payload->at("entries").size()) {
            const auto binding = payload->at("entries").at(selected_row).value("binding", Json{});
            if (binding.is_object()) {
                const auto group = QString::fromStdString(binding.value("group_name", std::string{}));
                ray_tasks->setProperty("hitGroup", group);
                ray_tasks->setEnabled(ray_group_action && !group.isEmpty());
                const auto sources = binding.value("setting_sources", Json::object());
                const auto display = [](const Json &value) {
                    return QString::fromStdString(value.is_null() ? "—" : value.is_string() ? value.get<std::string>() : value.dump());
                };
                for (const auto *field : {"mask", "instance_flags", "custom_index", "geometry_flags"}) {
                    const int row = ray_settings->rowCount();
                    ray_settings->insertRow(row);
                    const auto source = sources.value(field, std::string{});
                    QString source_label = QString::fromStdString(source);
                    if (source == "engine_default") source_label = owner.tr("Engine default");
                    else if (source == "layout_default") source_label = owner.tr("Layout default");
                    else if (source == "procedural_default") source_label = owner.tr("Procedural geometry default");
                    else if (source == "component") source_label = owner.tr("ray_instance component");
                    else if (source == "geometry_rule") source_label = owner.tr("Geometry rule %1 (0-based)").arg(display(sources.value("geometry_rule_index", Json{})));
                    else if (source == "occurrence_override") source_label = owner.tr("Occurrence override row %1").arg(display(sources.value("override_row_id", Json{})));
                    else if (source == "layout_v1_q") source_label = owner.tr("Layout v1: row q");
                    const auto priority = source == "layout_v1_q" ? owner.tr("Row q (layout v1)")
                        : std::string_view{field} == "geometry_flags"
                        ? owner.tr("Defaults → matching triangle geometry rule; procedural geometry uses its defaults")
                        : owner.tr("Defaults → component → occurrence override");
                    ray_settings->setItem(row, 0, new QTableWidgetItem(QString::fromLatin1(field)));
                    ray_settings->setItem(row, 1, new QTableWidgetItem(display(binding.value(field, Json{}))));
                    ray_settings->setItem(row, 2, new QTableWidgetItem(source_label));
                    auto *priority_item = new QTableWidgetItem(priority);
                    priority_item->setToolTip(priority);
                    ray_settings->setItem(row, 3, priority_item);
                }
                ray_settings->resizeColumnsToContents();
                ray_settings->resizeRowsToContents();
            }
        }
        ray_add->setEnabled(payload && selected && selected->data(Qt::UserRole + 1).toBool() &&
            model.canEditList(ray_destination->currentData().toString().toStdString()));
        ray_notice->setText(!model.notice().message.empty() ? QString::fromStdString(model.notice().message)
            : payload ? owner.tr("Geometry for the selected object. Refresh after edits or model reload.")
                      : owner.tr("Refresh to load geometry for the current selection."));
    }

    void synchronize() {
        const auto &snapshot = model.snapshot();
        graph_attachments->setOwner(snapshot && snapshot->authoring_object_id != 0
            ? std::optional<GraphAttachmentOwner>{{snapshot->scene_id, snapshot->authoring_object_id}}
            : std::nullopt);
        ray_occurrences_button->setEnabled(model.snapshot().has_value() && !model.busy() && !model.refreshBlocked());
        synchronizeRayDialog();
        add_component_button->setEnabled(model.canRequestComponentAddCandidates());
        synchronizeComponentAddDialog();
        // N-7 rule 2: one line, hidden when there is nothing to act on; the
        // whole message stays as the row's tooltip.
        const InspectorNotice &notice = model.notice();
        setPanelNotice(notice_label,
                       oneLine(QString::fromStdString(notice.message)),
                       QString::fromStdString(notice.message));
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
        for (const auto &[button, index] : remove_buttons) {
            button->setEnabled(model.canRemoveComponent(index));
        }
        for (const auto &[control, key] : list_controls) {
            control->setEnabled(model.canEditList(key));
        }
        for (const auto &[button, key] : default_buttons) {
            button->setEnabled(model.canInsertField(key));
        }
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
        remove_buttons.clear();
        list_controls.clear();
        default_buttons.clear();
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
            auto *empty = plainLabel(owner.tr("No object selected"), contents);
            empty->setFont(studioSmallFont());
            layout->addWidget(empty);
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
                auto *parameters = new QTableWidget(
                    static_cast<int>(model.snapshot()->prefab_parameters.size()),
                    3, contents);
                parameters->setObjectName(
                    QStringLiteral("pelican.inspectorPrefabParameters"));
                parameters->setHorizontalHeaderLabels(
                    {owner.tr("Parameter"), owner.tr("Resolved value"),
                     owner.tr("Source")});
                parameters->setEditTriggers(
                    QAbstractItemView::NoEditTriggers);
                parameters->setSelectionMode(
                    QAbstractItemView::NoSelection);
                parameters->verticalHeader()->setVisible(false);
                parameters->horizontalHeader()->setStretchLastSection(true);
                parameters->horizontalHeader()->setSectionResizeMode(
                    0, QHeaderView::ResizeToContents);
                parameters->horizontalHeader()->setSectionResizeMode(
                    1, QHeaderView::Stretch);
                parameters->horizontalHeader()->setSectionResizeMode(
                    2, QHeaderView::ResizeToContents);
                for (std::size_t row = 0;
                     row < model.snapshot()->prefab_parameters.size(); ++row) {
                    const auto &parameter =
                        model.snapshot()->prefab_parameters[row];
                    parameters->setItem(
                        static_cast<int>(row), 0,
                        new QTableWidgetItem(
                            QString::fromStdString(parameter.name)));
                    parameters->setItem(
                        static_cast<int>(row), 1,
                        new QTableWidgetItem(QString::fromStdString(
                            parameter.value_resolved.dump())));
                    parameters->setItem(
                        static_cast<int>(row), 2,
                        new QTableWidgetItem(
                            QString::fromStdString(parameter.source)));
                }
                parameters->resizeRowsToContents();
                layout->addWidget(parameters);
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
        if (!component.generated) {
            auto *remove = new QPushButton(owner.tr("Remove component"), group);
            remove->setObjectName(QStringLiteral("pelican.inspectorRemoveComponent"));
            const auto index = component.component_index;
            remove->setEnabled(model.canRemoveComponent(index));
            remove_buttons.emplace_back(remove, index);
            QObject::connect(remove, &QPushButton::clicked, &owner, [this, index] {
                model.removeComponent(index);
                synchronize();
                dispatchRequests();
            });
            form->addRow(remove);
        }
        for (const InspectorWidgetDescriptor &descriptor : component.widgets) {
            addField(*form, group, descriptor);
        }
        for (const auto &list : component.lists) {
            addList(*form, group, list);
        }
        if (component.widgets.empty() && component.lists.empty()) {
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

    void editListRowDialog(const InspectorListDescriptor &list,
                           std::optional<std::uint32_t> row_id, Json value) {
        if (!model.canEditList(list.key) || !model.snapshot()) return;
        const auto revision = model.snapshot()->scene_revision;
        const auto object_id = model.snapshot()->authoring_object_id;
        const auto scene_id = model.snapshot()->scene_id;
        auto *dialog = new QDialog(&owner);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(row_id ? owner.tr("Edit row") : owner.tr("Add row"));
        dialog->resize(540, 440);
        auto *layout = new QVBoxLayout(dialog);
        auto *editor = new QPlainTextEdit(dialog);
        editor->setObjectName(QStringLiteral("pelican.inspectorListRowJson"));
        editor->setToolTip(owner.tr(
            "Enter row values as JSON. Row IDs are managed by the engine."));
        value.erase("$row");
        editor->setPlainText(QString::fromStdString(value.dump(2)));
        layout->addWidget(editor, 1);
        auto *schema = new QPlainTextEdit(dialog);
        schema->setReadOnly(true);
        schema->setMaximumHeight(130);
        schema->setPlainText(QString::fromStdString(list.schema.dump(2)));
        layout->addWidget(plainLabel(owner.tr("Available fields and defaults"), dialog));
        layout->addWidget(schema);
        auto *notice = plainLabel({}, dialog);
        layout->addWidget(notice);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, editor, notice, key = list.key, row_id, revision, object_id, scene_id] {
                const auto &snapshot = model.snapshot();
                if (!snapshot || snapshot->scene_id != scene_id ||
                    snapshot->authoring_object_id != object_id || snapshot->scene_revision != revision) {
                    notice->setText(owner.tr("The scene changed. Close this draft and reopen the row."));
                    return;
                }
                Json row;
                try {
                    row = Json::parse(editor->toPlainText().toStdString());
                    if (!row.is_object() || row.contains("$row")) {
                        notice->setText(owner.tr("Enter an object containing row values, without $row."));
                        return;
                    }
                } catch (const Json::exception &error) {
                    notice->setText(QString::fromUtf8(error.what()));
                    return;
                }
                const bool accepted = row_id ? model.editListRow(key, *row_id, row)
                                             : model.insertListRow(key, row);
                if (!accepted) {
                    notice->setText(QString::fromStdString(model.notice().message));
                    return;
                }
                dialog->accept();
                synchronize();
                dispatchRequests();
            });
        dialog->open();
    }

    void addList(QFormLayout &form, QWidget *parent, const InspectorListDescriptor &list) {
        auto *container = new QWidget(parent);
        auto *layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *table = new QTableWidget(static_cast<int>(list.rows.size()), 2, container);
        table->setObjectName(QStringLiteral("pelican.inspectorListRows"));
        table->setHorizontalHeaderLabels({owner.tr("Row ID"), owner.tr("Values")});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setStretchLastSection(true);
        table->setMaximumHeight(180);
        for (std::size_t i = 0; i < list.rows.size(); ++i) {
            auto row = list.rows.at(i);
            const auto id = row.at("$row").at("id").get<std::uint32_t>();
            row.erase("$row");
            table->setItem(static_cast<int>(i), 0, new QTableWidgetItem(QString::number(id)));
            table->setItem(static_cast<int>(i), 1, new QTableWidgetItem(QString::fromStdString(row.dump())));
        }
        layout->addWidget(table);
        auto *buttons = new QHBoxLayout;
        for (const auto &action : {QStringLiteral("Add"), QStringLiteral("Edit"),
                                  QStringLiteral("Remove"), QStringLiteral("Up"), QStringLiteral("Down")}) {
            auto *button = new QPushButton(owner.tr(qPrintable(action)), container);
            button->setObjectName(QStringLiteral("pelican.inspectorList") + action);
            buttons->addWidget(button);
            QObject::connect(button, &QPushButton::clicked, &owner, [this, table, list, action] {
                if (!model.canEditList(list.key)) return;
                if (action == QStringLiteral("Add")) {
                    editListRowDialog(list, std::nullopt, Json::object());
                    return;
                }
                const auto index = table->currentRow();
                if (index < 0) return;
                const auto &row = list.rows.at(index);
                const auto id = row.at("$row").at("id").get<std::uint32_t>();
                if (action == QStringLiteral("Edit")) {
                    editListRowDialog(list, id, row);
                    return;
                }
                if (action == QStringLiteral("Remove")) model.removeListRow(list.key, id);
                else if (action == QStringLiteral("Up") && index > 0) {
                    model.moveListRow(list.key, id, list.rows.at(index - 1).at("$row").at("id").get<std::uint32_t>());
                } else if (action == QStringLiteral("Down") && index + 1 < static_cast<int>(list.rows.size())) {
                    std::optional<std::uint32_t> before;
                    if (index + 2 < static_cast<int>(list.rows.size()))
                        before = list.rows.at(index + 2).at("$row").at("id").get<std::uint32_t>();
                    model.moveListRow(list.key, id, before);
                }
                synchronize();
                dispatchRequests();
            });
        }
        layout->addLayout(buttons);
        container->setEnabled(model.canEditList(list.key));
        list_controls.emplace_back(container, list.key);
        form.addRow(QString::fromStdString(list.path), container);
    }

    void addField(QFormLayout &form, QWidget *parent,
                  const InspectorWidgetDescriptor &descriptor) {
        if (!descriptor.authored) {
            if (descriptor.default_available) {
                auto *button = new QPushButton(owner.tr("Add default"), parent);
                button->setObjectName(QStringLiteral("pelican.inspectorAddDefault"));
                const auto key = descriptor.field_key;
                button->setEnabled(model.canInsertField(key));
                default_buttons.emplace_back(button, key);
                QObject::connect(button, &QPushButton::clicked, &owner, [this, key] {
                    model.insertField(key);
                    synchronize();
                    dispatchRequests();
                });
                form.addRow(fieldLabel(descriptor), button);
            } else {
                form.addRow(fieldLabel(descriptor), plainLabel(owner.tr("Not authored"), parent));
            }
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

void InspectorWidget::setGraphSourceRevealAction(
    std::function<void(const QString &, const QString &)> action) {
    impl_->graph_source_reveal_action = std::move(action);
}

void InspectorWidget::setRayGroupAction(std::function<void(const QString &)> action) {
    impl_->ray_group_action = std::move(action);
}

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
