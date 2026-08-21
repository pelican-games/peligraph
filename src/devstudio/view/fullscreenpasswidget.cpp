#include "fullscreenpasswidget.hpp"

#include "../model/frameplanmodel.hpp"
#include "../model/frameplanresourcekind.hpp"

#include "passfieldownership.hpp"
#include "passshapepolicy.hpp"

#include <nlohmann/json.hpp>

#include <QAbstractItemView>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString text(std::string_view value) {
    return QString::fromUtf8(value.data(),
                             static_cast<qsizetype>(value.size()));
}

const Pelican::PassFieldOwnershipEntry &fullscreenOwnership() {
    const auto table = Pelican::passFieldOwnershipTable();
    const auto found = std::ranges::find(
        table, Pelican::RenderPassType::fullscreen,
        &Pelican::PassFieldOwnershipEntry::type);
    if (found == table.end()) {
        throw std::runtime_error(
            "passFieldOwnershipTable has no fullscreen entry");
    }
    return *found;
}

bool fullscreenOwns(std::string_view field) {
    const auto &entry = fullscreenOwnership();
    return std::ranges::find(entry.fields, field) != entry.fields.end();
}

std::vector<std::string> selectedSequence(const QListWidget &list) {
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(list.count()));
    for (int index = 0; index < list.count(); ++index) {
        result.push_back(list.item(index)->text().toStdString());
    }
    return result;
}

void populateCandidates(QComboBox &combo,
                        const std::vector<std::string> &names) {
    const QSignalBlocker blocker{&combo};
    combo.clear();
    for (const auto &name : names) {
        combo.addItem(text(name));
    }
    combo.setCurrentIndex(-1);
}

QStringList stringList(const std::set<std::string, std::less<>> &values) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto &value : values) {
        result.push_back(text(value));
    }
    return result;
}

} // namespace

struct FullscreenPassWidget::Impl {
    using ResourceKinds =
        std::map<std::string, Pelican::PassShapeResourceKind, std::less<>>;

    struct PlanBinding {
        std::string graph;
        ResourceKinds resources;

        bool operator==(const PlanBinding &) const = default;
    };

    struct ResourceReference {
        std::string name;
        Pelican::PassShapeResourceRole role;
        bool history = false;
    };

    FullscreenPassWidget &owner;
    FramePlanReadCapability frame_plan;
    RenderPassAuthoringCapability authoring;
    Pelican::PassShapePolicy shape_policy;
    QPushButton *refresh = nullptr;
    QLabel *plan_status = nullptr;
    QLabel *authoring_status = nullptr;
    QLabel *graph = nullptr;
    QLabel *type = nullptr;
    QSpinBox *position = nullptr;
    QLabel *location = nullptr;
    QLineEdit *name = nullptr;
    QLineEdit *vertex_shader = nullptr;
    QLineEdit *fragment_shader = nullptr;
    QComboBox *input_candidate = nullptr;
    QPushButton *add_input = nullptr;
    QPushButton *add_history_input = nullptr;
    QListWidget *inputs = nullptr;
    QPushButton *remove_input = nullptr;
    QComboBox *color_candidate = nullptr;
    QPushButton *add_color = nullptr;
    QListWidget *colors = nullptr;
    QPushButton *remove_color = nullptr;
    QComboBox *depth = nullptr;
    QPushButton *clear_depth = nullptr;
    QLabel *field_ownership = nullptr;
    QLabel *name_collision = nullptr;
    QLabel *target_names = nullptr;
    QLabel *pass_shape = nullptr;
    QLabel *history_support = nullptr;
    QLabel *target_usage = nullptr;
    QLabel *generation_order = nullptr;
    QLabel *shader_resolution = nullptr;
    QLabel *validation_summary = nullptr;
    QLabel *omitted = nullptr;
    QPlainTextEdit *json = nullptr;
    QPushButton *copy_json = nullptr;
    QPushButton *save = nullptr;
    QComboBox *remove_fragment = nullptr;
    QPushButton *remove_authored = nullptr;
    std::optional<FramePlanModel> plan;
    std::optional<PlanBinding> binding;
    ResourceKinds resource_kinds;
    std::map<std::string,
             std::map<std::string, Json, std::less<>>, std::less<>>
        authored_passes;
    std::map<std::string, std::size_t, std::less<>>
        authored_pass_counts;
    struct Anchor {
        int position = 0;
        std::string insert;
    };
    std::map<std::string, std::vector<Anchor>, std::less<>> anchors;
    std::string source_digest;
    bool authoring_available = false;
    bool authoring_rpc_available = false;
    bool refresh_available = false;
    bool updating = false;
    qint64 pending_request = 0;
    enum class AuthoringRequest {
        none,
        context,
        add,
        remove,
        result,
    };
    AuthoringRequest pending_authoring_kind = AuthoringRequest::none;
    qint64 pending_authoring_request = 0;
    QString pending_ticket;

