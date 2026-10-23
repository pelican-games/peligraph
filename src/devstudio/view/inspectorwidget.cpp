#include "inspectorwidget.hpp"

#include "panelhelp.hpp"
#include "graphattachmentwidget.hpp"
#include "materialassignwidget.hpp"
#include "componentaddlist.hpp"

#include "../model/inspectormodel.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
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
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QPointer>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
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

// PM-3c: "1.5, 2, 0" -- a runtime reading, as the numbers it is.
template <std::size_t N>
QString runtimeNumbers(const std::optional<std::array<double, N>> &values) {
    if (!values) return QStringLiteral("-");
    QStringList parts;
    for (const double value : *values) parts << QString::number(value, 'g', 6);
    return parts.join(QStringLiteral(", "));
}

} // namespace

struct InspectorWidget::Impl {
    struct NumericBinding {
        std::string field_key;
        InspectorWidgetKind kind = InspectorWidgetKind::FloatingPointDrag;
        std::vector<QDoubleSpinBox *> editors;
        bool active = false;
        // T-1: three angles in degrees standing for a quaternion field. The
        // value that travels is still the quaternion.
        bool euler = false;
        // UX-10, decision 2: an azimuth and an elevation in degrees standing
        // for a direction vec3. The value that travels is still the vector.
        bool direction = false;
        // UX-10, decision 6: shown = stored x scale (an angle in radians is
        // shown in degrees). The value that travels is the stored one.
        double scale = 1.0;
    };

    struct TextBinding {
        std::string field_key;
        QLineEdit *editor = nullptr;
        bool active = false;
    };

    InspectorWidget &owner;
    EmbeddedViewport &viewport;
    InspectorModel model;
    // PM-3b: the play session, as the window's play controls know it.
    PlaySessionHooks play;
    QPushButton *keep_button = nullptr;
    QPointer<QPushButton> take_from_game;
    GraphAttachmentWidget *graph_attachments = nullptr;
    QHash<qint64, quint64> graph_transport_requests;
    // MT-1s, decision 5: the Material rows of an object that shows a model.
    MaterialAssignWidget *material_assign = nullptr;
    // EB-6s, decision 5: scroll to them once they are shown.
    bool reveal_material_rows = false;
    QLabel *selection_label = nullptr;
    QLabel *notice_label = nullptr;
    // PM-3c: the read-only view of a spawned object, in place of the fields.
    QGroupBox *runtime_box = nullptr;
    QLabel *runtime_position = nullptr;
    QLabel *runtime_rotation = nullptr;
    QLabel *runtime_scale = nullptr;
    QLabel *runtime_world_position = nullptr;
    bool runtime_shown = false;
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
    // RTL-2s, decision 4: Assign hit group... on the Material rows.
    std::function<void(const QString &)> assign_hit_group_action;
    QPushButton *assign_hit_group = nullptr;
    std::function<void(const QString &, const QString &)> graph_source_reveal_action;
    // RSX: the main window's engine restart.
    std::function<void(const QString &)> engine_restart_action;
    // MI-1s, decision 2: Import... beside the model field.
    std::function<void(std::function<void(const std::string &)>)> model_import_action;
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
    // RTL-2s-c: Enable hit groups..., beside a Ray geometry that needs it.
    QPushButton *component_enable_hit_groups = nullptr;
    std::function<void()> enable_hit_groups_action;
    QPushButton *refresh_component_types = nullptr;
    std::optional<Json> displayed_candidates;
    std::string adding_scene;
    std::uint64_t adding_object = 0;
    QPushButton *undo_button = nullptr;
    QPushButton *redo_button = nullptr;
    QAction *undo_action = nullptr;
    QAction *redo_action = nullptr;
    bool scene_history_focus = true;
    QPushButton *cancel_preview_button = nullptr;
    QTimer *pending_timer = nullptr;
    QTimer *watch_timer = nullptr;
    QHash<qint64, quint64> transport_requests;
    // UX-13c: the request the engine has just refused as an unknown method
    // (-32601); its failure, which follows at once, says so to the model.
    qint64 unknown_method_transport = 0;
    // MT-2s: the Material rows' set_object_material requests out, and the one
    // the engine has just refused with the sentence it reads as; its failure,
    // which follows at once, hands the model that sentence.
    QSet<qint64> object_material_transports;
    qint64 refused_object_material_transport = 0;
    QString refused_object_material_sentence;
    std::uint64_t rendered_content_revision =
        std::numeric_limits<std::uint64_t>::max();
    bool rebuilding = false;
    std::function<void()> scene_state_changed;
    // OL-1: told of each spawn, Outliner edit, undo and redo that finished.
    std::function<void(const SceneEditOutcome &)> scene_edit_completed;
    std::uint64_t reported_scene_edit_serial = 0;
    bool reported_scene_dirty = false;
    bool reported_scene_busy = false;
    std::uint64_t reported_save_completion = 0;
    std::vector<std::unique_ptr<NumericBinding>> numeric_bindings;
    std::vector<std::unique_ptr<TextBinding>> text_bindings;
    // T-1: whether a rotation's quaternion fields are unfolded; kept across
    // a rebuild of the panel.
    bool rotation_details_shown = false;
    struct ModelChoice {
        QComboBox *choices = nullptr;
        std::string field_key;
        std::string current;
        // UX-10, decision 5: sprite_view.texture, from the textures.
        bool texture = false;
    };
    // UX-10, decision 2: whether a direction's raw vec3 is unfolded (kept
    // across a rebuild), and the colour swatches with the field they show.
    bool direction_raw_shown = false;
    std::vector<std::pair<QPushButton *, std::string>> color_swatches;
    std::vector<ModelChoice> model_choices;
    // Q-7: what a new snapshot may change without the panel being built
    // again. The last structure drawn, the editors whose value alone is
    // replaced, and the runtime rows whose text alone is.
    std::string rendered_structure;
    struct DirectValue {
        QWidget *editor = nullptr;
        std::string field_key;
        InspectorWidgetKind kind = InspectorWidgetKind::BooleanCheckbox;
    };
    std::vector<DirectValue> direct_values;
    struct RuntimeRow {
        QLabel *label = nullptr;
        std::string component;
        std::size_t component_index = 0;
    };
    std::vector<RuntimeRow> runtime_rows;
    // Q-7: a value typed into a field while another edit was in flight. It
    // is sent, as one edit, once the Inspector is idle.
    std::optional<std::pair<std::string, Json>> deferred_commit;
    std::uint64_t shown_model_assets_revision = 0;
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
        QObject::connect(graph_attachments,
                         &GraphAttachmentWidget::engineRestartRequested, &owner,
                         [this](const QString &reason) {
                             if (engine_restart_action) engine_restart_action(reason);
                         });
        material_assign = new MaterialAssignWidget(
            MaterialAssignWidget::Hooks{
                .send = [this](const QString &method, const QJsonObject &params, QString *error) {
                    return viewport.requestRpc(method, params, error);
                },
                .restart = [this](const QString &reason) {
                    if (engine_restart_action) engine_restart_action(reason);
                },
                // MT-2s, decision 5: a scene edit of this session, so the
                // Undo above takes it back.
                .edit_object = [this](const QJsonObject &params, const QString &description, QString *error) {
                    const auto parsed = Json::parse(
                        QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString(), nullptr, false);
                    const auto outcome = model.submitObjectMaterial(
                        parsed.is_discarded() ? Json::object() : parsed, description.toStdString());
                    if (outcome == SpawnOutcome::Refused && error != nullptr)
                        *error = QString::fromStdString(model.notice().message);
                    synchronize();
                    dispatchRequests();
                    return outcome != SpawnOutcome::Refused;
                },
            },
            &owner);
        layout->addWidget(material_assign);
        // RTL-2s, decision 4: the material picked here, in the Ray layout
        // section's Hit group assignments.
        assign_hit_group = new QPushButton(owner.tr("Assign hit group..."), material_assign);
        assign_hit_group->setObjectName(QStringLiteral("pelican.inspectorAssignHitGroup"));
        assign_hit_group->setToolTip(owner.tr("The hit group the ray tracing tasks answer this material with "
                                              "(Rendering settings > Ray layout)"));
        assign_hit_group->setEnabled(false);
        if (material_assign->layout() != nullptr) material_assign->layout()->addWidget(assign_hit_group);
        QObject::connect(material_assign, &MaterialAssignWidget::rowsPresented, &owner,
                         [this] { synchronizeAssignHitGroup(); });
        QObject::connect(assign_hit_group, &QPushButton::clicked, &owner, [this] {
            if (!assign_hit_group_action) return;
            const auto material = material_assign->assignableMaterial();
            if (material.isEmpty()) return;
            assign_hit_group_action(material);
        });