    Impl(FullscreenPassWidget &widget, FramePlanReadCapability capability,
         RenderPassAuthoringCapability authoring_capability,
         const Pelican::PassShapePolicy &policy)
        : owner{widget}, frame_plan{std::move(capability)},
          authoring{std::move(authoring_capability)},
          shape_policy{policy} {
        if (!frame_plan.ready || !frame_plan.requestFramePlan ||
            !frame_plan.result || !frame_plan.failure) {
            throw std::invalid_argument(
                "FullscreenPassWidget requires a complete asynchronous "
                "frame-plan read capability");
        }
        if (!authoring.ready || !authoring.requestContext ||
            !authoring.addAuthoredPass || !authoring.removeAuthoredPass ||
            !authoring.requestEditResult || !authoring.result ||
            !authoring.failure) {
            throw std::invalid_argument(
                "FullscreenPassWidget requires a complete asynchronous "
                "render-pass authoring capability");
        }

        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        auto *toolbar = new QHBoxLayout;
        refresh = new QPushButton(owner.tr("Refresh frame plan"), &owner);
        refresh->setObjectName(
            QStringLiteral("pelican.fullscreenPass.refresh"));
        refresh->setToolTip(owner.tr(
            "Fetches get_frame_plan after an engine-owned authoring change."));
        plan_status = new QLabel(&owner);
        plan_status->setObjectName(
            QStringLiteral("pelican.fullscreenPass.planStatus"));
        plan_status->setWordWrap(true);
        toolbar->addWidget(refresh);
        toolbar->addWidget(plan_status, 1);
        layout->addLayout(toolbar);

        auto *scope = new QLabel(
            owner.tr("Save sends this draft to the engine. The engine owns "
                     "the project documents, validates the complete "
                     "candidate set, commits it, and publishes the new "
                     "frame plan."),
            &owner);
        scope->setObjectName(
            QStringLiteral("pelican.fullscreenPass.scopeNotice"));
        scope->setWordWrap(true);
        scope->setStyleSheet(QStringLiteral("color: #b36b00;"));
        layout->addWidget(scope);

        authoring_status = new QLabel(
            owner.tr("Engine authoring context is unavailable."), &owner);
        authoring_status->setObjectName(
            QStringLiteral("pelican.fullscreenPass.authoringStatus"));
        authoring_status->setWordWrap(true);
        layout->addWidget(authoring_status);

        auto *form = new QFormLayout;
        graph = new QLabel(owner.tr("(no frame plan)"), &owner);
        graph->setObjectName(
            QStringLiteral("pelican.fullscreenPass.graph"));
        form->addRow(owner.tr("Target graph"), graph);

        type = new QLabel(
            text(Pelican::renderPassTypeName(
                Pelican::RenderPassType::fullscreen)),
            &owner);
        type->setObjectName(
            QStringLiteral("pelican.fullscreenPass.type"));
        form->addRow(owner.tr("Type (fixed)"), type);

        position = new QSpinBox(&owner);
        position->setObjectName(
            QStringLiteral("pelican.fullscreenPass.position"));
        position->setRange(0, 0);
        form->addRow(owner.tr("Expected passes[] position"), position);

        location = new QLabel(&owner);
        location->setObjectName(
            QStringLiteral("pelican.fullscreenPass.location"));
        location->setWordWrap(true);
        form->addRow(owner.tr("Paste destination"), location);

        name = new QLineEdit(&owner);
        name->setObjectName(
            QStringLiteral("pelican.fullscreenPass.name"));
        form->addRow(owner.tr("Pass name"), name);

        vertex_shader = new QLineEdit(&owner);
        vertex_shader->setObjectName(
            QStringLiteral("pelican.fullscreenPass.shader.vertex"));
        vertex_shader->setPlaceholderText(
            owner.tr("for example engine://fullscreen"));
        form->addRow(owner.tr("Vertex shader stem"), vertex_shader);

        fragment_shader = new QLineEdit(&owner);
        fragment_shader->setObjectName(
            QStringLiteral("pelican.fullscreenPass.shader.fragment"));
        fragment_shader->setPlaceholderText(
            owner.tr("enter a stem; assets are not enumerated here"));
        form->addRow(owner.tr("Fragment shader stem"), fragment_shader);
        layout->addLayout(form);

        auto *targets = new QGroupBox(owner.tr("Frame-plan targets"), &owner);
        targets->setObjectName(
            QStringLiteral("pelican.fullscreenPass.targets"));
        auto *target_layout = new QGridLayout(targets);
        target_layout->addWidget(new QLabel(owner.tr("Input sequence"), targets),
                                 0, 0);
        target_layout->addWidget(new QLabel(owner.tr("Color outputs"), targets),
                                 0, 1);
        target_layout->addWidget(new QLabel(owner.tr("Depth output"), targets),
                                 0, 2);

        input_candidate = new QComboBox(targets);
        input_candidate->setObjectName(
            QStringLiteral("pelican.fullscreenPass.inputTarget"));
        input_candidate->setInsertPolicy(QComboBox::NoInsert);
        input_candidate->setPlaceholderText(owner.tr("Choose a resource"));
        add_input = new QPushButton(owner.tr("Add input"), targets);
        add_input->setObjectName(
            QStringLiteral("pelican.fullscreenPass.addInput"));
        add_history_input = new QPushButton(
            owner.tr("Add history input"), targets);
        add_history_input->setObjectName(
            QStringLiteral("pelican.fullscreenPass.addHistoryInput"));
        add_history_input->setEnabled(false);
        inputs = new QListWidget(targets);
        inputs->setObjectName(
            QStringLiteral("pelican.fullscreenPass.inputs"));
        inputs->setSelectionMode(QAbstractItemView::SingleSelection);
        inputs->setMaximumHeight(96);
        remove_input = new QPushButton(owner.tr("Remove selected"), targets);
        remove_input->setObjectName(
            QStringLiteral("pelican.fullscreenPass.removeInput"));

        color_candidate = new QComboBox(targets);
        color_candidate->setObjectName(
            QStringLiteral("pelican.fullscreenPass.colorTarget"));
        color_candidate->setInsertPolicy(QComboBox::NoInsert);
        color_candidate->setPlaceholderText(owner.tr("Choose a resource"));
        add_color = new QPushButton(owner.tr("Add color"), targets);
        add_color->setObjectName(
            QStringLiteral("pelican.fullscreenPass.addColor"));
        colors = new QListWidget(targets);
        colors->setObjectName(
            QStringLiteral("pelican.fullscreenPass.colors"));
        colors->setSelectionMode(QAbstractItemView::SingleSelection);
        colors->setMaximumHeight(96);
        remove_color = new QPushButton(owner.tr("Remove selected"), targets);
        remove_color->setObjectName(
            QStringLiteral("pelican.fullscreenPass.removeColor"));

        depth = new QComboBox(targets);
        depth->setObjectName(
            QStringLiteral("pelican.fullscreenPass.depthTarget"));
        depth->setInsertPolicy(QComboBox::NoInsert);
        depth->setPlaceholderText(owner.tr("No depth output"));
        clear_depth = new QPushButton(owner.tr("Clear depth"), targets);
        clear_depth->setObjectName(
            QStringLiteral("pelican.fullscreenPass.clearDepth"));

        target_layout->addWidget(input_candidate, 1, 0);
        target_layout->addWidget(color_candidate, 1, 1);
        target_layout->addWidget(depth, 1, 2);
        target_layout->addWidget(add_input, 2, 0);
        target_layout->addWidget(add_color, 2, 1);
        target_layout->addWidget(clear_depth, 2, 2);
        target_layout->addWidget(inputs, 3, 0);
        target_layout->addWidget(colors, 3, 1);
        target_layout->addWidget(new QLabel(
                                     owner.tr("No selection emits null."),
                                     targets),
                                 3, 2);
        target_layout->addWidget(remove_input, 4, 0);
        target_layout->addWidget(remove_color, 4, 1);
        target_layout->addWidget(add_history_input, 5, 0);
        layout->addWidget(targets);

        auto *validation = new QGroupBox(owner.tr("Validation axes"), &owner);
        validation->setObjectName(
            QStringLiteral("pelican.fullscreenPass.validation"));
        auto *validation_layout = new QVBoxLayout(validation);
        const auto make_axis = [&](const char *object_name) {
            auto *label = new QLabel(validation);
            label->setObjectName(QString::fromLatin1(object_name));
            label->setWordWrap(true);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            validation_layout->addWidget(label);
            return label;
        };
        field_ownership = make_axis(
            "pelican.fullscreenPass.axis.fieldOwnership");
        name_collision = make_axis(
            "pelican.fullscreenPass.axis.nameCollision");
        target_names = make_axis(
            "pelican.fullscreenPass.axis.targetNames");
        pass_shape = make_axis(
            "pelican.fullscreenPass.axis.passShape");
        history_support = make_axis(
            "pelican.fullscreenPass.axis.historySupport");
        target_usage = make_axis(
            "pelican.fullscreenPass.axis.targetUsage");
        generation_order = make_axis(
            "pelican.fullscreenPass.axis.generationOrder");
        shader_resolution = make_axis(
            "pelican.fullscreenPass.axis.shaderResolution");
        layout->addWidget(validation);

        validation_summary = new QLabel(&owner);
        validation_summary->setObjectName(
            QStringLiteral("pelican.fullscreenPass.validationSummary"));
        validation_summary->setWordWrap(true);
        validation_summary->setTextInteractionFlags(
            Qt::TextSelectableByMouse);
        layout->addWidget(validation_summary);

        omitted = new QLabel(&owner);
        omitted->setObjectName(
            QStringLiteral("pelican.fullscreenPass.omittedKeys"));
        omitted->setWordWrap(true);
        omitted->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(omitted);

        json = new QPlainTextEdit(&owner);
        json->setObjectName(
            QStringLiteral("pelican.fullscreenPass.json"));
        json->setReadOnly(true);
        json->setLineWrapMode(QPlainTextEdit::NoWrap);
        json->setMinimumHeight(170);
        layout->addWidget(json, 1);

        copy_json = new QPushButton(owner.tr("Copy JSON"), &owner);
        copy_json->setObjectName(
            QStringLiteral("pelican.fullscreenPass.copyJson"));
        layout->addWidget(copy_json);

        auto *authoring_controls = new QHBoxLayout;
        save = new QPushButton(owner.tr("Save authored pass"), &owner);
        save->setObjectName(
            QStringLiteral("pelican.fullscreenPass.save"));
        save->setToolTip(owner.tr(
            "Calls add_authored_pass with the current engine digest and "
            "anchor."));
        remove_fragment = new QComboBox(&owner);
        remove_fragment->setObjectName(
            QStringLiteral("pelican.fullscreenPass.removeFragment"));
        remove_fragment->setPlaceholderText(
            owner.tr("Choose a managed fragment"));
        remove_authored = new QPushButton(
            owner.tr("Remove authored pass"), &owner);
        remove_authored->setObjectName(
            QStringLiteral("pelican.fullscreenPass.remove"));
        remove_authored->setToolTip(owner.tr(
            "Calls remove_authored_pass. Only engine-reported managed "
            "fragments are offered here."));
        authoring_controls->addWidget(save);
        authoring_controls->addWidget(remove_fragment, 1);
        authoring_controls->addWidget(remove_authored);
        layout->addLayout(authoring_controls);

        QObject::connect(refresh, &QPushButton::clicked, &owner,
                         [this] { requestRefresh(); });
        QObject::connect(position, qOverload<int>(&QSpinBox::valueChanged),
                         &owner, [this](int) { updateDraft(); });
        QObject::connect(name, &QLineEdit::textChanged, &owner,
                         [this] { updateDraft(); });
        QObject::connect(vertex_shader, &QLineEdit::textChanged, &owner,
                         [this] { updateDraft(); });
        QObject::connect(fragment_shader, &QLineEdit::textChanged, &owner,
                         [this] { updateDraft(); });
        QObject::connect(depth, &QComboBox::currentTextChanged, &owner,
                         [this] { updateDraft(); });
        QObject::connect(
            input_candidate, qOverload<int>(&QComboBox::currentIndexChanged),
            &owner, [this](int) { updateHistoryInputEnabled(); });
        QObject::connect(clear_depth, &QPushButton::clicked, &owner,
                         [this] {
                             depth->setCurrentIndex(-1);
                             updateDraft();
                         });
        QObject::connect(add_input, &QPushButton::clicked, &owner,
                         [this] { appendCandidate(*input_candidate, *inputs); });
        QObject::connect(add_history_input, &QPushButton::clicked, &owner,
                         [this] {
                             appendCandidate(*input_candidate, *inputs,
                                             true);
                         });
        QObject::connect(add_color, &QPushButton::clicked, &owner,
                         [this] { appendCandidate(*color_candidate, *colors); });
        QObject::connect(remove_input, &QPushButton::clicked, &owner,
                         [this] { removeSelected(*inputs); });
        QObject::connect(remove_color, &QPushButton::clicked, &owner,
                         [this] { removeSelected(*colors); });
        QObject::connect(inputs, &QListWidget::itemDoubleClicked, &owner,
                         [this](QListWidgetItem *) { removeSelected(*inputs); });
        QObject::connect(colors, &QListWidget::itemDoubleClicked, &owner,
                         [this](QListWidgetItem *) { removeSelected(*colors); });

        QObject::connect(copy_json, &QPushButton::clicked, &owner,
                         [this] {
                             QApplication::clipboard()->setText(
                                 json->toPlainText());
                         });
        QObject::connect(save, &QPushButton::clicked, &owner,
                         [this] { requestAddAuthoredPass(); });
        QObject::connect(remove_authored, &QPushButton::clicked, &owner,
                         [this] { requestRemoveAuthoredPass(); });
        QObject::connect(
            remove_fragment, qOverload<int>(&QComboBox::currentIndexChanged),
            &owner, [this](int) { updateAuthoringButtons(); });

        refresh->setEnabled(false);
        save->setEnabled(false);
        remove_authored->setEnabled(false);
        plan_status->setText(owner.tr("Frame plan unavailable."));
        updateDraft();

        frame_plan.result(
            &owner,
            [this](qint64 request_id,
                   const QByteArray &result_json) {
                receiveRefreshResult(request_id, result_json);
            });
        frame_plan.failure(
            &owner,
            [this](qint64 request_id, const QString &message) {
                receiveRefreshFailure(request_id, message);
            });
        frame_plan.ready(
            &owner,
            [this](bool available, const QString &reason) {
                setRefreshAvailable(available, reason);
            });
        authoring.result(
            &owner,
            [this](qint64 request_id,
                   const QByteArray &result_json) {
                receiveAuthoringResult(request_id, result_json);
            });
        authoring.failure(
            &owner,
            [this](qint64 request_id, const QString &message) {
                receiveAuthoringFailure(request_id, message);
            });
        authoring.ready(
            &owner,
            [this](bool available, const QString &reason) {
                setAuthoringRpcAvailable(available, reason);
            });
    }

    Json draftJson() const {
        Json draft = Json::object();
        draft["name"] = name->text().toStdString();
        draft["type"] = std::string{Pelican::renderPassTypeName(
            Pelican::RenderPassType::fullscreen)};
        draft["input"] = selectedSequence(*inputs);
        draft["output"] = Json{
            {"color", selectedSequence(*colors)},
            {"depth", depth->currentIndex() < 0
                          ? Json(nullptr)
                          : Json(depth->currentText().toStdString())},
        };
        // The ownership authority, rather than a Studio-maintained field set,
        // decides whether this type may emit its shader editor value.
        if (fullscreenOwns("shader")) {
            draft["shader"] = Json{
                {"vertex", vertex_shader->text().toStdString()},
                {"fragment", fragment_shader->text().toStdString()},
            };
        }
        return draft;
    }

    void setAxis(QLabel &label, const QString &axis, const char *state,
                 const QString &detail) {
        const QString state_text =
            QString::fromLatin1(state) == QStringLiteral("valid")
                ? owner.tr("Valid")
                : QString::fromLatin1(state) == QStringLiteral("invalid")
                      ? owner.tr("Invalid")
                      : owner.tr("Not checked");
        label.setProperty("pelicanValidationState",
                          QString::fromLatin1(state));
        label.setText(owner.tr("%1: %2 — %3")
                          .arg(axis, state_text, detail));
    }

    std::vector<ResourceReference> referencedResources() const {
        std::vector<ResourceReference> result;
        for (const auto &authored : selectedSequence(*inputs)) {
            const auto input =
                Pelican::parsePassShapeInputReference(authored);
            result.push_back(ResourceReference{
                .name = input.name,
                .role = Pelican::PassShapeResourceRole::input,
                .history = input.history,
            });
        }
        const auto color_names = selectedSequence(*colors);
        for (const auto &color : color_names) {
            result.push_back(ResourceReference{
                .name = color,
                .role = Pelican::PassShapeResourceRole::color_output,
            });
        }
        if (depth->currentIndex() >= 0) {
            result.push_back(ResourceReference{
                .name = depth->currentText().toStdString(),
                .role = Pelican::PassShapeResourceRole::depth_output,
            });
        }
        return result;
    }