        notice_label = new QLabel(&owner);
        notice_label->setObjectName(QStringLiteral("pelican.inspectorNotice"));
        notice_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        applyPanelNoticeStyle(notice_label);
        notice_label->hide();
        // PM-3b (12.3): an edit committed while the game runs goes back on
        // Stop; Keep marks it in Play changes so it stays.
        auto *notice_row = new QHBoxLayout;
        notice_row->setContentsMargins(0, 0, 0, 0);
        notice_row->addWidget(notice_label, 1);
        keep_button = new QPushButton(owner.tr("Keep"), &owner);
        keep_button->setObjectName(QStringLiteral("pelican.inspectorKeep"));
        keep_button->setFlat(true);
        keep_button->setCursor(Qt::PointingHandCursor);
        keep_button->setStyleSheet(
            QStringLiteral("QPushButton { color: #4f7cac; text-decoration: underline; "
                           "border: none; padding: 0 4px; }"));
        keep_button->setToolTip(owner.tr("Keep this edit after Stop (marks it in Play changes)"));
        keep_button->hide();
        notice_row->addWidget(keep_button);
        layout->addLayout(notice_row);
        QObject::connect(keep_button, &QPushButton::clicked, &owner, [this] {
            const auto &transaction = model.noticeTransaction();
            if (transaction.empty() || !play.keep) return;
            play.keep(transaction);
            synchronize();
        });