    void updateOmitted(const Json &draft) {
        omitted->setProperty("pelicanOmittedKeys", QStringList{});
        if (!authoring_available) {
            omitted->setText(owner.tr(
                "Omitted keys: not known because no authored declaration is "
                "available. The JSON still contains only fields represented "
                "by this form."));
            return;
        }
        if (!plan) {
            omitted->setText(owner.tr(
                "Omitted keys: not known until a target graph is loaded. The "
                "JSON still contains only fields represented by this form."));
            return;
        }
        const auto graph_found = authored_passes.find(plan->graph);
        const auto pass_found =
            graph_found == authored_passes.end()
                ? std::map<std::string, Json, std::less<>>::const_iterator{}
                : graph_found->second.find(name->text().toStdString());
        if (graph_found == authored_passes.end() ||
            pass_found == graph_found->second.end()) {
            omitted->setText(owner.tr(
                "Omitted keys: no existing declaration has this graph/name. "
                "This is a new partial draft and only represented fields are "
                "emitted."));
            return;
        }

        std::set<std::string, std::less<>> missing;
        for (const auto &[key, value] : pass_found->second.items()) {
            (void)value;
            if (!draft.contains(key)) {
                missing.insert(key);
            }
        }
        const QStringList reported = stringList(missing);
        omitted->setProperty("pelicanOmittedKeys", reported);
        omitted->setText(
            missing.empty()
                ? owner.tr("Omitted keys: none for the matching authored "
                           "declaration. The output remains a form-owned "
                           "projection, not a complete validation result.")
                : owner.tr("Omitted keys from matching authored declaration: "
                           "%1")
                      .arg(reported.join(QStringLiteral(", "))));
    }

    void updateValidationSummary() {
        const std::array axes{
            std::pair{field_ownership, owner.tr("Field ownership")},
            std::pair{name_collision,
                      owner.tr("Same-graph name collision")},
            std::pair{target_names, owner.tr("Target names")},
            std::pair{pass_shape, owner.tr("Pass shape")},
            std::pair{history_support,
                      owner.tr("History input support")},
            std::pair{target_usage,
                      owner.tr("Target usage compatibility")},
            std::pair{generation_order,
                      owner.tr("Input generation order")},
            std::pair{shader_resolution,
                      owner.tr("Shader stem resolution")},
        };
        QStringList invalid;
        QStringList not_checked;
        for (const auto &[axis, axis_name] : axes) {
            const QString state =
                axis->property("pelicanValidationState").toString();
            if (state == QStringLiteral("invalid")) {
                invalid.push_back(axis_name);
            } else if (state == QStringLiteral("not_checked")) {
                not_checked.push_back(axis_name);
            }
        }

        const bool required_values_present =
            plan.has_value() && !name->text().trimmed().isEmpty() &&
            !vertex_shader->text().trimmed().isEmpty() &&
            !fragment_shader->text().trimmed().isEmpty();
        if (!invalid.isEmpty()) {
            validation_summary->setProperty(
                "pelicanValidationSummaryState",
                QStringLiteral("invalid"));
            validation_summary->setStyleSheet(
                QStringLiteral("color: #d94c3d;"));
            validation_summary->setText(
                owner.tr("Draft cannot be copied: Invalid axes — %1.")
                    .arg(invalid.join(QStringLiteral(", "))));
            copy_json->setEnabled(false);
            return;
        }
        if (!required_values_present) {
            validation_summary->setProperty(
                "pelicanValidationSummaryState",
                QStringLiteral("incomplete"));
            validation_summary->setStyleSheet(
                QStringLiteral("color: #b36b00;"));
            validation_summary->setText(owner.tr(
                "Draft is incomplete; load a frame plan and provide a name "
                "and both shader stems before copying."));
            copy_json->setEnabled(false);
            return;
        }
        if (!not_checked.isEmpty()) {
            validation_summary->setProperty(
                "pelicanValidationSummaryState",
                QStringLiteral("partial"));
            validation_summary->setStyleSheet(
                QStringLiteral("color: #b36b00;"));
            validation_summary->setText(
                owner.tr("Partial draft: Not checked — %1. Copying is "
                         "available, but these named axes still require "
                         "engine-side review.")
                    .arg(not_checked.join(QStringLiteral(", "))));
            copy_json->setEnabled(true);
            return;
        }

        validation_summary->setProperty(
            "pelicanValidationSummaryState", QStringLiteral("valid"));
        validation_summary->setStyleSheet({});
        validation_summary->setText(
            owner.tr("All displayed validation axes are Valid; copying is "
                     "available."));
        copy_json->setEnabled(true);
    }