        runtime_box = new QGroupBox(owner.tr("Transform (runtime, read-only)"), &owner);
        runtime_box->setObjectName(QStringLiteral("pelican.inspectorRuntime"));
        auto *runtime_form = new QFormLayout(runtime_box);
        const auto runtime_row = [&](const QString &label, const char *name) {
            QLabel *value = plainLabel({}, runtime_box);
            value->setObjectName(QStringLiteral("pelican.inspectorRuntime.") +
                                 QLatin1String(name));
            runtime_form->addRow(label, value);
            return value;
        };
        runtime_position = runtime_row(owner.tr("Position"), "position");
        runtime_rotation = runtime_row(owner.tr("Rotation (x, y, z, w)"), "rotation");
        runtime_scale = runtime_row(owner.tr("Scale"), "scale");
        runtime_world_position = runtime_row(owner.tr("World position"), "worldPosition");
        // In the fields' place, and taking the room they had: its rows at
        // the top, not floated down the panel.
        runtime_form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        runtime_box->hide();
        layout->addWidget(runtime_box, 1);

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
        // RU-1: Ctrl+Z / Ctrl+Y here are the scene's Undo / Redo, as the
        // buttons are; the render panels have their own history, and a text
        // field keeps the keys for its own text.
        for (auto [button, name, keys] :
             {std::tuple{undo_button, "pelican.inspector.undo",
                         QList<QKeySequence>{QKeySequence::Undo}},
              std::tuple{redo_button, "pelican.inspector.redo",
                         QList<QKeySequence>{QKeySequence{Qt::CTRL | Qt::Key_Y},
                                             QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_Z}}}}) {
            auto *action = new QAction(button->text(), &owner);
            action->setObjectName(QString::fromLatin1(name));
            (button == undo_button ? undo_action : redo_action) = action;
            action->setShortcuts(keys);
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            owner.addAction(action);
            QPushButton *target = button;
            QObject::connect(action, &QAction::triggered, target, [target] {
                if (target->isEnabled()) target->click();
            });
        }
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
            flushDeferredCommit();
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
                             object_material_transports.clear();
                             graph_attachments->setConnected(true);
                             material_assign->engineAvailable();
                             model.startSession();
                             synchronize();
                             dispatchRequests();
                         });
        QObject::connect(
            &viewport, &EmbeddedViewport::engineRpcBecameUnavailable, &owner,
            [this](const QString &message) {
                transport_requests.clear();
                graph_transport_requests.clear();
                object_material_transports.clear();
                graph_attachments->setConnected(false);
                material_assign->engineUnavailable();
                model.stopSession(message.toStdString());
                synchronize();
            });
        QObject::connect(
            &viewport, &EmbeddedViewport::inspectorRpcSucceeded, &owner,
            [this](qint64 transport_request_id,
                   const QByteArray &result_json) {
                if (material_assign->receiveResult(transport_request_id, result_json)) return;
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
                object_material_transports.remove(transport_request_id);
                model.receiveRpcResult(
                    model_request_id,
                    std::string_view{
                        result_json.constData(),
                        static_cast<std::size_t>(result_json.size())});
                synchronize();
                dispatchRequests();
                flushDeferredCommit();
            });
        QObject::connect(
            &viewport, &EmbeddedViewport::inspectorRpcFailed, &owner,
            [this](qint64 transport_request_id, const QString &message) {
                if (material_assign->receiveFailure(transport_request_id, message)) return;
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
                object_material_transports.remove(transport_request_id);
                const bool refused =
                    std::exchange(refused_object_material_transport, 0) == transport_request_id;
                model.receiveRpcFailure(
                    model_request_id,
                    refused ? std::exchange(refused_object_material_sentence, {}).toStdString()
                            : message.toStdString(),
                    std::exchange(unknown_method_transport, 0) == transport_request_id, refused);
                synchronize();
                dispatchRequests();
            });
        QObject::connect(
            &viewport, &EmbeddedViewport::engineRpcRefused, &owner,
            [this](qint64 transport_request_id, int code, const QString &message,
                   const QJsonValue &data) {
                if (material_assign->receiveRefusal(transport_request_id, code, message, data)) return;
                // MT-2s, decision 7: a refused set_object_material is said
                // in the Material rows and, as one sentence, by the Inspector.
                const auto error_code =
                    data.toObject().value(QStringLiteral("error_code")).toString();
                if (object_material_transports.contains(transport_request_id) && !error_code.isEmpty()) {
                    refused_object_material_transport = transport_request_id;
                    refused_object_material_sentence =
                        material_assign->objectEditRefused(error_code, message, data);
                }
                unknown_method_transport =
                    code == -32601 && transport_requests.contains(transport_request_id)
                        ? transport_request_id
                        : 0;
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
            material_assign->engineAvailable();
            model.startSession();
            dispatchRequests();
        }
    }

    void synchronizeAssignHitGroup() {
        assign_hit_group->setEnabled(static_cast<bool>(assign_hit_group_action) &&
                                     !material_assign->assignableMaterial().isEmpty());
    }

    void setSelection(std::optional<OutlinerObjectKey> selection,
                      const QString &display_text) {
        showRuntimeView(false);
        selection_label->setText(display_text);
        model.selectObject(std::move(selection));
        synchronize();
        dispatchRequests();
    }

    // PM-3c. The read-only view takes the fields' place: nothing that edits,
    // adds or attaches is on screen while it is.
    void showRuntimeView(bool shown) {
        if (runtime_shown == shown) return;
        runtime_shown = shown;
        runtime_box->setVisible(shown);
        scroll->setVisible(!shown);
        add_component_button->setVisible(!shown);
        ray_occurrences_button->setVisible(!shown);
        graph_attachments->setVisible(!shown);
        if (shown) material_assign->setSubject({});
    }

    void showRuntimeObject(const RuntimeObject &object) {
        if (!runtime_shown) {
            // No declared object is selected while a spawned one is.
            model.selectObject(std::nullopt);
            showRuntimeView(true);
            synchronize();
            dispatchRequests();
        }
        QString text = owner.tr("%1 (runtime)").arg(QString::fromStdString(object.name));
        if (!object.prefab.empty())
            text += QLatin1Char('\n') + owner.tr("spawned from %1 while the game runs")
                                            .arg(QString::fromStdString(object.prefab));
        selection_label->setText(text);
        const RuntimeObjectTrs none;
        const RuntimeObjectTrs &local = object.local ? *object.local : none;
        runtime_position->setText(runtimeNumbers(local.pos));
        runtime_rotation->setText(runtimeNumbers(local.rotation));
        runtime_scale->setText(runtimeNumbers(local.scale));
        runtime_world_position->setText(runtimeNumbers(object.world ? object.world->pos
                                                                     : none.pos));
    }

    std::optional<GizmoTransformBinding> gizmoTransformBinding() const {
        if (model.refreshBlocked() || model.busy() || !model.snapshot()) {
            return std::nullopt;
        }
        const InspectorObjectSnapshot &snapshot = *model.snapshot();
        GizmoTransformBinding binding{
            .selection = {.scene_id = snapshot.scene_id,
                          .declaration_index = snapshot.declaration_index,
                          .authoring_object_id = snapshot.authoring_object_id},
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
        // A rotation has two bindings: its angles and its quaternion.
        for (const auto &binding : numeric_bindings)
            if (binding->field_key == field_key) setNumericEditors(*binding, value);
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

    SpawnOutcome spawnObject(const std::string &scene_id, Json object,
                             QString *error) {
        const auto outcome = model.spawnObject(scene_id, std::move(object));
        if (outcome == SpawnOutcome::Refused && error != nullptr)
            *error = QString::fromStdString(model.notice().message);
        synchronize();
        dispatchRequests();
        return outcome;
    }

    SpawnOutcome submitSceneEdit(SceneEditKind kind, std::uint64_t subject_id, Json operation,
                                 const std::string &description, QString *error) {
        const auto outcome =
            model.submitSceneEdit(kind, subject_id, std::move(operation), description);
        if (outcome == SpawnOutcome::Refused && error != nullptr)
            *error = QString::fromStdString(model.notice().message);
        synchronize();
        dispatchRequests();
        return outcome;
    }

    const InspectorWidgetDescriptor *descriptorFor(std::string_view field_key) const {
        if (!model.snapshot()) return nullptr;
        for (const auto &component : model.snapshot()->components)
            for (const auto &widget : component.widgets)
                if (widget.field_key == field_key) return &widget;
        return nullptr;
    }

    // Focus as the window keeps it, whether or not the window is the active
    // one: a field keeps its focus while another application is in front.
    static bool holdsFocus(const QWidget *widget) {
        const QWidget *window = widget->window();
        const QWidget *focus = window != nullptr ? window->focusWidget() : nullptr;
        return focus != nullptr && (focus == widget || widget->isAncestorOf(focus));
    }

    static bool holdsFocus(const NumericBinding &binding) {
        return std::any_of(binding.editors.begin(), binding.editors.end(),
                           [](const QDoubleSpinBox *editor) { return holdsFocus(editor); });
    }

    // Text a person is part-way through: what the box shows is not yet what
    // its value reads as ("1." over 1.0000, "-" over 2.0000).
    static bool typingInProgress(const NumericBinding &binding) {
        return std::any_of(binding.editors.begin(), binding.editors.end(),
                           [](const QDoubleSpinBox *editor) {
                               return editor->cleanText() !=
                                      editor->textFromValue(editor->value());
                           });
    }

    bool deferredFor(const std::string &field_key) const {
        return deferred_commit && deferred_commit->first == field_key;
    }

    bool inspectorIdle() const {
        return model.canEdit() && !model.busy() && !model.refreshBlocked();
    }

    // A value typed while the Inspector was busy goes out as soon as it is
    // not. The field it belongs to may have gone (another selection) or may
    // already show it (someone else set the same value); then there is
    // nothing to send.
    void flushDeferredCommit() {
        if (!deferred_commit || !inspectorIdle()) return;
        auto [field_key, value] = std::move(*deferred_commit);
        deferred_commit.reset();
        const auto *descriptor = descriptorFor(field_key);
        if (descriptor == nullptr || descriptor->value == value) return;
        model.commitWidgetValue(field_key, std::move(value));
        synchronize();
        dispatchRequests();
    }

    static std::optional<std::array<double, 4>> quaternionOf(const Json &value) {
        if (!value.is_array() || value.size() != 4) return std::nullopt;
        std::array<double, 4> result{};
        for (std::size_t index = 0; index < 4; ++index) {
            if (!value[index].is_number()) return std::nullopt;
            result[index] = value[index].get<double>();
        }
        return result;
    }

    static std::optional<std::array<double, 3>> vector3Of(const Json &value) {
        if (!value.is_array() || value.size() != 3) return std::nullopt;
        std::array<double, 3> result{};
        for (std::size_t index = 0; index < 3; ++index) {
            if (!value[index].is_number()) return std::nullopt;
            result[index] = value[index].get<double>();
        }
        return result;
    }

    // UX-10: what the azimuth and elevation say, as the vector.
    static Json directionValue(const NumericBinding &binding) {
        const auto vector = directionFromAnglesDegrees(
            {binding.editors[0]->value(), binding.editors[1]->value()});
        return Json::array({vector[0], vector[1], vector[2]});
    }

    // T-1: what the angle fields say, as the quaternion they stand for.
    static Json eulerValue(const NumericBinding &binding) {
        const auto quaternion = quaternionFromEulerDegrees(
            {binding.editors[0]->value(), binding.editors[1]->value(),
             binding.editors[2]->value()});
        return Json::array({quaternion[0], quaternion[1], quaternion[2], quaternion[3]});
    }

    // Whether the fields already show `value`. The angle fields show a
    // quaternion to their own precision, and a quaternion made back from them
    // is that rotation, not those bits: they are compared as shown.
    bool showsValue(const NumericBinding &binding, const Json &value) const {
        // A field shows a value to its own decimals, and what it would send
        // is that rounding: a stored 0.38268343 is what "0.3827" shows, not
        // a change to send when focus leaves it.
        const auto shown = [](const QDoubleSpinBox *editor, const Json &component) {
            return component.is_number() &&
                   editor->textFromValue(editor->value()) ==
                       editor->textFromValue(component.get<double>());
        };
        if (binding.direction) {
            const auto vector = vector3Of(value);
            if (!vector) return false;
            const auto angles = directionAnglesDegrees(*vector);
            const QDoubleSpinBox *azimuth = binding.editors[0];
            const QDoubleSpinBox *elevation = binding.editors[1];
            // Straight up or down has no azimuth: whichever is shown is it.
            const bool vertical = std::abs(std::abs(angles[1]) - 90.0) < 1.0e-9;
            return elevation->textFromValue(elevation->value()) ==
                       elevation->textFromValue(angles[1]) &&
                   (vertical ||
                    azimuth->textFromValue(azimuth->value()) == azimuth->textFromValue(angles[0]));
        }
        if (!binding.euler) {
            if (binding.editors.size() == 1)
                return value.is_number() &&
                       shown(binding.editors.front(), Json(value.get<double>() * binding.scale));
            if (!value.is_array() || value.size() != binding.editors.size()) return false;
            for (std::size_t index = 0; index < value.size(); ++index)
                if (!shown(binding.editors[index], value[index])) return false;
            return true;
        }
        const auto quaternion = quaternionOf(value);
        if (!quaternion) return false;
        const auto degrees = eulerDegreesFromQuaternion(*quaternion);
        for (std::size_t index = 0; index < 3; ++index) {
            const QDoubleSpinBox *editor = binding.editors[index];
            if (editor->textFromValue(editor->value()) !=
                editor->textFromValue(degrees[index]))
                return false;
        }
        return true;
    }

    void setNumericEditors(NumericBinding &binding, const Json &value) {
        if (binding.direction) {
            const auto vector = vector3Of(value);
            if (!vector || showsValue(binding, value)) return;
            const auto angles = directionAnglesDegrees(*vector);
            const bool vertical = std::abs(std::abs(angles[1]) - 90.0) < 1.0e-9;
            for (std::size_t index = vertical ? 1 : 0; index < 2; ++index) {
                const QSignalBlocker blocker{binding.editors[index]};
                binding.editors[index]->setValue(angles[index]);
            }
            return;
        }
        if (binding.euler) {
            const auto quaternion = quaternionOf(value);
            if (!quaternion || showsValue(binding, value)) return;
            const auto degrees = eulerDegreesFromQuaternion(*quaternion);
            for (std::size_t index = 0; index < 3; ++index) {
                const QSignalBlocker blocker{binding.editors[index]};
                binding.editors[index]->setValue(degrees[index]);
            }
            return;
        }
        if (binding.editors.size() == 1 && value.is_number()) {
            const QSignalBlocker blocker{binding.editors.front()};
            binding.editors.front()->setValue(value.get<double>() * binding.scale);
        } else if (value.is_array() && value.size() == binding.editors.size()) {
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (!value[index].is_number()) return;
                const QSignalBlocker blocker{binding.editors[index]};
                binding.editors[index]->setValue(value[index].get<double>());
            }
        }
    }

    // Everything the panel draws except the values in its editable fields
    // and the runtime rows' text. Equal before and after a refresh, the panel
    // keeps its widgets -- and with them focus and anything half typed -- and
    // only the values move.
    std::string structureSignature() const {
        if (!model.selection()) return "none";
        const auto &snapshot = model.snapshot();
        if (!snapshot) return "loading";
        Json structure{{"scene_id", snapshot->scene_id},
                       {"object", snapshot->authoring_object_id},
                       {"declaration", snapshot->declaration_index},
                       {"prefab", snapshot->prefab_instance ? *snapshot->prefab_instance : Json{}}};
        Json &parameters = structure["parameters"] = Json::array();
        for (const auto &parameter : snapshot->prefab_parameters)
            parameters.push_back({parameter.name, parameter.value_resolved, parameter.source});
        Json &components = structure["components"] = Json::array();
        for (const auto &component : snapshot->components) {
            Json entry{{"name", component.name},
                       {"index", component.component_index},
                       {"title", component.title},
                       {"editable", component.editable},
                       {"pending", component.pending},
                       {"schema", component.schema_available},
                       {"read_only", component.read_only_reason},
                       {"generated", component.generated},
                       {"source", component.generated_source},
                       {"runtime_shown", component.runtime_json.has_value() &&
                                             *component.runtime_json != component.authored_json}};
            if (component.widgets.empty() && component.lists.empty())
                entry["authored"] = component.authored_json;
            Json &widgets = entry["widgets"] = Json::array();
            for (const auto &widget : component.widgets) {
                Json field{{"key", widget.field_key},
                           {"kind", static_cast<int>(widget.kind)},
                           {"columns", widget.columns},
                           {"authored", widget.authored},
                           {"matches", widget.value_matches_schema},
                           {"default", widget.default_available},
                           {"unit", widget.unit},
                           {"enum", widget.enum_values},
                           {"min", widget.range_min ? Json(*widget.range_min) : Json{}},
                           {"max", widget.range_max ? Json(*widget.range_max) : Json{}},
                           // UX-10, decision 6: a controller.type picked
                           // shows other fields.
                           {"shown", inspectorFieldShown(widget, component.widgets)},
                           {"missing", inspectorFieldRequiredMissing(widget, component.widgets)}};
                // A field that is not an editor shows its state, not a value
                // that can be replaced in place.
                if (!widget.authored || !widget.value_matches_schema) field["value"] = widget.value;
                widgets.push_back(std::move(field));
            }
            Json &lists = entry["lists"] = Json::array();
            for (const auto &list : component.lists)
                lists.push_back({list.key, list.path, list.schema, list.rows});
            components.push_back(std::move(entry));
        }
        return structure.dump();
    }

    // Q-7: the values of a snapshot whose structure is the one on screen.
    // A field being edited, holding a value that waits to be sent, or with
    // text half typed keeps what it shows.
    void applyValues() {
        rebuilding = true;
        for (const auto &binding : numeric_bindings) {
            if (binding->active || deferredFor(binding->field_key) ||
                typingInProgress(*binding))
                continue;
            if (const auto *descriptor = descriptorFor(binding->field_key))
                setNumericEditors(*binding, descriptor->value);
        }
        for (const auto &binding : text_bindings) {
            if (binding->active || deferredFor(binding->field_key) ||
                binding->editor->isModified())
                continue;
            const auto *descriptor = descriptorFor(binding->field_key);
            if (descriptor == nullptr || !descriptor->value.is_string()) continue;
            const QString text = QString::fromStdString(descriptor->value.get<std::string>());
            if (binding->editor->text() == text) continue;
            const QSignalBlocker blocker{binding->editor};
            binding->editor->setText(text);
        }
        for (const auto &direct : direct_values) {
            const auto *descriptor = descriptorFor(direct.field_key);
            if (descriptor == nullptr) continue;
            const QSignalBlocker blocker{direct.editor};
            if (auto *box = qobject_cast<QCheckBox *>(direct.editor);
                box != nullptr && descriptor->value.is_boolean()) {
                box->setChecked(descriptor->value.get<bool>());
            } else if (auto *combo = qobject_cast<QComboBox *>(direct.editor);
                       combo != nullptr && descriptor->value.is_string()) {
                combo->setCurrentText(QString::fromStdString(descriptor->value.get<std::string>()));
            }
        }
        for (auto &choice : model_choices) {
            const auto *descriptor = descriptorFor(choice.field_key);
            if (descriptor == nullptr || !descriptor->value.is_string()) continue;
            choice.current = descriptor->value.get<std::string>();
            fillChoice(choice);
        }
        for (const auto &[swatch, key] : color_swatches)
            if (const auto *descriptor = descriptorFor(key)) showSwatch(*swatch, descriptor->value);
        if (const auto &snapshot = model.snapshot()) {
            for (const auto &row : runtime_rows) {
                for (const auto &component : snapshot->components) {
                    if (component.name != row.component ||
                        component.component_index != row.component_index ||
                        !component.runtime_json)
                        continue;
                    row.label->setText(QString::fromStdString(component.runtime_json->dump(2)));
                }
            }
        }
        rebuilding = false;
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
                if (request.method == "set_object_material")
                    object_material_transports.insert(transport_request_id);
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
        // RTL-2s-c (RTL-2s decision 5): a Ray geometry needs a ray layout;
        // the way to one is the Ray layout section's Enable hit groups.
        component_enable_hit_groups = new QPushButton(owner.tr("Enable hit groups..."), dialog);
        component_enable_hit_groups->setObjectName(QStringLiteral("pelican.componentAddEnableHitGroups"));
        component_enable_hit_groups->setToolTip(
            owner.tr("Open the Ray layout section (Rendering settings), where hit groups are enabled"));
        component_enable_hit_groups->hide();
        layout->addWidget(component_enable_hit_groups, 0, Qt::AlignLeft);
        QObject::connect(component_enable_hit_groups, &QPushButton::clicked, dialog, [this, dialog] {
            if (!enable_hit_groups_action) return;
            dialog->close();
            enable_hit_groups_action();
        });
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
            // G-2: built-in shapes read "Cube (built-in)" and come first,
            // above a separator. For a simplemodelview Studio lists the three
            // itself when the engine's catalogue does not (before GP-5b), and
            // once each when it does.
            std::vector<Json> choices(selection->at("choices").begin(),
                                      selection->at("choices").end());
            if (candidate.value("name", std::string{}) == "simplemodelview") {
                std::vector<Json> fallback;
                for (const auto &reference : enginePrimitiveModelReferences()) {
                    if (std::none_of(choices.begin(), choices.end(), [&](const Json &choice) {
                            return choice.value("id", std::string{}) == reference;
                        }))
                        fallback.push_back({{"id", reference}, {"path", reference},
                                            {"status", "builtin"}});
                }
                choices.insert(choices.begin(), fallback.begin(), fallback.end());
            }
            const auto builtin = [](const Json &choice) {
                return choice.value("status", std::string{}) == "builtin" ||
                       !builtinModelLabel(choice.value("id", std::string{})).empty();
            };
            std::stable_partition(choices.begin(), choices.end(), builtin);
            bool separated = false;
            for (const auto &choice : choices) {
                const auto id_text = choice.at("id").get<std::string>();
                const auto id = QString::fromStdString(id_text);
                const auto path = QString::fromStdString(choice.at("path").get<std::string>());
                const auto status = QString::fromStdString(choice.at("status").get<std::string>());
                if (!builtin(choice) && !separated && component_assets->count() > 0) {
                    auto *separator = new QListWidgetItem(QStringLiteral("────────"), component_assets);
                    separator->setFlags(Qt::NoItemFlags);
                    separated = true;
                }
                QString label = QStringLiteral("%1 [%2]").arg(id, status);
                if (builtin(choice)) {
                    const auto named = builtinModelLabel(id_text);
                    label = named.empty() ? owner.tr("%1 (built-in)").arg(id)
                                          : QString::fromStdString(named);
                }
                auto *item = new QListWidgetItem(label, component_assets);
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
                // Q-6: a candidate this build cannot take is listed after the
                // rest, greyed, with a short word after its name.
                fillComponentAddTypes(*component_types, componentAddRows(*catalog));
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
        const auto row = currentComponentAddRow(*component_types);
        if (!row) {
            component_description->setText(owner.tr("No additional registered component types."));
            return;
        }
        const auto &candidate = catalog->at("candidates").at(row->candidate);
        synchronizeComponentAssets(candidate, ready && row->addable);
        component_enable_hit_groups->setVisible(!row->addable && row->key == "pelican_ray_instance" &&
                                                static_cast<bool>(enable_hit_groups_action));
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
        applyComponentAddAvailability(*component_types, ready, *confirm_add_component,
                                      *component_declaration);
        component_declaration->setToolTip(owner.tr("Edit the declaration, then Add. Use registered asset names for asset fields; the engine validates the values."));
        // Q-6: why a row cannot be added comes first, since the panel shows
        // only the first line: "This build has no physics module
        // (physics.module_unavailable@1)".
        QStringList lines;
        if (!row->addable) lines.push_back(QString::fromStdString(row->blocked));
        if (!fields.isEmpty()) lines.push_back((candidate.contains("behavior_type")
            ? owner.tr("Params: %1") : owner.tr("Fields: %1")).arg(fields.join(", ")));
        if (row->addable && !candidate.value("available", false))
            lines.push_back(QString::fromStdString(candidate.value("unavailable_reason", "")));
        QString description = lines.join("\n");
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
            "Select geometry to inspect its settings. Omitted fields inherit; an explicit zero overrides."));
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
                    const auto priority = std::string_view{field} == "geometry_flags"
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

    // MT-1s, decision 5: the model the selected object shows (its
    // simplemodelview's), and the object's name.
    std::optional<std::pair<std::string, std::string>> materialSubject() const {
        const auto &snapshot = model.snapshot();
        if (!snapshot || runtime_shown) return std::nullopt;
        for (const auto &component : snapshot->components) {
            if (component.name != "simplemodelview" || !component.authored_json.is_object()) continue;
            const auto found = component.authored_json.find("model");
            if (found == component.authored_json.end() || !found->is_string() ||
                found->get_ref<const std::string &>().empty())
                continue;
            return std::pair{found->get<std::string>(), snapshot->name.value_or(std::string{})};
        }
        return std::nullopt;
    }

    void synchronize() {
        const bool scene_dirty = model.hasUnsavedChanges();
        const bool scene_busy = model.busy() || model.refreshBlocked();
        const auto save_completion = model.sceneSaveCompletion();
        if (scene_dirty != reported_scene_dirty || scene_busy != reported_scene_busy ||
            save_completion != reported_save_completion) {
            reported_scene_dirty = scene_dirty;
            reported_scene_busy = scene_busy;
            reported_save_completion = save_completion;
            if (scene_state_changed) scene_state_changed();
        }
        if (const auto &edit = model.lastSceneEdit();
            edit && edit->serial != reported_scene_edit_serial) {
            reported_scene_edit_serial = edit->serial;
            // After this panel has finished with the reply: what the window
            // does with it (a new selection) comes back into this panel.
            if (scene_edit_completed)
                QTimer::singleShot(0, &owner, [this, outcome = *edit] {
                    if (scene_edit_completed) scene_edit_completed(outcome);
                });
        }
        const auto &snapshot = model.snapshot();
        graph_attachments->setOwner(snapshot && snapshot->authoring_object_id != 0
            ? std::optional<GraphAttachmentOwner>{{snapshot->scene_id, snapshot->authoring_object_id}}
            : std::nullopt);
        ray_occurrences_button->setEnabled(model.snapshot().has_value() && !model.busy() && !model.refreshBlocked());
        {
            // MT-2s: the object itself and the revision it was read at, so
            // the rows read its materials again after any edit or Undo.
            const auto subject = materialSubject();
            MaterialAssignSession::Subject material_subject;
            if (subject && snapshot) {
                material_subject = {.model = subject->first,
                                    .name = subject->second,
                                    .object = snapshot->authoring_object_id,
                                    .scene_id = snapshot->scene_id,
                                    .revision = snapshot->scene_revision};
            }
            material_assign->setSubject(material_subject);
            material_assign->setSceneBusy(model.busy() || model.refreshBlocked());
            // EB-6s, decision 5: asked to show them before they were.
            if (reveal_material_rows && !material_subject.model.empty()) {
                reveal_material_rows = false;
                scroll->ensureWidgetVisible(material_assign);
            }
        }
        synchronizeRayDialog();
        add_component_button->setEnabled(model.canRequestComponentAddCandidates());
        synchronizeComponentAddDialog();
        // PM-3b: what this session committed, told to the play controls so
        // Play changes can name it.
        for (auto &edit : model.takeCommittedEdits())
            if (play.committed) play.committed(edit);
        // N-7 rule 2: one line, hidden when there is nothing to act on; the
        // whole message stays as the row's tooltip.
        const InspectorNotice &notice = model.notice();
        const bool in_session = play.session && play.session();
        const std::string &notice_transaction = model.noticeTransaction();
        const bool session_edit = in_session && notice.kind == InspectorNoticeKind::Success &&
                                  !notice_transaction.empty();
        const bool kept_edit = session_edit && play.kept && play.kept(notice_transaction);
        const QString notice_text = QString::fromStdString(notice.message);
        // After the one line, so it is never the part cut off.
        const QString stop_note = !session_edit ? QString{}
                                  : kept_edit   ? owner.tr(" — kept after Stop")
                                                : owner.tr(" — reverts on Stop");
        setPanelNotice(notice_label, oneLine(notice_text) + stop_note, notice_text + stop_note);
        keep_button->setVisible(session_edit && !kept_edit);
        if (take_from_game) {
            // PM-3c: never for a spawned object's read-only view.
            const bool offered = in_session && !runtime_shown;
            take_from_game->setVisible(offered);
            take_from_game->setEnabled(offered && model.canTakeTransformFromGame());
        }
        const QString notice_style = notice.kind == InspectorNoticeKind::Error
            ? QStringLiteral("color: #d94c3d;")
            : notice.kind == InspectorNoticeKind::Success
                ? QStringLiteral("color: #388e3c;")
                : notice.kind == InspectorNoticeKind::Information
                    ? QStringLiteral("color: #4f7cac;") : QString{};
        // Qt repolishes even an identical stylesheet. The 40 ms pending poll
        // must not invalidate the dock layout and repaint the whole window.
        if (notice_label->styleSheet() != notice_style)
            notice_label->setStyleSheet(notice_style);

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
        const auto undo_label = QString::fromStdString(model.undoLabel());
        const auto redo_label = QString::fromStdString(model.redoLabel());
        undo_action->setText(undo_label.isEmpty() ? owner.tr("Undo") : owner.tr("Undo %1").arg(undo_label));
        redo_action->setText(redo_label.isEmpty() ? owner.tr("Redo") : owner.tr("Redo %1").arg(redo_label));
        undo_button->setToolTip(undo_action->text());
        redo_button->setToolTip(redo_action->text());
        undo_action->setEnabled(model.canUndoRedo() && scene_history_focus);
        redo_action->setEnabled(model.canUndoRedo() && scene_history_focus);
        cancel_preview_button->setEnabled(model.busy() &&
                                          model.refreshBlocked());

        const bool field_interaction_locked =
            model.refreshBlocked() || model.busy();
        for (QWidget *editor : direct_editors) {
            editor->setEnabled(!field_interaction_locked);
        }
        // Q-7: the field that has focus stays enabled while the Inspector is
        // busy. Disabling it made Qt hand focus to the next field, and that
        // focus used to begin an edit which nothing ever finished.
        for (const auto &binding : numeric_bindings) {
            const bool keep = binding->active || holdsFocus(*binding);
            for (QDoubleSpinBox *editor : binding->editors) {
                editor->setEnabled(!field_interaction_locked || keep);
            }
        }
        for (const auto &binding : text_bindings) {
            binding->editor->setEnabled(!field_interaction_locked ||
                                        binding->active ||
                                        holdsFocus(binding->editor));
        }

        if (rendered_content_revision != model.contentRevision()) {
            // Q-7: a commit or a refresh that changes only values replaces
            // the values; the panel is built again only when what it draws
            // changes (another object, a component added or removed, ...).
            if (model.snapshot() && structureSignature() == rendered_structure) {
                rendered_content_revision = model.contentRevision();
                applyValues();
            } else {
                rebuildProperties();
            }
        }
        if (shown_model_assets_revision != model.modelAssetsRevision()) {
            shown_model_assets_revision = model.modelAssetsRevision();
            for (const auto &entry : model_choices) fillChoice(entry);
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
        model_choices.clear();
        color_swatches.clear();
        direct_values.clear();
        runtime_rows.clear();
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
        rendered_structure = structureSignature();
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
        if (component.name == "transform" && !component.generated) {
            // PM-3b (12.3): while the game runs, the values the game gave
            // the object can be written into the scene and kept after Stop.
            auto *take = new QPushButton(owner.tr("Take from game"), group);
            take->setObjectName(QStringLiteral("pelican.inspectorTakeFromGame"));
            take->setToolTip(owner.tr(
                "Write the position, rotation and scale the game has given this object now "
                "into the scene, and keep that edit after Stop"));
            const bool in_session = play.session && play.session();
            take->setVisible(in_session);
            take->setEnabled(in_session && model.canTakeTransformFromGame());
            QObject::connect(take, &QPushButton::clicked, &owner, [this] {
                model.takeTransformFromGame();
                synchronize();
                dispatchRequests();
            });
            take_from_game = take;
            form->addRow(take);
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
            // UX-10, decision 6: only the fields the discriminant's value
            // reads; one it requires and that is not given is marked.
            if (!inspectorFieldShown(descriptor, component.widgets)) continue;
            addField(*form, group, descriptor);
            if (inspectorFieldRequiredMissing(descriptor, component.widgets) && form->rowCount() > 0)
                if (auto *item = form->itemAt(form->rowCount() - 1, QFormLayout::LabelRole))
                    if (auto *label = qobject_cast<QLabel *>(item->widget())) {
                        label->setStyleSheet(QStringLiteral("color: #d94c3d;"));
                        label->setToolTip(owner.tr("Required for this %1")
                                              .arg(QString::fromStdString(descriptor.discriminant)));
                        label->setObjectName(QStringLiteral("pelican.inspectorRequired.%1.%2")
                                                 .arg(QString::fromStdString(descriptor.component_slot),
                                                      QString::fromStdString(descriptor.field_name)));
                    }
        }
        for (const auto &list : component.lists) {
            // MT-2s, decision 6: a list the Material rows choose is not typed
            // here as well.
            if (!inspectorListShown(component.name, list.path)) {
                auto *note = plainLabel(owner.tr("Chosen in the Material rows above"), group);
                note->setObjectName(QStringLiteral("pelican.inspectorListElsewhere"));
                form->addRow(QString::fromStdString(list.path), note);
                continue;
            }
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
            runtime_rows.push_back({runtime, component.name, component.component_index});
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
            direct_values.push_back({editor, descriptor.field_key, descriptor.kind});
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
            direct_values.push_back({editor, descriptor.field_key, descriptor.kind});
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
            if (isModelReferenceField(descriptor)) {
                form.addRow(fieldLabel(descriptor),
                            makeModelChoice(parent, descriptor));
                break;
            }
            if (isTextureReferenceField(descriptor)) {
                form.addRow(fieldLabel(descriptor), makeModelChoice(parent, descriptor, true));
                break;
            }
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
                    // Q-7: typed while another edit is in flight, the text
                    // waits in the field; finishing it sends it.
                    if (!binding_pointer->active && !inspectorIdle()) {
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

    // G-2: a model is picked, not spelled. The built-in shapes first, a
    // separator, then the project's models as list_assets reports them; the
    // authored value is kept in the list whatever it is. A pick commits at
    // once, like the enum combo; the automation `select` and a popup pick
    // both arrive as a current-index change.
    void fillModelChoice(QComboBox &choices, const std::string &current) {
        const QSignalBlocker blocker{&choices};
        choices.clear();
        const auto &assets = model.modelAssets();
        bool separated = false;
        for (const auto &choice : modelReferenceChoices(
                 current, assets ? *assets : std::vector<std::string>{})) {
            if (!choice.builtin && !separated) {
                choices.insertSeparator(choices.count());
                separated = true;
            }
            choices.addItem(QString::fromStdString(choice.label),
                            QString::fromStdString(choice.value));
        }
        choices.setCurrentIndex(
            choices.findData(QString::fromStdString(current)));
    }

    // UX-10, decision 5: a texture is picked the same way, from the textures
    // list_assets reports; the authored value (an atlas frame) stays listed.
    void fillTextureChoice(QComboBox &choices, const std::string &current) {
        const QSignalBlocker blocker{&choices};
        choices.clear();
        const auto &assets = model.textureAssets();
        for (const auto &choice : textureReferenceChoices(
                 current, assets ? *assets : std::vector<std::string>{}))
            choices.addItem(QString::fromStdString(choice), QString::fromStdString(choice));
        choices.setCurrentIndex(choices.findData(QString::fromStdString(current)));
    }

    void fillChoice(const ModelChoice &choice) {
        if (choice.texture)
            fillTextureChoice(*choice.choices, choice.current);
        else
            fillModelChoice(*choice.choices, choice.current);
    }

    QWidget *makeModelChoice(QWidget *parent,
                             const InspectorWidgetDescriptor &descriptor,
                             bool texture = false) {
        auto *choices = new QComboBox(parent);
        choices->setObjectName(texture ? QStringLiteral("pelican.inspectorTextureChoice")
                                       : QStringLiteral("pelican.inspectorModelChoice"));
        const auto current = descriptor.value.get<std::string>();
        if (texture)
            fillTextureChoice(*choices, current);
        else
            fillModelChoice(*choices, current);
        direct_editors.push_back(choices);
        model_choices.push_back({choices, descriptor.field_key, current, texture});
        QObject::connect(
            choices, &QComboBox::currentIndexChanged, &owner,
            [this, choices, key = descriptor.field_key](int index) {
                if (rebuilding || index < 0) return;
                const auto value = choices->itemData(index).toString();
                if (value.isEmpty()) return;
                model.commitWidgetValue(key, value.toStdString());
                synchronize();
                dispatchRequests();
            });
        if (texture) return choices;
        // MI-1s, decision 2: a model file from outside, registered, and the
        // field set to the name it was registered as.
        auto *row = new QWidget(parent);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(4);
        choices->setParent(row);
        row_layout->addWidget(choices, 1);
        auto *import = new QPushButton(owner.tr("Import\xE2\x80\xA6"), row);
        import->setObjectName(QStringLiteral("pelican.inspectorModelImport"));
        import->setToolTip(owner.tr("Choose a .glb, .gltf or .vrm file: it is copied into "
                                    "assets/models (unless it is in the project already), "
                                    "registered as a model, and this field set to it"));
        import->setVisible(static_cast<bool>(model_import_action));
        direct_editors.push_back(import);
        row_layout->addWidget(import);
        QObject::connect(import, &QPushButton::clicked, &owner, [this, key = descriptor.field_key] {
            if (!model_import_action) return;
            const QPointer<QWidget> alive{&owner};
            model_import_action([this, alive, key](const std::string &name) {
                if (!alive) return;
                model.refreshModelAssets();
                model.commitWidgetValue(key, name);
                synchronize();
                dispatchRequests();
            });
        });
        return row;
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

    // G-5. One name per field, `<kind>.<component>.<field>`, so a scenario
    // reaches the scale of a transform rather than whichever number box came
    // first (every vector's first column used to share one name).
    static QString fieldObjectName(const char *kind,
                                   const InspectorWidgetDescriptor &descriptor) {
        return QStringLiteral("%1.%2.%3")
            .arg(QLatin1String(kind),
                 QString::fromStdString(descriptor.component_slot),
                 QString::fromStdString(descriptor.field_name));
    }

    QWidget *makeNumericEditor(QWidget *parent,
                               const InspectorWidgetDescriptor &descriptor) {
        auto binding = std::make_unique<NumericBinding>();
        binding->field_key = descriptor.field_key;
        binding->kind = descriptor.kind;
        auto *editor = new QDoubleSpinBox(parent);
        editor->setObjectName(fieldObjectName("pelican.inspectorNumeric", descriptor));
        configureNumeric(*editor, descriptor, false);
        // UX-10, decision 6: an angle stored in radians is shown in degrees.
        binding->scale = inspectorDisplayScale(descriptor);
        if (binding->scale != 1.0) {
            editor->setRange(editor->minimum() * binding->scale, editor->maximum() * binding->scale);
            editor->setDecimals(2);
            editor->setSingleStep(1.0);
            editor->setSuffix(QString{QChar{0x00B0}});
            // UX-14, decision 3: a camera's yaw and pitch say their basis.
            if (const auto tip = inspectorAngleTip(descriptor); !tip.empty())
                editor->setToolTip(QString::fromStdString(tip));
        }
        editor->setValue(descriptor.value.get<double>() * binding->scale);
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

    void connectNumericBinding(NumericBinding *binding_pointer) {
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
    }

    // T-1: a rotation is typed as three angles. The quaternion's own four
    // fields stay, folded under "Quaternion", for a value the angles cannot
    // say exactly or a person who wants the numbers.
    QWidget *makeRotationEditor(QWidget *parent,
                                const InspectorWidgetDescriptor &descriptor) {
        auto *container = new QWidget(parent);
        auto *column = new QVBoxLayout(container);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(2);
        auto *angles = new QWidget(container);
        auto *row = new QHBoxLayout(angles);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(3);
        auto binding = std::make_unique<NumericBinding>();
        binding->field_key = descriptor.field_key;
        binding->kind = descriptor.kind;
        binding->euler = true;
        const auto quaternion = quaternionOf(descriptor.value);
        const auto degrees = quaternion ? eulerDegreesFromQuaternion(*quaternion)
                                        : std::array<double, 3>{};
        static constexpr std::array axes{"X", "Y", "Z"};
        for (std::size_t index = 0; index < 3; ++index) {
            auto *editor = new QDoubleSpinBox(angles);
            editor->setObjectName(
                fieldObjectName("pelican.inspectorEuler", descriptor) +
                QStringLiteral(".%1").arg(index));
            editor->setRange(-360.0, 360.0);
            editor->setDecimals(2);
            editor->setSingleStep(1.0);
            editor->setAccelerated(true);
            editor->setKeyboardTracking(true);
            editor->setSuffix(QString{QChar{0x00B0}});
            editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            editor->setToolTip(owner.tr("Rotation about %1, in degrees (applied X, then Y, then Z)")
                                   .arg(QLatin1String{axes[index]}));
            editor->setValue(degrees[index]);
            binding->editors.push_back(editor);
            row->addWidget(editor, 1);
        }
        NumericBinding *binding_pointer = binding.get();
        numeric_bindings.push_back(std::move(binding));
        connectNumericBinding(binding_pointer);

        auto *details = new QPushButton(owner.tr("Quaternion"), container);
        details->setObjectName(
            fieldObjectName("pelican.inspectorRotationDetails", descriptor));
        details->setCheckable(true);
        details->setFlat(true);
        details->setToolTip(owner.tr("Show the stored quaternion x, y, z, w"));
        row->addWidget(details);
        column->addWidget(angles);
        QWidget *raw = makeVectorEditor(container, descriptor, false);
        raw->setVisible(rotation_details_shown);
        details->setChecked(rotation_details_shown);
        column->addWidget(raw);
        QObject::connect(details, &QPushButton::toggled, &owner,
                         [this, raw](bool shown) {
                             rotation_details_shown = shown;
                             raw->setVisible(shown);
                         });
        return container;
    }

    // UX-10, decision 2: a direction is typed as an azimuth and an
    // elevation. Its vec3 stays, folded under "Raw", for a value the angles
    // cannot say exactly or a person who wants the numbers.
    QWidget *makeDirectionEditor(QWidget *parent, const InspectorWidgetDescriptor &descriptor) {
        auto *container = new QWidget(parent);
        auto *column = new QVBoxLayout(container);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(2);
        auto *angles = new QWidget(container);
        auto *row = new QHBoxLayout(angles);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(3);
        auto binding = std::make_unique<NumericBinding>();
        binding->field_key = descriptor.field_key;
        binding->kind = descriptor.kind;
        binding->direction = true;
        const auto vector = vector3Of(descriptor.value);
        const auto degrees = vector ? directionAnglesDegrees(*vector) : std::array<double, 2>{};
        struct Angle {
            const char *name;
            double low;
            double high;
            QString tip;
        };
        const std::array<Angle, 2> fields{
            Angle{"azimuth", -180.0, 180.0, QString::fromStdString(inspectorDirectionAngleTip(0))},
            Angle{"elevation", -90.0, 90.0, QString::fromStdString(inspectorDirectionAngleTip(1))}};
        for (std::size_t index = 0; index < 2; ++index) {
            auto *editor = new QDoubleSpinBox(angles);
            editor->setObjectName(fieldObjectName("pelican.inspectorDirection", descriptor) +
                                  QStringLiteral(".") + QLatin1String{fields[index].name});
            editor->setRange(fields[index].low, fields[index].high);
            editor->setWrapping(index == 0);
            editor->setDecimals(2);
            editor->setSingleStep(1.0);
            editor->setAccelerated(true);
            editor->setKeyboardTracking(true);
            editor->setSuffix(QString{QChar{0x00B0}});
            editor->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            editor->setToolTip(fields[index].tip);
            editor->setValue(degrees[index]);
            binding->editors.push_back(editor);
            row->addWidget(editor, 1);
        }
        NumericBinding *binding_pointer = binding.get();
        numeric_bindings.push_back(std::move(binding));
        connectNumericBinding(binding_pointer);
        auto *raw_button = new QPushButton(owner.tr("Raw"), container);
        raw_button->setObjectName(fieldObjectName("pelican.inspectorDirectionRaw", descriptor));
        raw_button->setCheckable(true);
        raw_button->setFlat(true);
        raw_button->setToolTip(owner.tr("Show the stored vector x, y, z"));
        row->addWidget(raw_button);
        column->addWidget(angles);
        QWidget *raw = makeVectorEditor(container, descriptor, false);
        raw->setVisible(direction_raw_shown);
        raw_button->setChecked(direction_raw_shown);
        column->addWidget(raw);
        QObject::connect(raw_button, &QPushButton::toggled, &owner, [this, raw](bool shown) {
            direction_raw_shown = shown;
            raw->setVisible(shown);
        });
        return container;
    }

    static void showSwatch(QPushButton &swatch, const Json &value) {
        const auto channel = [&](std::size_t index, double fallback) {
            return value.is_array() && index < value.size() && value[index].is_number()
                       ? std::clamp(value[index].get<double>(), 0.0, 1.0)
                       : fallback;
        };
        const auto colour = QColor::fromRgbF(static_cast<float>(channel(0, 1.0)),
                                             static_cast<float>(channel(1, 1.0)),
                                             static_cast<float>(channel(2, 1.0)));
        swatch.setStyleSheet(
            QStringLiteral("background-color: %1; border: 1px solid #808080;").arg(colour.name()));
    }

    // UX-10, decision 2: a colour is a swatch that opens the colour picker,
    // beside its numbers. The picked components are stored as picked
    // (linear); an HDR light keeps its strength in intensity.
    QWidget *makeColorEditor(QWidget *parent, const InspectorWidgetDescriptor &descriptor) {
        auto *container = new QWidget(parent);
        auto *row = new QHBoxLayout(container);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(3);
        auto *swatch = new QPushButton(container);
        swatch->setObjectName(fieldObjectName("pelican.inspectorColor", descriptor));
        swatch->setFixedSize(36, 20);
        swatch->setToolTip(owner.tr("Pick a colour"));
        showSwatch(*swatch, descriptor.value);
        row->addWidget(swatch);
        row->addWidget(makeVectorEditor(container, descriptor, false), 1);
        color_swatches.emplace_back(swatch, descriptor.field_key);
        QObject::connect(swatch, &QPushButton::clicked, &owner,
                         [this, key = descriptor.field_key] { pickColor(key); });
        return container;
    }

    void pickColor(const std::string &field_key) {
        const auto *descriptor = descriptorFor(field_key);
        if (descriptor == nullptr || !descriptor->value.is_array()) return;
        const Json before = descriptor->value;
        const auto channel = [&](std::size_t index) {
            return index < before.size() && before[index].is_number()
                       ? std::clamp(before[index].get<double>(), 0.0, 1.0)
                       : 1.0;
        };
        const bool alpha = before.size() > 3;
        const auto picked = QColorDialog::getColor(
            QColor::fromRgbF(static_cast<float>(channel(0)), static_cast<float>(channel(1)),
                             static_cast<float>(channel(2)), static_cast<float>(channel(3))),
            &owner, QString::fromStdString(descriptor->component_slot + "." + descriptor->field_name),
            alpha ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions{});
        if (!picked.isValid()) return;
        Json value = Json::array({picked.redF(), picked.greenF(), picked.blueF()});
        if (alpha) value.push_back(picked.alphaF());
        commitHeldValue(field_key, std::move(value));
    }

    QWidget *makeVectorEditor(QWidget *parent,
                              const InspectorWidgetDescriptor &descriptor,
                              bool angles_for_rotation = true) {
        if (angles_for_rotation && isEulerRotationField(descriptor))
            return makeRotationEditor(parent, descriptor);
        if (angles_for_rotation && isDirectionField(descriptor))
            return makeDirectionEditor(parent, descriptor);
        if (angles_for_rotation && isColorField(descriptor))
            return makeColorEditor(parent, descriptor);
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
                fieldObjectName("pelican.inspectorVector", descriptor) +
                QStringLiteral(".%1").arg(index));
            configureNumeric(*editor, descriptor, true);
            editor->setValue(descriptor.value.at(index).get<double>());
            binding->editors.push_back(editor);
            layout->addWidget(editor, 1);
        }
        NumericBinding *binding_pointer = binding.get();
        numeric_bindings.push_back(std::move(binding));
        connectNumericBinding(binding_pointer);
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
        if (binding.euler) return eulerValue(binding);
        if (binding.direction) return directionValue(binding);
        if (binding.editors.size() > 1) {
            Json result = Json::array();
            for (const QDoubleSpinBox *editor : binding.editors) {
                result.push_back(editor->value());
            }
            return result;
        }
        const double value = binding.editors.front()->value() / binding.scale;
        if (binding.kind == InspectorWidgetKind::SignedIntegerDrag) {
            return static_cast<std::int64_t>(std::llround(value));
        }
        if (binding.kind == InspectorWidgetKind::UnsignedIntegerDrag) {
            return static_cast<std::uint64_t>(std::llround(value));
        }
        return value;
    }

    void beginNumeric(NumericBinding &binding) {
        // Q-7: nothing begins while another edit is in flight.
        if (binding.active || rebuilding || !inspectorIdle()) {
            return;
        }
        binding.active = model.beginWidgetEdit(binding.field_key);
        synchronize();
    }

    void numericChanged(NumericBinding &binding) {
        if (rebuilding) {
            return;
        }
        if (!binding.active) {
            // Q-7: typed while another edit is in flight, the value waits in
            // the field; finishing it sends it (finishNumeric).
            if (!inspectorIdle()) return;
            // Return re-emits the value it already had: not an edit.
            const auto *descriptor = descriptorFor(binding.field_key);
            if (descriptor != nullptr && showsValue(binding, descriptor->value)) return;
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
        if (rebuilding) {
            return;
        }
        if (!binding.active) {
            const auto *descriptor = descriptorFor(binding.field_key);
            // Angles that still show the stored rotation send nothing: made
            // back into a quaternion they differ from it in the last bits.
            if (commit && !(descriptor != nullptr && showsValue(binding, descriptor->value)))
                commitHeldValue(binding.field_key, numericValue(binding));
            return;
        }
        binding.active = false;
        model.finishWidgetEdit(binding.field_key, commit);
        synchronize();
        dispatchRequests();
    }

    // Q-7: a field finished without an edit of its own holds either what the
    // model already has (nothing to do) or a value typed while the Inspector
    // was busy. That value is committed now, or as soon as the Inspector is
    // idle.
    void commitHeldValue(const std::string &field_key, Json value) {
        const auto *descriptor = descriptorFor(field_key);
        if (descriptor == nullptr || !descriptor->authored ||
            !descriptor->value_matches_schema || descriptor->value == value)
            return;
        deferred_commit = std::pair{field_key, std::move(value)};
        flushDeferredCommit();
    }

    void beginText(TextBinding &binding) {
        // Q-7: nothing begins while another edit is in flight.
        if (binding.active || rebuilding || !inspectorIdle()) {
            return;
        }
        binding.active = model.beginWidgetEdit(binding.field_key);
        synchronize();
    }

    void finishText(TextBinding &binding, bool commit) {
        if (rebuilding) {
            return;
        }
        if (!binding.active) {
            if (commit && binding.editor->isModified()) {
                binding.editor->setModified(false);
                commitHeldValue(binding.field_key, binding.editor->text().toStdString());
            }
            return;
        }
        binding.active = false;
        binding.editor->setModified(false);
        model.finishWidgetEdit(binding.field_key, commit);
        synchronize();
        dispatchRequests();
    }

    bool handleEvent(QObject *watched, QEvent *event) {
        if (const auto numeric = numeric_events.find(watched);
            numeric != numeric_events.end()) {
            NumericBinding &binding = *numeric->second;
            switch (event->type()) {
            // Q-7: focus alone begins nothing. An edit begins when a value
            // changes (numericChanged) or a button is pressed on the field;
            // a field that merely has focus leaves Refresh, the other fields
            // and the next Add object free.
            case QEvent::MouseButtonPress:
                beginNumeric(binding);
                break;
            case QEvent::FocusOut:
                // Tab from x to y of one vector is still one edit.
                if (const QWidget *next = QApplication::focusWidget();
                    next != nullptr &&
                    std::any_of(binding.editors.begin(), binding.editors.end(),
                                [next](const QDoubleSpinBox *editor) {
                                    return editor == next || editor->isAncestorOf(next);
                                }))
                    break;
                finishNumeric(binding, true);
                break;
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

InspectorWidget::~InspectorWidget() {
    // QWidget clears focus before QObject disconnects its children. Stop the
    // editors' callbacks now, while Impl and their bindings are still alive.
    // This also covers text fields and every numeric representation.
    for (QObject *child : findChildren<QObject *>()) {
        child->removeEventFilter(this);
        QObject::disconnect(child, nullptr, this, nullptr);
    }
}

void InspectorWidget::setGraphSourceRevealAction(
    std::function<void(const QString &, const QString &)> action) {
    impl_->graph_source_reveal_action = std::move(action);
}

void InspectorWidget::setEngineRestartAction(
    std::function<void(const QString &reason)> action) {
    impl_->engine_restart_action = std::move(action);
}

std::optional<std::pair<std::string, std::string>> InspectorWidget::materialSubject() const {
    return impl_->materialSubject();
}

void InspectorWidget::setModelImportAction(
    std::function<void(std::function<void(const std::string &name)> registered)> action) {
    impl_->model_import_action = std::move(action);
}

void InspectorWidget::modelsChanged() {
    impl_->model.refreshModelAssets();
    impl_->dispatchRequests();
}

void InspectorWidget::resetMaterialState() {
    impl_->material_assign->session().open();
}

void InspectorWidget::materialsChanged() { impl_->material_assign->materialsChanged(); }

void InspectorWidget::materialBindingsChanged() { impl_->material_assign->bindingsChanged(); }

void InspectorWidget::revealMaterialRows() {
    if (impl_->material_assign->isVisible() && impl_->scroll != nullptr) {
        impl_->scroll->ensureWidgetVisible(impl_->material_assign);
        return;
    }
    impl_->reveal_material_rows = true;
}

void InspectorWidget::setRayGroupAction(std::function<void(const QString &)> action) {
    impl_->ray_group_action = std::move(action);
}

void InspectorWidget::setEnableHitGroupsAction(std::function<void()> action) {
    impl_->enable_hit_groups_action = std::move(action);
}

void InspectorWidget::setAssignHitGroupAction(std::function<void(const QString &material)> action) {
    impl_->assign_hit_group_action = std::move(action);
    impl_->synchronizeAssignHitGroup();
}

void InspectorWidget::setSelection(
    std::optional<OutlinerObjectKey> selection,
    const QString &display_text) {
    impl_->setSelection(std::move(selection), display_text);
}

void InspectorWidget::showRuntimeObject(const RuntimeObject &object) {
    impl_->showRuntimeObject(object);
}

bool InspectorWidget::showsRuntimeObject() const noexcept { return impl_->runtime_shown; }

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

QAction *InspectorWidget::undoAction() const noexcept { return impl_->undo_action; }
QAction *InspectorWidget::redoAction() const noexcept { return impl_->redo_action; }

void InspectorWidget::setSceneHistoryFocus(bool enabled) {
    impl_->scene_history_focus = enabled;
    impl_->undo_action->setEnabled(enabled && impl_->model.canUndoRedo());
    impl_->redo_action->setEnabled(enabled && impl_->model.canUndoRedo());
}

bool InspectorWidget::saveScene() { return impl_->saveScene(); }

SpawnOutcome InspectorWidget::spawnObject(const std::string &scene_id,
                                          nlohmann::json object, QString *error) {
    return impl_->spawnObject(scene_id, std::move(object), error);
}

SpawnOutcome InspectorWidget::submitSceneEdit(SceneEditKind kind, std::uint64_t subject_id,
                                              nlohmann::json operation,
                                              const std::string &description, QString *error) {
    return impl_->submitSceneEdit(kind, subject_id, std::move(operation), description, error);
}

void InspectorWidget::setSceneEditCompletedAction(
    std::function<void(const SceneEditOutcome &)> action) {
    impl_->scene_edit_completed = std::move(action);
}

bool InspectorWidget::hasUnsavedSceneChanges() const noexcept {
    return impl_->model.hasUnsavedChanges();
}
bool InspectorWidget::sceneEditPending() const noexcept {
    return impl_->model.busy() || impl_->model.refreshBlocked();
}
bool InspectorWidget::sceneSaveInFlight() const noexcept { return impl_->model.sceneSaveInFlight(); }
std::uint64_t InspectorWidget::sceneSaveCompletion() const noexcept {
    return impl_->model.sceneSaveCompletion();
}
bool InspectorWidget::lastSceneSaveSucceeded() const noexcept {
    return impl_->model.lastSceneSaveSucceeded();
}
void InspectorWidget::observeSceneRevision(std::uint64_t revision) {
    impl_->model.observeSceneRevision(revision);
    impl_->synchronize();
}
void InspectorWidget::resetSceneSaveState() {
    // The previous process can still send replies while it is shutting down.
    // They must not establish the new project's initial saved revision.
    impl_->transport_requests.clear();
    impl_->model.stopSession();
    impl_->model.resetSceneSaveState();
    impl_->synchronize();
}
void InspectorWidget::setSceneStateChangedAction(std::function<void()> action) {
    impl_->scene_state_changed = std::move(action);
}
void InspectorWidget::setPlaySessionHooks(PlaySessionHooks hooks) {
    impl_->play = std::move(hooks);
    impl_->synchronize();
}
void InspectorWidget::playSessionChanged() { impl_->synchronize(); }
void InspectorWidget::adoptRestoredRevisionAsSaved() {
    impl_->model.adoptRestoredRevisionAsSaved();
}
void InspectorWidget::refreshHistoryLabels() {
    impl_->model.refreshHistoryLabels();
    impl_->synchronize();
    impl_->dispatchRequests();
}
void InspectorWidget::refreshFromEngine() {
    impl_->model.requestRefresh();
    impl_->synchronize();
    impl_->dispatchRequests();
}

bool InspectorWidget::eventFilter(QObject *watched, QEvent *event) {
    return impl_->handleEvent(watched, event) ||
           QWidget::eventFilter(watched, event);
}

} // namespace PelicanStudio