    void updateDraft() {
        if (updating) {
            return;
        }
        const Json draft = draftJson();
        json->setPlainText(QString::fromStdString(draft.dump(2)));

        const QString graph_name =
            plan ? text(plan->graph) : owner.tr("(no frame plan)");
        graph->setText(graph_name);
        location->setText(
            plan ? owner.tr("Graph '%1', passes[%2] "
                            "(declaration_index %2)")
                       .arg(graph_name)
                       .arg(position->value())
                 : owner.tr("Load a frame plan to name the graph and "
                            "declaration_index."));

        try {
            (void)Pelican::validatePassFieldOwnership(
                draft, Pelican::PassFieldOwnershipCapabilities{},
                "fullscreen pass form draft");
            setAxis(*field_ownership, owner.tr("Field ownership"), "valid",
                    owner.tr("generated type-owned fields come from "
                             "passFieldOwnershipTable()."));
        } catch (const std::exception &error) {
            setAxis(*field_ownership, owner.tr("Field ownership"), "invalid",
                    QString::fromUtf8(error.what()));
        }

        try {
            std::vector<std::string> non_image_inputs;
            for (const auto &[resource_name, kind] : resource_kinds) {
                if (kind == Pelican::PassShapeResourceKind::buffer) {
                    non_image_inputs.push_back(resource_name);
                }
            }
            const auto violations = Pelican::evaluatePassShape(
                shape_policy,
                Pelican::passShapeObservationFromJson(
                    Pelican::RenderPassType::fullscreen, draft,
                    non_image_inputs));
            QStringList violation_names;
            for (const auto &violation : violations) {
                violation_names.push_back(text(
                    Pelican::passShapeViolationName(violation.kind)));
            }
            pass_shape->setProperty("pelicanPassShapeViolations",
                                    violation_names);
            if (violations.empty()) {
                setAxis(*pass_shape, owner.tr("Pass shape"), "valid",
                        owner.tr("the injected PassShapePolicy accepts the "
                                 "input/output shape."));
            } else {
                setAxis(*pass_shape, owner.tr("Pass shape"), "invalid",
                        text(Pelican::passShapeViolationDescription(
                            violations.front())));
            }
        } catch (const std::exception &error) {
            pass_shape->setProperty("pelicanPassShapeViolations",
                                    QStringList{});
            setAxis(*pass_shape, owner.tr("Pass shape"), "invalid",
                    QString::fromUtf8(error.what()));
        }

        if (!plan || !authoring_available) {
            setAxis(*name_collision, owner.tr("Same-graph name collision"),
                    "not_checked",
                    !plan ? owner.tr("no frame plan is loaded.")
                          : owner.tr("no authored configuration is loaded."));
        } else if (authored_passes.find(plan->graph) ==
                   authored_passes.end()) {
            setAxis(*name_collision, owner.tr("Same-graph name collision"),
                    "not_checked",
                    owner.tr("the authored configuration has no graph '%1'.")
                        .arg(text(plan->graph)));
        } else if (name->text().isEmpty()) {
            setAxis(*name_collision, owner.tr("Same-graph name collision"),
                    "invalid", owner.tr("the pass name is empty."));
        } else {
            const auto runtime_collision = std::ranges::find(
                plan->nodes, name->text().toStdString(),
                &FramePlanNode::name);
            const auto &authored = authored_passes.at(plan->graph);
            const bool authored_collision =
                authored.contains(name->text().toStdString());
            const bool collision = authored_collision ||
                                   runtime_collision != plan->nodes.end();
            setAxis(*name_collision, owner.tr("Same-graph name collision"),
                    collision ? "invalid" : "valid",
                    !collision
                        ? owner.tr("the name is absent from the authored and "
                                   "runtime graph.")
                        : owner.tr("'%1' already exists in the %2 graph '%3'.")
                              .arg(name->text(),
                                   authored_collision
                                       ? owner.tr("authored")
                                       : owner.tr("runtime"),
                                   text(plan->graph)));
        }

        if (!plan) {
            setAxis(*target_names, owner.tr("Target names"), "not_checked",
                    owner.tr("no frame plan is loaded."));
        } else {
            const auto references = referencedResources();
            std::vector<std::string> invalid_names;
            for (const auto &reference : references) {
                const auto found = resource_kinds.find(reference.name);
                if (found == resource_kinds.end()) {
                    invalid_names.push_back(reference.name);
                    continue;
                }
                if (!Pelican::passShapeResourceSupportsRole(
                        found->second, reference.name, reference.role)) {
                    invalid_names.push_back(reference.name);
                }
            }
            if (references.empty()) {
                setAxis(*target_names, owner.tr("Target names"),
                        "not_checked",
                        owner.tr("no target names are selected."));
            } else if (!invalid_names.empty()) {
                setAxis(*target_names, owner.tr("Target names"), "invalid",
                        owner.tr("unknown or role-incompatible resources: %1")
                            .arg(text(invalid_names.front())));
            } else {
                setAxis(*target_names, owner.tr("Target names"), "valid",
                        owner.tr("every selected name and role is compatible "
                                 "with this frame plan's resources[].kind."));
            }
        }

        if (!plan) {
            setAxis(*history_support, owner.tr("History input support"),
                    "not_checked", owner.tr("no frame plan is loaded."));
        } else {
            const auto references = referencedResources();
            const auto history = std::ranges::find(
                references, true, &ResourceReference::history);
            if (history == references.end()) {
                setAxis(*history_support,
                        owner.tr("History input support"), "valid",
                        owner.tr("no @history input is selected."));
            } else {
                const auto unsupported = std::ranges::find_if(
                    references, [this](const ResourceReference &reference) {
                        if (!reference.history) {
                            return false;
                        }
                        const auto found =
                            resource_kinds.find(reference.name);
                        return found != resource_kinds.end() &&
                               Pelican::passShapeResourceHistorySupport(
                                   found->second) ==
                                   Pelican::PassShapeHistorySupport::unsupported;
                    });
                if (unsupported != references.end()) {
                    setAxis(*history_support,
                            owner.tr("History input support"), "invalid",
                            owner.tr("'%1' cannot be read with @history for "
                                     "its published resource kind.")
                                .arg(text(unsupported->name)));
                } else {
                    setAxis(*history_support,
                            owner.tr("History input support"), "not_checked",
                            owner.tr("the frame plan does not publish whether "
                                     "render targets support history."));
                }
            }
        }

        setAxis(*target_usage, owner.tr("Target usage compatibility"),
                "not_checked",
                plan ? owner.tr("the frame plan does not publish resource "
                                "usage.")
                     : owner.tr("no frame plan is loaded."));
        setAxis(*generation_order, owner.tr("Input generation order"),
                "not_checked",
                plan ? owner.tr("Studio does not reproduce the engine's "
                                "position-dependent producer validation.")
                     : owner.tr("no frame plan is loaded."));
        setAxis(*shader_resolution, owner.tr("Shader stem resolution"),
                "not_checked",
                owner.tr("resolution depends on pelican_core and build "
                         "capabilities; Studio does not predict it."));
        updateOmitted(draft);
        updateValidationSummary();
        updateAuthoringButtons();
    }

    void updateAuthoringButtons() {
        const QString validation =
            validation_summary
                ->property("pelicanValidationSummaryState")
                .toString();
        bool selected_anchor = false;
        if (plan) {
            const auto found = anchors.find(plan->graph);
            if (found != anchors.end()) {
                selected_anchor = std::ranges::any_of(
                    found->second, [&](const Anchor &anchor) {
                        return anchor.position == position->value();
                    });
            }
        }
        const bool idle = pending_authoring_request == 0 &&
                          pending_ticket.isEmpty();
        save->setEnabled(
            authoring_rpc_available && authoring_available && idle &&
            source_digest.size() == 64 && selected_anchor &&
            (validation == QStringLiteral("valid") ||
             validation == QStringLiteral("partial")));
        remove_authored->setEnabled(
            authoring_rpc_available && authoring_available && idle &&
            source_digest.size() == 64 &&
            remove_fragment->currentIndex() >= 0);
    }

    void authoringError(const QString &message) {
        authoring_status->setStyleSheet(
            QStringLiteral("color: #d94c3d;"));
        authoring_status->setText(
            owner.tr("Render authoring failed: %1").arg(message));
        updateAuthoringButtons();
    }

    static QString responseError(const Json &response) {
        const auto error = response.find("error");
        if (error != response.end() && error->is_object()) {
            const auto message = error->find("message");
            if (message != error->end() && message->is_string()) {
                return text(message->get_ref<const std::string &>());
            }
        }
        return QStringLiteral("engine rejected the authoring request");
    }

    void requestAuthoringContext() {
        if (!authoring_rpc_available || pending_authoring_request != 0) {
            return;
        }
        QString error;
        pending_authoring_kind = AuthoringRequest::context;
        pending_authoring_request = authoring.requestContext(&error);
        if (pending_authoring_request == 0) {
            pending_authoring_kind = AuthoringRequest::none;
            authoringError(error);
            return;
        }
        authoring_status->setStyleSheet({});
        authoring_status->setText(
            owner.tr("Requesting the engine-owned authoring context..."));
        updateAuthoringButtons();
    }

    std::optional<Anchor> selectedAnchor() const {
        if (!plan) return std::nullopt;
        const auto graph_anchors = anchors.find(plan->graph);
        if (graph_anchors == anchors.end()) return std::nullopt;
        const auto found = std::ranges::find(
            graph_anchors->second, position->value(), &Anchor::position);
        if (found == graph_anchors->second.end()) return std::nullopt;
        return *found;
    }

    void requestAddAuthoredPass() {
        if (!save->isEnabled() || !plan) return;
        const auto anchor = selectedAnchor();
        if (!anchor) {
            authoringError(
                owner.tr("the selected position has no engine anchor"));
            return;
        }
        QJsonParseError parse_error;
        const auto pass_document = QJsonDocument::fromJson(
            QByteArray::fromStdString(draftJson().dump()), &parse_error);
        if (parse_error.error != QJsonParseError::NoError ||
            !pass_document.isObject()) {
            authoringError(owner.tr("the generated pass JSON is invalid"));
            return;
        }
        const QJsonObject params{
            {QStringLiteral("base_source_digest"),
             QString::fromStdString(source_digest)},
            {QStringLiteral("graph"), text(plan->graph)},
            {QStringLiteral("insert"), text(anchor->insert)},
            {QStringLiteral("pass"), pass_document.object()},
        };
        QString error;
        pending_authoring_kind = AuthoringRequest::add;
        pending_authoring_request =
            authoring.addAuthoredPass(params, &error);
        if (pending_authoring_request == 0) {
            pending_authoring_kind = AuthoringRequest::none;
            authoringError(error);
            return;
        }
        authoring_status->setStyleSheet({});
        authoring_status->setText(
            owner.tr("Submitting the complete authored-pass candidate..."));
        updateAuthoringButtons();
    }

    void requestRemoveAuthoredPass() {
        if (!remove_authored->isEnabled()) return;
        const auto reference =
            remove_fragment->currentData().toString();
        if (reference.isEmpty()) return;
        const QJsonObject params{
            {QStringLiteral("base_source_digest"),
             QString::fromStdString(source_digest)},
            {QStringLiteral("fragment_reference"), reference},
        };
        QString error;
        pending_authoring_kind = AuthoringRequest::remove;
        pending_authoring_request =
            authoring.removeAuthoredPass(params, &error);
        if (pending_authoring_request == 0) {
            pending_authoring_kind = AuthoringRequest::none;
            authoringError(error);
            return;
        }
        authoring_status->setStyleSheet({});
        authoring_status->setText(
            owner.tr("Submitting the managed-fragment removal..."));
        updateAuthoringButtons();
    }

    void requestTicketResult() {
        if (!authoring_rpc_available || pending_ticket.isEmpty() ||
            pending_authoring_request != 0) {
            return;
        }
        QString error;
        pending_authoring_kind = AuthoringRequest::result;
        pending_authoring_request =
            authoring.requestEditResult(pending_ticket, &error);
        if (pending_authoring_request == 0) {
            pending_authoring_kind = AuthoringRequest::none;
            authoringError(error);
            return;
        }
        updateAuthoringButtons();
    }

    void consumeAuthoringContext(const QByteArray &context_json) {
        const Json context = Json::parse(
            context_json.constData(),
            context_json.constData() + context_json.size());
        if (!context.is_object() || !context.contains("source_digest") ||
            !context.at("source_digest").is_object() ||
            !context.at("source_digest").contains("hex") ||
            !context.at("source_digest").at("hex").is_string() ||
            !context.contains("graphs") ||
            !context.at("graphs").is_array()) {
            throw std::runtime_error(
                "render authoring context requires source_digest.hex and graphs[]");
        }
        const auto next_digest =
            context.at("source_digest").at("hex").get<std::string>();
        if (next_digest.size() != 64) {
            throw std::runtime_error(
                "render authoring context digest must contain 64 hex characters");
        }

        decltype(authored_passes) next_passes;
        decltype(authored_pass_counts) next_counts;
        decltype(anchors) next_anchors;
        QStringList provenance_summary;
        for (const auto &graph_value : context.at("graphs")) {
            if (!graph_value.is_object() ||
                !graph_value.contains("name") ||
                !graph_value.at("name").is_string() ||
                !graph_value.contains("passes") ||
                !graph_value.at("passes").is_array() ||
                !graph_value.contains("anchor_candidates") ||
                !graph_value.at("anchor_candidates").is_array()) {
                throw std::runtime_error(
                    "render authoring graphs require name, passes[] and anchor_candidates[]");
            }
            const auto graph_name =
                graph_value.at("name").get<std::string>();
            auto [pass_position, inserted] =
                next_passes.try_emplace(graph_name);
            if (!inserted) {
                throw std::runtime_error(
                    "duplicate graph in render authoring context: " +
                    graph_name);
            }
            const auto &passes = graph_value.at("passes");
            next_counts.emplace(graph_name, passes.size());
            for (const auto &pass_value : passes) {
                if (!pass_value.is_object() ||
                    !pass_value.contains("name") ||
                    !pass_value.at("name").is_string()) {
                    throw std::runtime_error(
                        "render authoring passes require a string name");
                }
                const auto pass_name =
                    pass_value.at("name").get<std::string>();
                const Json &declaration =
                    pass_value.contains("declaration")
                        ? pass_value.at("declaration")
                        : pass_value;
                if (!declaration.is_object() ||
                    !pass_position->second
                         .emplace(pass_name, declaration)
                         .second) {
                    throw std::runtime_error(
                        "duplicate pass in render authoring context: " +
                        graph_name + "/" + pass_name);
                }
                if (pass_value.contains("provenance") &&
                    pass_value.at("provenance").is_object()) {
                    const auto &provenance =
                        pass_value.at("provenance");
                    provenance_summary.push_back(
                        text(graph_name + "/" + pass_name + " <- " +
                             provenance.value("source", std::string{})));
                }
            }
            auto &graph_anchors = next_anchors[graph_name];
            for (const auto &anchor :
                 graph_value.at("anchor_candidates")) {
                if (!anchor.is_object() || !anchor.contains("position") ||
                    !anchor.at("position").is_number_integer() ||
                    !anchor.contains("insert") ||
                    !anchor.at("insert").is_string()) {
                    throw std::runtime_error(
                        "render authoring anchors require position and insert");
                }
                graph_anchors.push_back(Anchor{
                    .position = anchor.at("position").get<int>(),
                    .insert = anchor.at("insert").get<std::string>(),
                });
            }
        }

        const QSignalBlocker blocker{remove_fragment};
        remove_fragment->clear();
        if (const auto fragments = context.find("managed_fragments");
            fragments != context.end()) {
            if (!fragments->is_array()) {
                throw std::runtime_error(
                    "managed_fragments must be an array");
            }
            for (const auto &fragment : *fragments) {
                if (!fragment.is_object() ||
                    !fragment.contains("reference") ||
                    !fragment.at("reference").is_string()) {
                    throw std::runtime_error(
                        "managed fragment requires a reference");
                }
                const auto reference =
                    fragment.at("reference").get<std::string>();
                QString label = text(reference);
                if (fragment.contains("pass_names") &&
                    fragment.at("pass_names").is_array() &&
                    !fragment.at("pass_names").empty()) {
                    label = text(fragment.at("pass_names").front()
                                     .get<std::string>()) +
                            QStringLiteral(" — ") + text(reference);
                }
                remove_fragment->addItem(label, text(reference));
            }
        }
        remove_fragment->setCurrentIndex(-1);

        source_digest = next_digest;
        authored_passes = std::move(next_passes);
        authored_pass_counts = std::move(next_counts);
        anchors = std::move(next_anchors);
        authoring_available = true;
        const auto kind = context.value("config_kind", std::string{"direct"});
        authoring_status->setStyleSheet({});
        authoring_status->setText(
            owner.tr("Engine authoring context loaded (%1): %2 graph(s), "
                     "%3 managed fragment(s).")
                .arg(text(kind))
                .arg(static_cast<qulonglong>(authored_passes.size()))
                .arg(remove_fragment->count()));
        authoring_status->setToolTip(
            provenance_summary.isEmpty()
                ? owner.tr("The engine returned no pass provenance entries.")
                : owner.tr("Resolved pass provenance: %1")
                      .arg(provenance_summary.join(
                          QStringLiteral(", "))));
        configurePosition();
        updateDraft();
    }

    void receiveAuthoringResult(qint64 request_id,
                                const QByteArray &result_json) {
        if (request_id != pending_authoring_request) return;
        const auto completed_kind = pending_authoring_kind;
        pending_authoring_request = 0;
        pending_authoring_kind = AuthoringRequest::none;
        try {
            if (completed_kind == AuthoringRequest::context) {
                consumeAuthoringContext(result_json);
                return;
            }
            const Json response = Json::parse(
                result_json.constData(),
                result_json.constData() + result_json.size());
            if (!response.is_object()) {
                throw std::runtime_error(
                    "render authoring response must be an object");
            }
            const auto status = response.value("status", std::string{});
            if (completed_kind == AuthoringRequest::add ||
                completed_kind == AuthoringRequest::remove) {
                if (status != "accepted" ||
                    !response.contains("ticket") ||
                    !response.at("ticket").is_string()) {
                    authoringError(responseError(response));
                    return;
                }
                pending_ticket =
                    text(response.at("ticket").get_ref<const std::string &>());
                authoring_status->setStyleSheet({});
                authoring_status->setText(
                    owner.tr("Candidate accepted; waiting for engine "
                             "publication..."));
                updateAuthoringButtons();
                QTimer::singleShot(
                    0, &owner, [this] { requestTicketResult(); });
                return;
            }
            if (completed_kind == AuthoringRequest::result) {
                if (status == "pending") {
                    QTimer::singleShot(
                        25, &owner,
                        [this] { requestTicketResult(); });
                    return;
                }
                if (status != "committed" ||
                    !response.value("committed", false)) {
                    pending_ticket.clear();
                    authoringError(responseError(response));
                    return;
                }
                pending_ticket.clear();
                authoring_status->setStyleSheet({});
                authoring_status->setText(
                    owner.tr("Authored pass transaction committed and "
                             "published."));
                updateAuthoringButtons();
                requestAuthoringContext();
                requestRefresh();
            }
        } catch (const std::exception &error) {
            pending_ticket.clear();
            authoringError(QString::fromUtf8(error.what()));
        }
    }

    void receiveAuthoringFailure(qint64 request_id,
                                 const QString &message) {
        if (request_id != pending_authoring_request) return;
        pending_authoring_request = 0;
        pending_authoring_kind = AuthoringRequest::none;
        pending_ticket.clear();
        authoringError(message);
    }

    void setAuthoringRpcAvailable(bool available,
                                  const QString &reason) {
        pending_authoring_request = 0;
        pending_authoring_kind = AuthoringRequest::none;
        pending_ticket.clear();
        authoring_rpc_available = available;
        if (available) {
            requestAuthoringContext();
            return;
        }
        source_digest.clear();
        anchors.clear();
        authored_passes.clear();
        authored_pass_counts.clear();
        authoring_available = false;
        remove_fragment->clear();
        authoring_status->setToolTip({});
        authoring_status->setStyleSheet(
            QStringLiteral("color: #b36b00;"));
        authoring_status->setText(
            reason.isEmpty()
                ? owner.tr("Engine authoring context is unavailable.")
                : owner.tr("Engine authoring context is unavailable: %1")
                      .arg(reason));
        configurePosition();
        updateDraft();
    }

    void appendCandidate(QComboBox &candidate, QListWidget &sequence,
                         bool history = false) {
        if (candidate.currentIndex() < 0) {
            return;
        }
        QString selected = candidate.currentText();
        if (history) {
            selected += QStringLiteral("@history");
        }
        sequence.addItem(selected);
        candidate.setCurrentIndex(-1);
        updateDraft();
    }

    void updateHistoryInputEnabled() {
        if (input_candidate->currentIndex() < 0) {
            add_history_input->setEnabled(false);
            return;
        }
        const auto name = input_candidate->currentText().toStdString();
        const auto found = resource_kinds.find(name);
        add_history_input->setEnabled(
            found != resource_kinds.end() &&
            Pelican::passShapeResourceHistorySupport(found->second) !=
                Pelican::PassShapeHistorySupport::unsupported);
    }

    std::vector<std::string> candidatesForRole(
        Pelican::PassShapeResourceRole role) const {
        std::vector<std::string> result;
        for (const auto &[name, kind] : resource_kinds) {
            if (Pelican::passShapeResourceSupportsRole(kind, name, role)) {
                result.push_back(name);
            }
        }
        return result;
    }

    void populateRoleCandidates() {
        populateCandidates(
            *input_candidate,
            candidatesForRole(Pelican::PassShapeResourceRole::input));
        populateCandidates(
            *color_candidate,
            candidatesForRole(
                Pelican::PassShapeResourceRole::color_output));
        populateCandidates(
            *depth,
            candidatesForRole(
                Pelican::PassShapeResourceRole::depth_output));
        updateHistoryInputEnabled();
    }

    void removeSelected(QListWidget &sequence) {
        const int row = sequence.currentRow();
        if (row >= 0) {
            delete sequence.takeItem(row);
            updateDraft();
        }
    }

    void clearDraft() {
        updating = true;
        name->clear();
        vertex_shader->clear();
        fragment_shader->clear();
        inputs->clear();
        colors->clear();
        depth->setCurrentIndex(-1);
        position->setValue(0);
        updating = false;
    }

    void configurePosition() {
        std::size_t last = 0;
        if (plan && authoring_available) {
            const auto found = authored_pass_counts.find(plan->graph);
            if (found != authored_pass_counts.end()) {
                last = found->second;
            }
        }
        const auto capped = std::min<std::size_t>(
            last, static_cast<std::size_t>(std::numeric_limits<int>::max()));
        position->setRange(0, static_cast<int>(capped));
        position->setValue(static_cast<int>(capped));
    }

    void invalidateFramePlanContext() {
        plan.reset();
        binding.reset();
        resource_kinds.clear();
        populateRoleCandidates();
        clearDraft();
        configurePosition();
        updateDraft();
    }

    void consumeResult(const QByteArray &result_json) {
        try {
            const std::string_view response{
                result_json.constData(),
                static_cast<std::size_t>(result_json.size())};
            const Json wire = Json::parse(response);
            const auto resources = wire.find("resources");
            if (resources == wire.end() || !resources->is_array()) {
                throw std::runtime_error(
                    "frame plan response requires resources[] for this form");
            }
            ResourceKinds next_kinds;
            for (std::size_t index = 0; index < resources->size(); ++index) {
                const auto &resource = resources->at(index);
                if (!resource.is_object() || !resource.contains("name") ||
                    !resource.at("name").is_string()) {
                    throw std::runtime_error(
                        "frame plan resources[] entries require a string name");
                }
                const std::string resource_name =
                    resource.at("name").get<std::string>();
                const std::string context =
                    "resources[" + std::to_string(index) + "]";
                const auto kind =
                    decodeFramePlanResourceKind(resource, context);
                next_kinds.insert_or_assign(resource_name, kind);
            }
            FramePlanModel next = buildFramePlanModel(response);
            PlanBinding next_binding{
                .graph = next.graph,
                .resources = next_kinds,
            };
            const bool changed = binding && *binding != next_binding;
            const QString previous_depth = depth->currentText();

            plan = std::move(next);
            binding = next_binding;
            resource_kinds = std::move(next_kinds);
            if (changed) {
                clearDraft();
            }
            populateRoleCandidates();
            if (!changed && !previous_depth.isEmpty()) {
                depth->setCurrentIndex(
                    depth->findText(previous_depth, Qt::MatchExactly));
            }
            configurePosition();
            refresh->setEnabled(refresh_available);
            plan_status->setStyleSheet(
                changed ? QStringLiteral("color: #b36b00;") : QString{});
            plan_status->setText(
                changed
                    ? owner.tr("Frame plan graph/resources changed; the old "
                               "draft was discarded. Draft restoration is "
                               "outside this work package.")
                    : owner.tr("Bound to graph '%1' and %2 resources. "
                               "runtime_generation is not part of the "
                               "binding key.")
                          .arg(text(plan->graph))
                          .arg(static_cast<qulonglong>(
                              next_binding.resources.size())));
            updateDraft();
        } catch (const std::exception &error) {
            invalidateFramePlanContext();
            refresh->setEnabled(refresh_available);
            plan_status->setStyleSheet(
                QStringLiteral("color: #d94c3d;"));
            plan_status->setText(
                owner.tr("Frame plan refresh failed: %1")
                    .arg(QString::fromUtf8(error.what())));
        }
    }

    void requestRefresh() {
        if (pending_request != 0) {
            return;
        }
        if (!refresh_available) {
            setRefreshAvailable(false,
                                owner.tr("the RPC connection is not ready."));
            return;
        }
        QString error;
        pending_request = frame_plan.requestFramePlan(&error);
        if (pending_request == 0) {
            invalidateFramePlanContext();
            plan_status->setStyleSheet(
                QStringLiteral("color: #d94c3d;"));
            plan_status->setText(owner.tr("Frame plan refresh failed: %1")
                                     .arg(error));
            return;
        }
        refresh->setEnabled(false);
        plan_status->setStyleSheet({});
        plan_status->setText(owner.tr("Requesting the current frame plan..."));
    }

    void receiveRefreshResult(qint64 request_id,
                              const QByteArray &result_json) {
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        consumeResult(result_json);
    }

    void receiveRefreshFailure(qint64 request_id, const QString &message) {
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        invalidateFramePlanContext();
        refresh->setEnabled(refresh_available);
        plan_status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        plan_status->setText(
            owner.tr("Frame plan refresh failed: %1")
                .arg(message));
    }

    void setRefreshAvailable(bool available, const QString &reason) {
        pending_request = 0;
        refresh_available = available;
        refresh->setEnabled(available);
        if (available) {
            plan_status->setStyleSheet({});
            plan_status->setText(
                owner.tr("Frame plan RPC available; press Refresh frame plan."));
            return;
        }
        invalidateFramePlanContext();
        plan_status->setStyleSheet(QStringLiteral("color: #b36b00;"));
        plan_status->setText(
            reason.isEmpty()
                ? owner.tr("Frame plan unavailable.")
                : owner.tr("Frame plan unavailable: %1").arg(reason));
    }

    void consumeAuthoringConfig(const QByteArray &config_json) {
        try {
            source_digest.clear();
            anchors.clear();
            remove_fragment->clear();
            const Json root = Json::parse(
                config_json.constData(),
                config_json.constData() + config_json.size());
            if (!root.is_object()) {
                throw std::runtime_error(
                    "rendering config must be a JSON object");
            }
            const auto graphs = root.find("rendering_passes");
            if (graphs == root.end() || !graphs->is_array()) {
                throw std::runtime_error(
                    "resolved rendering config requires rendering_passes[]");
            }

            decltype(authored_passes) next;
            decltype(authored_pass_counts) next_counts;
            for (const auto &graph_value : *graphs) {
                if (!graph_value.is_object() ||
                    !graph_value.contains("name") ||
                    !graph_value.at("name").is_string() ||
                    !graph_value.contains("passes") ||
                    !graph_value.at("passes").is_array()) {
                    throw std::runtime_error(
                        "rendering_passes entries require name and passes[]");
                }
                const std::string graph_name =
                    graph_value.at("name").get<std::string>();
                auto [graph_position, inserted] =
                    next.try_emplace(graph_name);
                if (!inserted) {
                    throw std::runtime_error(
                        "duplicate rendering graph name: " + graph_name);
                }
                auto &graph_entry = graph_position->second;
                const auto &passes = graph_value.at("passes");
                next_counts.emplace(graph_name, passes.size());
                for (const auto &pass : passes) {
                    if (!pass.is_object() || !pass.contains("name") ||
                        !pass.at("name").is_string()) {
                        throw std::runtime_error(
                            "passes[] entries require a string name");
                    }
                    const std::string pass_name =
                        pass.at("name").get<std::string>();
                    if (!graph_entry.emplace(pass_name, pass).second) {
                        throw std::runtime_error(
                            "duplicate pass name in graph '" + graph_name +
                            "': " + pass_name);
                    }
                }
            }
            authored_passes = std::move(next);
            authored_pass_counts = std::move(next_counts);
            authoring_available = true;
            std::size_t pass_count = 0;
            QStringList authored_names;
            for (const auto &[graph_name, passes] : authored_passes) {
                pass_count += passes.size();
                for (const auto &[pass_name, declaration] : passes) {
                    (void)declaration;
                    authored_names.push_back(
                        text(graph_name + "/" + pass_name));
                }
            }
            authoring_status->setStyleSheet({});
            authoring_status->setText(
                owner.tr("Projection-test authoring context loaded: %1 "
                         "graph(s), %2 pass(es). Saving remains disabled "
                         "without an engine digest and anchors.")
                    .arg(static_cast<qulonglong>(authored_passes.size()))
                    .arg(static_cast<qulonglong>(pass_count)));
            authoring_status->setToolTip(
                owner.tr("Authored graph/pass declarations: %1")
                    .arg(authored_names.join(QStringLiteral(", "))));
        } catch (const std::exception &error) {
            authored_passes.clear();
            authored_pass_counts.clear();
            authoring_available = false;
            authoring_status->setToolTip({});
            authoring_status->setStyleSheet(
                QStringLiteral("color: #b36b00;"));
            authoring_status->setText(
                owner.tr("Authoring context unavailable: %1")
                    .arg(QString::fromUtf8(error.what())));
        }
        configurePosition();
        updateDraft();
    }
};

FullscreenPassWidget::FullscreenPassWidget(
    FramePlanReadCapability frame_plan, QWidget *parent)
    : FullscreenPassWidget(
          std::move(frame_plan),
          unavailableRenderPassAuthoringCapability(),
                           Pelican::defaultPassShapePolicy(), parent) {}

FullscreenPassWidget::FullscreenPassWidget(
    FramePlanReadCapability frame_plan,
    RenderPassAuthoringCapability authoring,
    QWidget *parent)
    : FullscreenPassWidget(std::move(frame_plan), std::move(authoring),
                           Pelican::defaultPassShapePolicy(), parent) {}

FullscreenPassWidget::FullscreenPassWidget(
    FramePlanReadCapability frame_plan,
    const Pelican::PassShapePolicy &shape_policy, QWidget *parent)
    : FullscreenPassWidget(
          std::move(frame_plan),
          unavailableRenderPassAuthoringCapability(), shape_policy,
          parent) {}

FullscreenPassWidget::FullscreenPassWidget(
    FramePlanReadCapability frame_plan,
    RenderPassAuthoringCapability authoring,
    const Pelican::PassShapePolicy &shape_policy, QWidget *parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("pelican.fullscreenPass"));
    impl_ = std::make_unique<Impl>(
        *this, std::move(frame_plan), std::move(authoring), shape_policy);
}

FullscreenPassWidget::~FullscreenPassWidget() = default;

void FullscreenPassWidget::receiveResult(const QByteArray &result_json) {
    impl_->pending_request = 0;
    impl_->consumeResult(result_json);
}

void FullscreenPassWidget::receiveAuthoringConfig(
    const QByteArray &config_json) {
    impl_->consumeAuthoringConfig(config_json);
}

} // namespace PelicanStudio
