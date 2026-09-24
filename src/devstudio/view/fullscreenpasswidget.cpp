#include <set>
#include "fullscreenpasswidget.hpp"

#include "../model/frameplanmodel.hpp"
#include "../model/frameplanresourcekind.hpp"

#include "fullscreenpasssettings.hpp"
#include "fullscreenshadertemplate.hpp"
#include "panelhelp.hpp"
#include "shadercompilelogpanel.hpp"
#include "shaderdiagnostics.hpp"
#include "passfieldownership.hpp"
#include "renderconfigsection.hpp"
#include "passshapepolicy.hpp"
#include "rasterpass.hpp"

#include <nlohmann/json.hpp>

#include <QAbstractItemView>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QComboBox>
#include <QCheckBox>
#include <QDesktopServices>
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
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <filesystem>
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
    std::function<void()> committed_action;
    RenderPassAuthoringCapability authoring;
    Pelican::PassShapePolicy shape_policy;
    QPushButton *refresh = nullptr;
    QLabel *plan_status = nullptr;
    QLabel *authoring_status = nullptr;
    QLabel *graph = nullptr;
    QLabel *type = nullptr;
    QFormLayout *form = nullptr;
    QGroupBox *targets = nullptr;
    QGroupBox *validation = nullptr;
    QSpinBox *position = nullptr;
    QLabel *location = nullptr;
    QLineEdit *name = nullptr;
    QLineEdit *vertex_shader = nullptr;
    QLineEdit *fragment_shader = nullptr;
    // F-3: the fragment row carries "New...", which writes a starting shader
    // into the project; the row under it asks for the file's name.
    QWidget *fragment_row = nullptr;
    QPushButton *new_fragment_shader = nullptr;
    QWidget *shader_template_row = nullptr;
    QLineEdit *shader_template_name = nullptr;
    QPushButton *create_fragment_shader = nullptr;
    QPushButton *open_existing_shader = nullptr;
    QLabel *shader_template_notice = nullptr;
    // G-3: which starting point "New..." writes; the pass's scene lights
    // and push constants; each colour output's load, clear and store; and
    // what a template reads each input as.
    QComboBox *shader_template_kind = nullptr;
    FullscreenPassInterfaceRow *interface_row = nullptr;
    FullscreenOutputOpsEditor *output_ops = nullptr;
    QLabel *input_roles = nullptr;
    // The names "New..." filled in itself, so it may replace them when the
    // kind changes but never replaces a name the author typed.
    QString auto_pass_name;
    QString auto_shader_name;
    ShaderCompileLogPanel *compile_log = nullptr;
    // F-5: what a shader reload failure put in the log, so a later commit
    // takes down that log and not an authoring rejection shown since.
    QString reload_log_text;
    std::optional<std::filesystem::path> project_root;
    std::function<bool(const std::filesystem::path &)> shader_open_action;
    QLineEdit *skinned_vertex_shader = nullptr;
    QCheckBox *ray_query = nullptr;
    QLineEdit *ray_query_scene = nullptr;
    QPlainTextEdit *draw = nullptr;
    QPlainTextEdit *raster_state = nullptr;
    QComboBox *material_contract = nullptr;
    QPlainTextEdit *material_settings = nullptr;
    QPlainTextEdit *material_routing = nullptr;
    Json preset_material_routing = nullptr;
    QComboBox *input_candidate = nullptr;
    QLabel *connection_hint = nullptr;
    std::optional<QStringList> input_filter;
    // N-8: "after this node", from the Frame Plan canvas. It survives context
    // refreshes so the position is still right when the anchors arrive.
    std::optional<std::string> requested_insert_after;
    QPushButton *add_input = nullptr;
    QPushButton *add_history_input = nullptr;
    QListWidget *inputs = nullptr;
    QPushButton *remove_input = nullptr;
    QComboBox *color_candidate = nullptr;
    QPushButton *add_color = nullptr;
    QListWidget *colors = nullptr;
    QPushButton *remove_color = nullptr;
    QPlainTextEdit *color_attachment_references = nullptr;
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
    std::string source_digest, preset_digest;
    // F-4: "direct" when the root declares its passes itself and selects no
    // preset, which is what a Blank pipeline project is; with the reference of
    // that root, so a refusal can name the file to edit instead.
    std::string config_kind;
    std::string root_reference;
    std::set<std::pair<std::string, std::string>> root_declared_passes;
    // F-6: the ones the engine offers to edit in the root itself
    // (editable_root_pass); their declarations sit in preset_passes too.
    std::set<std::pair<std::string, std::string>> root_passes;
    std::map<std::pair<std::string, std::string>, Json> preset_passes;
    std::map<std::pair<std::string, std::string>, Json> feature_pass_info;
    std::optional<std::pair<std::string, std::string>> editing_preset_pass;
    bool load_edit_requested = false;
    Json original_preset_pass = Json::object();

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
        edit,
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

        owner.setFont(studioPanelFont());

        // N-7 rule 2: the two facts that used to be two paragraphs are one
        // row -- what the frame plan is, and what the authoring context is.
        // Everything that explained why is on the control it explains, or
        // behind the `?` at the end of this row.
        auto *toolbar = new QHBoxLayout;
        refresh = new QPushButton(owner.tr("Refresh"), &owner);
        refresh->setObjectName(
            QStringLiteral("pelican.fullscreenPass.refresh"));
        refresh->setToolTip(owner.tr(
            "Fetches get_frame_plan after an engine-owned authoring change."));
        plan_status = new QLabel(&owner);
        plan_status->setObjectName(
            QStringLiteral("pelican.fullscreenPass.planStatus"));
        applyPanelNoticeStyle(plan_status);
        authoring_status = new QLabel(&owner);
        authoring_status->setObjectName(
            QStringLiteral("pelican.fullscreenPass.authoringStatus"));
        applyPanelNoticeStyle(authoring_status);
        toolbar->addWidget(refresh);
        toolbar->addWidget(plan_status);
        toolbar->addWidget(authoring_status, 1);
        toolbar->addWidget(makePanelHelpButton(
            &owner, "pelican.fullscreenPass.help", ManualPassEditorAnchor,
            owner.tr("Save sends this draft to the engine. The engine owns "
                     "the project documents, validates the complete "
                     "candidate set, commits it, and publishes the new "
                     "frame plan.")));
        layout->addLayout(toolbar);
        setPanelNotice(
            authoring_status, owner.tr("authoring context unavailable"),
            owner.tr("Engine authoring context is unavailable."));

        form = new QFormLayout;
        graph = new QLabel(owner.tr("(no frame plan)"), &owner);
        graph->setObjectName(
            QStringLiteral("pelican.fullscreenPass.graph"));
        form->addRow(owner.tr("Graph"), graph);

        type = new QLabel(
            text(Pelican::renderPassTypeName(
                Pelican::RenderPassType::fullscreen)),
            &owner);
        type->setObjectName(
            QStringLiteral("pelican.fullscreenPass.type"));
        type->setToolTip(owner.tr("Fixed by the pass kind being edited."));
        form->addRow(owner.tr("Type"), type);

        position = new QSpinBox(&owner);
        position->setObjectName(
            QStringLiteral("pelican.fullscreenPass.position"));
        position->setRange(0, 0);
        position->setToolTip(
            owner.tr("Expected passes[] position in the target graph."));
        form->addRow(owner.tr("Position"), position);

        location = new QLabel(&owner);
        location->setObjectName(
            QStringLiteral("pelican.fullscreenPass.location"));
        applyPanelNoticeStyle(location);
        form->addRow(owner.tr("Destination"), location);

        name = new QLineEdit(&owner);
        name->setObjectName(
            QStringLiteral("pelican.fullscreenPass.name"));
        form->addRow(owner.tr("Name"), name);

        vertex_shader = new QLineEdit(&owner);
        vertex_shader->setObjectName(
            QStringLiteral("pelican.fullscreenPass.shader.vertex"));
        vertex_shader->setPlaceholderText(
            owner.tr("for example engine://fullscreen"));
        form->addRow(owner.tr("Vertex"), vertex_shader);

        fragment_shader = new QLineEdit(&owner);
        fragment_shader->setObjectName(
            QStringLiteral("pelican.fullscreenPass.shader.fragment"));
        fragment_shader->setPlaceholderText(
            owner.tr("enter a stem; assets are not enumerated here"));
        buildShaderTemplateRows();
        skinned_vertex_shader = new QLineEdit(&owner);
        skinned_vertex_shader->setObjectName("pelican.fullscreenPass.shader.skinnedVertex");
        skinned_vertex_shader->setPlaceholderText(owner.tr("Blank uses the engine default"));
        form->addRow(owner.tr("Skinned vertex stem"), skinned_vertex_shader);
        form->setRowVisible(skinned_vertex_shader, false);
        ray_query = new QCheckBox(owner.tr("Scene ray query"), &owner);
        ray_query->setObjectName("pelican.fullscreenPass.rayQuery");
        form->addRow(ray_query);
        ray_query_scene = new QLineEdit("scene", &owner);
        ray_query_scene->setObjectName("pelican.fullscreenPass.rayQueryScene");
        ray_query_scene->setEnabled(false);
        ray_query_scene->setToolTip(
            owner.tr("The scene acceleration structure the ray query reads."));
        form->addRow(owner.tr("Ray scene"), ray_query_scene);

        raster_state = new QPlainTextEdit(&owner);
        raster_state->setObjectName(
            QStringLiteral("pelican.fullscreenPass.rasterState"));
        raster_state->setPlaceholderText(owner.tr(
            "Optional raster_state JSON, for example "
            "{\"color_attachments\":[{\"blend\":\"additive\"}]}"));
        raster_state->setLineWrapMode(QPlainTextEdit::NoWrap);
        raster_state->setMaximumHeight(92);
        form->addRow(owner.tr("Raster state"), raster_state);
        draw = new QPlainTextEdit(&owner);
        draw->setObjectName("pelican.fullscreenPass.draw");
        draw->setPlaceholderText(owner.tr("{\"vertex_count\":3,\"instance_count\":1}"));
        draw->setMaximumHeight(92);
        form->addRow(owner.tr("Draw"), draw);
        draw->hide();
        form->setRowVisible(draw, false);
        material_contract = new QComboBox(&owner);
        material_contract->setObjectName("pelican.fullscreenPass.materialContract");
        material_contract->addItem(owner.tr("Default (no explicit contract)"), QString{});
        for (const auto *contract : {"legacy_gbuffer_v1", "deferred_geometry_v1", "forward_opaque_v1", "forward_transparent_v1"})
            material_contract->addItem(QString::fromLatin1(contract), QString::fromLatin1(contract));
        form->addRow(owner.tr("Contract"), material_contract);
        material_settings = new QPlainTextEdit(&owner);
        material_settings->setObjectName("pelican.fullscreenPass.materialSettings");
        material_settings->setMaximumHeight(140);
        material_settings->setPlaceholderText(owner.tr("Material selection, variant, outputs and resources (JSON object). Remove a key to use its default."));
        form->addRow(owner.tr("Material"), material_settings);
        material_routing = new QPlainTextEdit(&owner);
        material_routing->setObjectName("pelican.fullscreenPass.materialRouting");
        material_routing->setMaximumHeight(110);
        material_routing->setToolTip(owner.tr("Preset-wide routing shared by all graphs. Select distinct deferred_geometry, forward_opaque and forward_transparent passes using policy hybrid_auto_v1. Blank removes the routing table."));
        form->addRow(owner.tr("Routing"), material_routing);
        for (auto *field : std::array<QWidget *, 3>{material_contract, material_settings, material_routing})
            form->setRowVisible(field, false);
        layout->addLayout(form);

        targets = new QGroupBox(owner.tr("Targets"), &owner);
        targets->setObjectName(
            QStringLiteral("pelican.fullscreenPass.targets"));
        targets->setToolTip(owner.tr(
            "The frame-plan resources this pass reads and writes."));
        auto *target_layout = new QGridLayout(targets);
        target_layout->addWidget(new QLabel(owner.tr("Inputs"), targets),
                                 0, 0);
        target_layout->addWidget(new QLabel(owner.tr("Color"), targets),
                                 0, 1);
        target_layout->addWidget(new QLabel(owner.tr("Depth"), targets),
                                 0, 2);

        input_candidate = new QComboBox(targets);
        input_candidate->setObjectName(
            QStringLiteral("pelican.fullscreenPass.inputTarget"));
        input_candidate->setInsertPolicy(QComboBox::NoInsert);
        input_candidate->setPlaceholderText(owner.tr("Choose a resource"));
        add_input = new QPushButton(owner.tr("Add"), targets);
        add_input->setObjectName(
            QStringLiteral("pelican.fullscreenPass.addInput"));
        add_history_input = new QPushButton(
            owner.tr("Add @history"), targets);
        add_history_input->setObjectName(
            QStringLiteral("pelican.fullscreenPass.addHistoryInput"));
        add_history_input->setEnabled(false);
        add_history_input->setToolTip(owner.tr(
            "Reads the selected resource as it was left by the previous "
            "frame."));
        inputs = new QListWidget(targets);
        inputs->setObjectName(
            QStringLiteral("pelican.fullscreenPass.inputs"));
        inputs->setSelectionMode(QAbstractItemView::SingleSelection);
        inputs->setMaximumHeight(96);
        remove_input = new QPushButton(owner.tr("Remove"), targets);
        remove_input->setObjectName(
            QStringLiteral("pelican.fullscreenPass.removeInput"));

        color_candidate = new QComboBox(targets);
        color_candidate->setObjectName(
            QStringLiteral("pelican.fullscreenPass.colorTarget"));
        color_candidate->setInsertPolicy(QComboBox::NoInsert);
        color_candidate->setPlaceholderText(owner.tr("Choose a resource"));
        add_color = new QPushButton(owner.tr("Add"), targets);
        add_color->setObjectName(
            QStringLiteral("pelican.fullscreenPass.addColor"));
        colors = new QListWidget(targets);
        colors->setObjectName(
            QStringLiteral("pelican.fullscreenPass.colors"));
        colors->setSelectionMode(QAbstractItemView::SingleSelection);
        colors->setMaximumHeight(96);
        remove_color = new QPushButton(owner.tr("Remove"), targets);
        remove_color->setObjectName(
            QStringLiteral("pelican.fullscreenPass.removeColor"));

        depth = new QComboBox(targets);
        depth->setObjectName(
            QStringLiteral("pelican.fullscreenPass.depthTarget"));
        depth->setInsertPolicy(QComboBox::NoInsert);
        depth->setPlaceholderText(owner.tr("No depth output"));
        depth->setToolTip(owner.tr("No selection emits null."));
        clear_depth = new QPushButton(owner.tr("Clear"), targets);
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
        target_layout->addWidget(remove_input, 4, 0);
        target_layout->addWidget(remove_color, 4, 1);
        target_layout->addWidget(add_history_input, 5, 0);
        connection_hint = new QLabel(targets);
        connection_hint->setObjectName("pelican.fullscreenPass.connectionHint");
        applyPanelNoticeStyle(connection_hint);
        connection_hint->hide();
        target_layout->addWidget(connection_hint, 8, 0, 1, 3);
        color_attachment_references = new QPlainTextEdit(targets);
        color_attachment_references->setObjectName(QStringLiteral(
            "pelican.fullscreenPass.colorAttachmentReferences"));
        color_attachment_references->setPlaceholderText(owner.tr(
            "Optional output.color JSON for attachment objects, for example "
            "[{\"target\":\"color\",\"load_op\":\"Load\"}]. Targets must "
            "match the selected color outputs in order."));
        color_attachment_references->setLineWrapMode(QPlainTextEdit::NoWrap);
        color_attachment_references->setMaximumHeight(92);
        auto *attachment_label =
            new QLabel(owner.tr("output.color (JSON)"), targets);
        attachment_label->setToolTip(owner.tr(
            "Optional attachment objects for the selected color outputs, in "
            "order."));
        target_layout->addWidget(attachment_label, 6, 0, 1, 3);
        target_layout->addWidget(color_attachment_references, 7, 0, 1, 3);
        buildPassSettings(*target_layout);
        layout->addWidget(targets);

        // N-7 rule 3: eight sentences were eight lines. They are now eight
        // marks on one line; the sentence each mark stands for is the tooltip
        // of that mark, which is where a reader who wants it looks.
        validation = new QGroupBox(owner.tr("Checks"), &owner);
        validation->setObjectName(
            QStringLiteral("pelican.fullscreenPass.validation"));
        auto *validation_layout = new QHBoxLayout(validation);
        validation_layout->setSpacing(10);
        const auto make_axis = [&](const char *object_name) {
            auto *label = new QLabel(validation);
            label->setObjectName(QString::fromLatin1(object_name));
            label->setFont(studioSmallFont());
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
        validation_layout->addStretch(1);
        layout->addWidget(validation);

        validation_summary = new QLabel(&owner);
        validation_summary->setObjectName(
            QStringLiteral("pelican.fullscreenPass.validationSummary"));
        applyPanelNoticeStyle(validation_summary);
        validation_summary->setTextInteractionFlags(
            Qt::TextSelectableByMouse);
        layout->addWidget(validation_summary);

        omitted = new QLabel(&owner);
        omitted->setObjectName(
            QStringLiteral("pelican.fullscreenPass.omittedKeys"));
        applyPanelNoticeStyle(omitted);
        omitted->setTextInteractionFlags(Qt::TextSelectableByMouse);
        omitted->hide();
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

        compile_log = new ShaderCompileLogPanel(&owner);
        compile_log->setObjectName(
            QStringLiteral("pelican.fullscreenPass.compileLog"));
        layout->addWidget(compile_log);

        auto *authoring_controls = new QHBoxLayout;
        save = new QPushButton(owner.tr("Save"), &owner);
        save->setObjectName(
            QStringLiteral("pelican.fullscreenPass.save"));
        save->setToolTip(authoredPassSaveToolTip());
        remove_fragment = new QComboBox(&owner);
        remove_fragment->setObjectName(
            QStringLiteral("pelican.fullscreenPass.removeFragment"));
        remove_fragment->setPlaceholderText(
            owner.tr("Choose a managed fragment"));
        remove_authored = new QPushButton(owner.tr("Remove"), &owner);
        remove_authored->setObjectName(
            QStringLiteral("pelican.fullscreenPass.remove"));
        remove_authored->setToolTip(owner.tr(
            "Calls remove_authored_pass. Only engine-reported managed "
            "fragments are offered here."));
        authoring_controls->addWidget(save);
        auto *new_pass = new QPushButton(owner.tr("New pass"), &owner);
        new_pass->setObjectName("pelican.fullscreenPass.new");
        authoring_controls->addWidget(new_pass);
        QObject::connect(new_pass, &QPushButton::clicked, &owner, [this] {
            if (pending_authoring_request || !pending_ticket.isEmpty()) return;
            editing_preset_pass.reset(); load_edit_requested = false;
            input_filter.reset(); populateRoleCandidates();
            name->setReadOnly(false); position->setEnabled(true); raster_state->setEnabled(true);
            save->setText(owner.tr("Save"));
            clearDraft(); updateDraft();
        });
        authoring_controls->addWidget(remove_fragment, 1);
        authoring_controls->addWidget(remove_authored);
        layout->addLayout(authoring_controls);

        QObject::connect(refresh, &QPushButton::clicked, &owner,
                         [this] { requestRefresh(); });
        QObject::connect(position, qOverload<int>(&QSpinBox::valueChanged),
                         &owner, [this](int) { updateDraft(); });
        QObject::connect(ray_query, &QCheckBox::toggled, &owner, [this](bool enabled) {
            ray_query_scene->setEnabled(enabled);
            updateDraft();
        });
        QObject::connect(ray_query_scene, &QLineEdit::textChanged, &owner, [this] { updateDraft(); });
        QObject::connect(name, &QLineEdit::textChanged, &owner,
                         [this] { updateDraft(); });
        QObject::connect(vertex_shader, &QLineEdit::textChanged, &owner,
                         [this] { updateDraft(); });
        QObject::connect(fragment_shader, &QLineEdit::textChanged, &owner,
                         [this] { updateDraft(); });
        QObject::connect(skinned_vertex_shader, &QLineEdit::textChanged, &owner,
                         [this] { updateDraft(); });
        QObject::connect(draw, &QPlainTextEdit::textChanged, &owner, [this] { updateDraft(); });
        QObject::connect(material_contract, qOverload<int>(&QComboBox::currentIndexChanged), &owner, [this] { updateDraft(); });
        QObject::connect(material_settings, &QPlainTextEdit::textChanged, &owner, [this] { updateDraft(); });
        QObject::connect(material_routing, &QPlainTextEdit::textChanged, &owner, [this] { updateDraft(); });
        QObject::connect(raster_state, &QPlainTextEdit::textChanged, &owner,
                         [this] { updateDraft(); });
        QObject::connect(color_attachment_references,
                         &QPlainTextEdit::textChanged, &owner,
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
        setPanelNotice(plan_status, owner.tr("no frame plan"),
                       owner.tr("Frame plan unavailable."));
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

    // --- F-3: a fullscreen pass is made shader and all.

    void buildShaderTemplateRows() {
        // The same desktop hand-off the Frame Plan's shader Open uses.
        shader_open_action = [](const std::filesystem::path &path) {
            return QDesktopServices::openUrl(QUrl::fromLocalFile(
                QString::fromStdWString(path.wstring())));
        };
        fragment_row = new QWidget(&owner);
        fragment_row->setObjectName(
            QStringLiteral("pelican.fullscreenPass.fragmentRow"));
        auto *fragment_layout = new QHBoxLayout(fragment_row);
        fragment_layout->setContentsMargins(0, 0, 0, 0);
        fragment_layout->addWidget(fragment_shader, 1);
        new_fragment_shader =
            new QPushButton(owner.tr("New…"), fragment_row);
        new_fragment_shader->setObjectName(
            QStringLiteral("pelican.fullscreenPass.newFragmentShader"));
        fragment_layout->addWidget(new_fragment_shader);
        form->addRow(owner.tr("Fragment"), fragment_row);

        shader_template_row = new QWidget(&owner);
        shader_template_row->setObjectName(
            QStringLiteral("pelican.fullscreenPass.shaderTemplateRow"));
        auto *template_layout = new QHBoxLayout(shader_template_row);
        template_layout->setContentsMargins(0, 0, 0, 0);
        shader_template_name = new QLineEdit(shader_template_row);
        shader_template_name->setObjectName(
            QStringLiteral("pelican.fullscreenPass.shaderTemplateName"));
        shader_template_name->setToolTip(owner.tr(
            "Written to shaders/<name>.frag in the project. Letters, digits, "
            "'_' and '-'."));
        create_fragment_shader =
            new QPushButton(owner.tr("Create"), shader_template_row);
        create_fragment_shader->setObjectName(
            QStringLiteral("pelican.fullscreenPass.createFragmentShader"));
        open_existing_shader =
            new QPushButton(owner.tr("Open"), shader_template_row);
        open_existing_shader->setObjectName(
            QStringLiteral("pelican.fullscreenPass.openExistingShader"));
        open_existing_shader->setToolTip(owner.tr(
            "Uses the file that is already there and opens it; it is not "
            "overwritten."));
        open_existing_shader->hide();
        shader_template_notice = new QLabel(shader_template_row);
        shader_template_notice->setObjectName(
            QStringLiteral("pelican.fullscreenPass.shaderTemplateNotice"));
        applyPanelNoticeStyle(shader_template_notice);
        template_layout->addWidget(shader_template_name, 1);
        template_layout->addWidget(create_fragment_shader);
        template_layout->addWidget(open_existing_shader);
        template_layout->addWidget(shader_template_notice, 1);
        form->addRow(QString{}, shader_template_row);
        form->setRowVisible(shader_template_row, false);
        updateShaderTemplateAvailability();

        QObject::connect(new_fragment_shader, &QPushButton::clicked, &owner,
                         [this] { beginShaderTemplate(); });
        QObject::connect(create_fragment_shader, &QPushButton::clicked,
                         &owner, [this] { createShaderTemplate(); });
        QObject::connect(shader_template_name, &QLineEdit::returnPressed,
                         &owner, [this] { createShaderTemplate(); });
        QObject::connect(open_existing_shader, &QPushButton::clicked, &owner,
                         [this] { useExistingShaderTemplate(); });
        QObject::connect(shader_template_name, &QLineEdit::textChanged,
                         &owner, [this] {
                             open_existing_shader->hide();
                             setPanelNotice(shader_template_notice, {}, {});
                         });
    }

    void updateShaderTemplateAvailability() {
        new_fragment_shader->setEnabled(project_root.has_value());
        new_fragment_shader->setToolTip(
            project_root
                ? owner.tr("Writes a fragment shader for the current inputs "
                           "to shaders/<name>.frag, puts its stem here and "
                           "opens it.")
                : owner.tr("Open a project first: the shader is written "
                           "into its shaders folder."));
    }

    // The shader "New..." proposes: the pass's name. When the pass already
    // reads shaders/<that name>.frag -- New... on an existing pass, such as
    // the Blank pipeline's present -- the author is asking for a new shader
    // for it, and the one it has stays theirs: the kind's name is appended
    // (then _2, _3...) so the proposal is a file that is not there yet.
    QString proposedShaderName() const {
        const std::string base =
            suggestedShaderTemplateName(name->text().trimmed().toStdString());
        if (base.empty() || !project_root ||
            fragment_shader->text().trimmed() !=
                text(shaderTemplateReference(base)))
            return text(base);
        const auto kind = selectedTemplateKind();
        const std::string stem =
            kind == FullscreenShaderTemplateKind::pass_through
                ? base
                : base + "_" + std::string{fullscreenShaderTemplateKindName(kind)};
        std::string candidate = stem;
        std::error_code error;
        for (int index = 2; isValidShaderTemplateName(candidate) &&
                            std::filesystem::exists(
                                shaderTemplatePath(*project_root, candidate), error);
             ++index)
            candidate = stem + "_" + std::to_string(index);
        return isValidShaderTemplateName(candidate) ? text(candidate) : text(base);
    }

    void beginShaderTemplate() {
        if (!project_root) return;
        const QString proposed = proposedShaderName();
        {
            const QSignalBlocker blocker{shader_template_name};
            shader_template_name->setText(proposed);
        }
        open_existing_shader->hide();
        create_fragment_shader->show();
        setPanelNotice(shader_template_notice, {}, {});
        form->setRowVisible(shader_template_row, true);
        auto_shader_name = proposed;
        proposeTemplateNames();
        shader_template_name->setFocus();
        shader_template_name->selectAll();
    }

    std::vector<FullscreenShaderTemplateInput> templateInputs() const {
        std::vector<FullscreenShaderTemplateInput> result;
        for (const auto &authored : selectedSequence(*inputs)) {
            const auto reference =
                Pelican::parsePassShapeInputReference(authored);
            const auto found = resource_kinds.find(reference.name);
            result.push_back(FullscreenShaderTemplateInput{
                .resource = authored,
                .image = found == resource_kinds.end() ||
                         found->second !=
                             Pelican::PassShapeResourceKind::buffer,
            });
        }
        return result;
    }

    void templateNotice(const QString &shown, const QString &detail,
                        bool failure) {
        shader_template_notice->setStyleSheet(
            failure ? QStringLiteral("color: #d94c3d;") : QString{});
        setPanelNotice(shader_template_notice, shown, detail);
    }

    void openShaderFile(const std::filesystem::path &path,
                        const QString &reference) {
        const bool opened =
            shader_open_action && shader_open_action(path);
        if (!opened) {
            templateNotice(owner.tr("could not open %1").arg(reference),
                           owner.tr("The desktop could not open %1.")
                               .arg(QString::fromStdWString(path.wstring())),
                           true);
        }
    }

    void createShaderTemplate() {
        if (!project_root) return;
        const auto shader_name =
            shader_template_name->text().trimmed().toStdString();
        if (!isValidShaderTemplateName(shader_name)) {
            templateNotice(owner.tr("letters, digits, _ and - only"),
                           owner.tr("A shader name is letters, digits, '_' "
                                    "and '-', so it can only land in the "
                                    "project's shaders folder."),
                           true);
            return;
        }
        try {
            const auto pass_name = name->text().trimmed().isEmpty()
                                       ? shader_name
                                       : name->text().trimmed().toStdString();
            const auto inputs_now = templateInputs();
            const auto written = writeShaderTemplate(
                *project_root, shader_name,
                fullscreenFragmentShaderTemplate(pass_name, inputs_now,
                                                 selectedTemplateKind()));
            const QString file =
                QStringLiteral("shaders/%1.frag").arg(text(shader_name));
            if (!written.created) {
                // The author's file is theirs. Offer it; do not replace it.
                open_existing_shader->show();
                templateNotice(owner.tr("%1 already exists").arg(file),
                               owner.tr("%1 is already in the project and was "
                                        "left as it is. Open uses it for this "
                                        "pass.")
                                   .arg(file),
                               true);
                return;
            }
            fragment_shader->setText(text(written.reference));
            templateNotice(owner.tr("created %1").arg(file),
                           owner.tr("Wrote %1 with %2 input(s) and put %3 in "
                                    "the fragment field.")
                               .arg(file)
                               .arg(static_cast<qulonglong>(inputs_now.size()))
                               .arg(text(written.reference)),
                           false);
            openShaderFile(written.path, text(written.reference));
        } catch (const std::exception &error) {
            templateNotice(owner.tr("could not write the shader"),
                           QString::fromUtf8(error.what()), true);
        }
    }

    void useExistingShaderTemplate() {
        if (!project_root) return;
        const auto shader_name =
            shader_template_name->text().trimmed().toStdString();
        if (!isValidShaderTemplateName(shader_name)) return;
        const auto path = shaderTemplatePath(*project_root, shader_name);
        const QString reference = text(shaderTemplateReference(shader_name));
        fragment_shader->setText(reference);
        open_existing_shader->hide();
        templateNotice(owner.tr("using %1").arg(reference), {}, false);
        openShaderFile(path, reference);
    }

    // --- G-3: the pass's settings, and the kind of shader "New..." writes.

    void buildPassSettings(QGridLayout &target_layout) {
        interface_row = new FullscreenPassInterfaceRow(&owner);
        interface_row->setChangedAction([this] { updateDraft(); });
        form->addRow(owner.tr("Frame data"), interface_row);

        output_ops = new FullscreenOutputOpsEditor(targets);
        output_ops->setJsonWriter([this](const QString &json_text) {
            color_attachment_references->setPlainText(json_text);
        });
        output_ops->setChangedAction([this] { updateDraft(); });
        target_layout.addWidget(output_ops, 9, 0, 1, 3);

        input_roles = new QLabel(targets);
        input_roles->setObjectName(
            QStringLiteral("pelican.fullscreenPass.inputRoles"));
        applyPanelNoticeStyle(input_roles);
        input_roles->setWordWrap(true);
        input_roles->hide();
        target_layout.addWidget(input_roles, 10, 0, 1, 3);

        // Almost every fullscreen pass draws the engine's triangle, so a new
        // draft starts with it rather than an empty field to type it into.
        vertex_shader->setText(QString::fromLatin1(DefaultFullscreenVertex));

        shader_template_kind = new QComboBox(shader_template_row);
        shader_template_kind->setObjectName(
            QStringLiteral("pelican.fullscreenPass.shaderTemplateKind"));
        for (const auto kind : FullscreenShaderTemplateKinds)
            shader_template_kind->addItem(
                text(fullscreenShaderTemplateKindLabel(kind)),
                text(fullscreenShaderTemplateKindName(kind)));
        shader_template_kind->setToolTip(owner.tr(
            "What the new shader starts as. Deferred lighting also turns on "
            "Uses scene lights and Camera position."));
        qobject_cast<QHBoxLayout *>(shader_template_row->layout())
            ->insertWidget(0, shader_template_kind);
        QObject::connect(shader_template_kind,
                         qOverload<int>(&QComboBox::currentIndexChanged),
                         &owner, [this] { applyTemplateKind(); });
    }

    static constexpr const char *DefaultFullscreenVertex = "engine://fullscreen";

    FullscreenShaderTemplateKind selectedTemplateKind() const {
        return fullscreenShaderTemplateKindFromName(
                   shader_template_kind->currentData().toString().toStdString())
            .value_or(FullscreenShaderTemplateKind::pass_through);
    }

    // A kind whose shader needs the scene lights or the camera sets them on
    // the pass as soon as it is chosen, and the inputs say what it reads.
    void applyTemplateKind() {
        const auto settings =
            fullscreenShaderTemplatePassSettings(selectedTemplateKind());
        interface_row->require(settings.uses_light_data,
                               settings.push_constants);
        proposeTemplateNames();
    }

    // A pass name that is not in the graph yet: the kind's own, then _2, _3...
    QString unusedPassName(FullscreenShaderTemplateKind kind) const {
        const std::string base =
            kind == FullscreenShaderTemplateKind::pass_through
                ? std::string{"fullscreen_pass"}
                : std::string{fullscreenShaderTemplateKindName(kind)};
        const auto taken = [&](const std::string &candidate) {
            if (!plan) return false;
            if (std::ranges::find(plan->nodes, candidate, &FramePlanNode::name) !=
                plan->nodes.end())
                return true;
            const auto graph_passes = authored_passes.find(plan->graph);
            return graph_passes != authored_passes.end() &&
                   graph_passes->second.contains(candidate);
        };
        std::string candidate = base;
        for (int index = 2; taken(candidate); ++index)
            candidate = base + "_" + std::to_string(index);
        return text(candidate);
    }

    // "New..." names what it makes: an empty pass name (or one it filled in
    // itself) becomes the kind's, and the shader is named after the pass.
    void proposeTemplateNames() {
        if (!editing_preset_pass) {
            const QString current = name->text().trimmed();
            if (current.isEmpty() || current == auto_pass_name) {
                auto_pass_name = unusedPassName(selectedTemplateKind());
                name->setText(auto_pass_name);
            }
        }
        const QString shader_current = shader_template_name->text().trimmed();
        if (shader_current.isEmpty() || shader_current == auto_shader_name) {
            auto_shader_name = proposedShaderName();
            const QSignalBlocker blocker{shader_template_name};
            shader_template_name->setText(auto_shader_name);
        }
        refreshInputRoles();
    }

    void refreshInputRoles() {
        const auto roles = form->isRowVisible(shader_template_row)
                               ? fullscreenShaderTemplateInputRoles(
                                     selectedTemplateKind())
                               : std::span<const FullscreenShaderTemplateRole>{};
        QStringList named;
        for (std::size_t index = 0; index < roles.size(); ++index)
            named.push_back(QStringLiteral("%1 %2%3")
                                .arg(index)
                                .arg(text(roles[index].name))
                                .arg(roles[index].optional
                                         ? owner.tr(" (optional)")
                                         : QString{}));
        setPanelNotice(input_roles,
                       named.isEmpty() ? QString{}
                                       : owner.tr("inputs: %1")
                                             .arg(named.join(
                                                 QStringLiteral(", "))),
                       owner.tr("The template reads the inputs by position."));
        for (int row = 0; row < inputs->count(); ++row)
            inputs->item(row)->setToolTip(
                static_cast<std::size_t>(row) < roles.size()
                    ? text(roles[static_cast<std::size_t>(row)].name)
                    : QString{});
    }

    void syncPassSettings() {
        output_ops->sync(selectedSequence(*colors),
                         color_attachment_references->toPlainText());
        if (color_attachment_references->isHidden()) output_ops->hide();
        refreshInputRoles();
    }

    void resetPassSettings() {
        // clearDraft emptied it; a pass being edited gets its own back from
        // its declaration right after.
        vertex_shader->setText(QString::fromLatin1(DefaultFullscreenVertex));
        auto_pass_name.clear();
        auto_shader_name.clear();
        interface_row->reset();
        output_ops->reset();
        const QSignalBlocker blocker{shader_template_kind};
        shader_template_kind->setCurrentIndex(0);
        refreshInputRoles();
    }

    // The output.color JSON follows the colour list: an output that stays
    // keeps its attachment fields, a new one is a plain name.
    void reconcileColorAttachments() {
        const auto current = color_attachment_references->toPlainText();
        const auto next = FullscreenOutputOpsEditor::reconciledJson(
            selectedSequence(*colors), current);
        if (next == current) return;
        const QSignalBlocker blocker{color_attachment_references};
        color_attachment_references->setPlainText(next);
    }

    // What edit_preset_pass sends for a fullscreen pass besides its input,
    // output and shader: the interface fields and clear_colors, each only
    // when it changed.
    void addPassSettingsEditParams(const Json &draft, Json &params) const {
        interface_row->addEditParams(original_preset_pass, params);
        const auto clear = draft.value("clear_colors", Json(nullptr));
        if (clear != original_preset_pass.value("clear_colors", Json(nullptr)))
            params["clear_colors"] = clear;
    }

    // A rejected candidate. A shader that did not compile is shown whole,
    // under the form, and the form keeps every value it holds; the notice row
    // says only that it happened.
    void rejectAuthoring(const Json &response) {
        const QString message = responseError(response);
        if (compile_log->showMessage(message)) {
            authoringError(owner.tr("shader compile failed; see the log "
                                    "below"));
            return;
        }
        compile_log->clear();
        authoringError(message);
    }

    bool preservesPresetDeclaration() const {
        const auto pass_type = original_preset_pass.value("type", std::string{});
        // A pass whose declaration was refused has none to preserve (F-4: the
        // draft used to read a type out of the empty object and show that
        // failure in place of the reason).
        return editing_preset_pass && !pass_type.empty() &&
               pass_type != "fullscreen";
    }

    bool editingAuxiliaryPass() const {
        const auto kind = original_preset_pass.value("type", std::string{});
        return editing_preset_pass && (kind == "debug_draw" || kind == "debug_text" || kind == "gizmo" ||
            kind == "shadow_depth" || kind == "velocity" || kind == "picking");
    }

    bool editingShadowPass() const {
        return editing_preset_pass && original_preset_pass.value("type", std::string{}) == "shadow_depth";
    }

    bool editsSkinnedShader() const {
        const auto kind = original_preset_pass.value("type", std::string{});
        return editing_preset_pass && (kind == "velocity" || kind == "picking");
    }

    bool editingRasterPass() const {
        return editing_preset_pass && original_preset_pass.value("type", std::string{}) == "raster";
    }

    bool editingMaterialPass() const {
        return editing_preset_pass && original_preset_pass.value("type", std::string{}) == "material";
    }

    bool editingFeaturePass() const {
        return editing_preset_pass && feature_pass_info.contains(*editing_preset_pass) &&
            feature_pass_info.at(*editing_preset_pass).value("editable_feature_pass", false);
    }

    // WP334b: the save button names the engine domain operation it calls, in
    // every draft state.  Per-draft notes are appended to this line, never
    // written over it.
    QString authoredPassSaveToolTip() const {
        return owner.tr(
            "Calls add_authored_pass with the current engine digest and "
            "anchor.");
    }

    // F-6: a pass the direct root declares; edit_preset_pass then takes no
    // document digest besides base_source_digest.
    bool editingRootPass() const {
        return editing_preset_pass && root_passes.contains(*editing_preset_pass);
    }

    // F-6: Save names the file it writes, which for a root pass is the root.
    void showRootPassSave() {
        save->setText(owner.tr("Save"));
        save->setToolTip(
            owner.tr("Calls edit_preset_pass on the pass %1 declares.")
                .arg(root_reference.empty()
                         ? owner.tr("the project's rendering config")
                         : text(root_reference)));
    }

    // Only a preset owns a material_routing edit_preset_pass writes.
    bool editsPresetRouting() const {
        return !editingFeaturePass() && !editingRootPass();
    }

    std::string passSourceDigest() const {
        if (editingFeaturePass())
            return feature_pass_info.at(*editing_preset_pass).value("feature_source_digest", Json::object()).value("hex", std::string{});
        if (editingRootPass()) return source_digest;
        return preset_digest;
    }

    static constexpr std::array materialSettingKeys{
        "material_range", "material_filter", "material_variant", "gpu_draw_source",
        "material_outputs", "material_output_states", "screen_inputs", "surface_resources", "material_resources"};

    Json routingDraft() const {
        const auto bytes = material_routing->toPlainText().trimmed();
        auto result = bytes.isEmpty() ? Json(nullptr) : Json::parse(bytes.toStdString());
        if (!result.is_null() && !result.is_object())
            throw std::runtime_error("Material routing must be a JSON object or blank.");
        return result;
    }

    void showPresetFields(bool preserve_declaration) {
        type->setText(preserve_declaration ? text(original_preset_pass.at("type").get<std::string>()) : QStringLiteral("fullscreen"));
        for (auto *field : std::array<QWidget *, 5>{position, location, vertex_shader, fragment_row, raster_state})
            form->setRowVisible(field, !preserve_declaration);
        if (editingRasterPass()) {
            for (auto *field : std::array<QWidget *, 3>{vertex_shader, fragment_row, raster_state})
                form->setRowVisible(field, true);
        }
        form->setRowVisible(draw, editingRasterPass());
        if (editingAuxiliaryPass()) {
            form->setRowVisible(vertex_shader, true);
            form->setRowVisible(fragment_row, !editingShadowPass());
        }
        form->setRowVisible(shader_template_row, false);
        form->setRowVisible(interface_row, !preserve_declaration);
        // The stem field is inside the row now; it still says for itself
        // whether it is shown.
        fragment_shader->setHidden(!form->isRowVisible(fragment_row));
        form->setRowVisible(skinned_vertex_shader, editsSkinnedShader());
        auto *target_grid = qobject_cast<QGridLayout *>(targets->layout());
        const std::array visible_columns{!editingAuxiliaryPass(), !editingShadowPass(),
            !editingAuxiliaryPass() || editingShadowPass() || editsSkinnedShader()};
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 6; ++row)
                if (auto *item = target_grid->itemAtPosition(row, column); item && item->widget())
                    item->widget()->setVisible(visible_columns[column]);
        for (int row : {6, 7})
            target_grid->itemAtPosition(row, 0)->widget()->setVisible(!editingShadowPass());
        for (auto *field : std::array<QWidget *, 3>{material_contract, material_settings, material_routing})
            form->setRowVisible(field, editingMaterialPass());
        form->setRowVisible(material_routing, editingMaterialPass() && editsPresetRouting());
        targets->setVisible(true);
        validation->setVisible(true);
    }

    Json rayQueryDeclaration() const {
        if (!ray_query->isChecked()) return false;
        const auto selected_scene = ray_query_scene->text().trimmed();
        if (selected_scene.isEmpty()) throw std::runtime_error("Scene acceleration structure name must not be empty.");
        return {{"acceleration_structure", selected_scene.toStdString()}};
    }

    Json draftJson() const {
        const auto selected_colors = selectedSequence(*colors);
        Json color_output = selected_colors;
        const auto attachment_text =
            color_attachment_references->toPlainText().trimmed();
        if (!attachment_text.isEmpty()) {
            Json encoded = Json::parse(attachment_text.toUtf8().constData());
            const Json entries = encoded.is_array()
                                     ? encoded
                                     : Json::array({encoded});
            if (entries.size() != selected_colors.size()) {
                throw std::runtime_error(
                    "output.color attachment JSON count must match the "
                    "selected color output count");
            }
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const Json &entry = entries.at(index);
                std::string target;
                if (entry.is_string()) {
                    target = entry.get<std::string>();
                } else if (entry.is_object() && entry.contains("target") &&
                           entry.at("target").is_string()) {
                    target = entry.at("target").get<std::string>();
                } else {
                    throw std::runtime_error(
                        "each output.color attachment JSON entry requires a "
                        "string target");
                }
                if (target != selected_colors.at(index)) {
                    throw std::runtime_error(
                        "output.color attachment JSON targets must match the "
                        "selected color outputs in order");
                }
            }
            color_output = std::move(encoded);
        }

        const bool preserve_declaration = preservesPresetDeclaration();
        Json draft = preserve_declaration ? original_preset_pass : Json::object();
        draft["name"] = name->text().toStdString();
        draft["type"] = preserve_declaration ? original_preset_pass.at("type") : Json("fullscreen");
        if (preserve_declaration || ray_query->isChecked()) {
            draft["ray_query"] = rayQueryDeclaration();
        }
        const auto selected_inputs = selectedSequence(*inputs);
        if (!editingAuxiliaryPass() && (!preserve_declaration || draft.contains("input") || !selected_inputs.empty()))
            draft["input"] = selected_inputs;
        auto output = preserve_declaration ? draft.value("output", Json::object()) : Json::object();
        output["color"] = std::move(color_output);
        if (editingShadowPass() && selected_colors.empty()) output["color"] = nullptr;
        if (depth->currentIndex() >= 0) {
            auto selected_depth = output.value("depth", Json(nullptr));
            if (selected_depth.is_object()) selected_depth["target"] = depth->currentText().toStdString();
            else selected_depth = depth->currentText().toStdString();
            output["depth"] = std::move(selected_depth);
        } else if (!preserve_declaration || output.contains("depth")) {
            output["depth"] = nullptr;
        }
        draft["output"] = std::move(output);
        // The ownership authority, rather than a Studio-maintained field set,
        // decides whether this type may emit its shader editor value.
        if (!preserve_declaration && fullscreenOwns("shader")) {
            draft["shader"] = Json{
                {"vertex", vertex_shader->text().toStdString()},
                {"fragment", fragment_shader->text().toStdString()},
            };
        }
        const auto raster_text =
            raster_state->toPlainText().trimmed();
        if (!preserve_declaration && fullscreenOwns("raster_state") &&
            !raster_text.isEmpty()) {
            draft["raster_state"] = Json::parse(
                raster_text.toUtf8().constData());
        }
        if (editingRasterPass()) {
            auto shader = draft.value("shader", Json::object());
            shader["vertex"] = vertex_shader->text().toStdString();
            shader["fragment"] = fragment_shader->text().toStdString();
            draft["shader"] = std::move(shader);
            draft["draw"] = Json::parse(draw->toPlainText().toStdString());
            if (raster_text.isEmpty()) draft.erase("raster_state");
            else draft["raster_state"] = Json::parse(raster_text.toStdString());
        }
        if (editingAuxiliaryPass()) {
            const auto default_shader = "engine://" + draft.at("type").get<std::string>();
            auto shader = draft.value("shader", Json::object());
            const auto vertex = vertex_shader->text().trimmed().toStdString();
            const auto fragment = fragment_shader->text().trimmed().toStdString();
            const auto skinned = skinned_vertex_shader->text().trimmed().toStdString();
            const bool changed = draft.contains("shader") || vertex != default_shader ||
                (!editingShadowPass() && fragment != default_shader) || (editsSkinnedShader() && !skinned.empty());
            if (changed) {
                shader["vertex"] = vertex;
                if (!editingShadowPass()) shader["fragment"] = fragment;
                if (editsSkinnedShader()) {
                    if (skinned.empty()) shader.erase("skinned_vertex");
                    else shader["skinned_vertex"] = skinned;
                }
                draft["shader"] = std::move(shader);
            }
        }
        if (editingMaterialPass()) {
            const auto bytes = material_settings->toPlainText().trimmed();
            const auto settings = bytes.isEmpty() ? Json::object() : Json::parse(bytes.toStdString());
            if (!settings.is_object()) throw std::runtime_error("Material settings must be a JSON object.");
            for (const auto &[key, value] : settings.items())
                if (std::find(materialSettingKeys.begin(), materialSettingKeys.end(), key) == materialSettingKeys.end())
                    throw std::runtime_error("Unknown material setting: " + key);
            for (const auto *key : materialSettingKeys) {
                if (settings.contains(key) && !settings.at(key).is_null()) draft[key] = settings.at(key);
                else draft.erase(key);
            }
            const auto contract = material_contract->currentData().toString();
            if (contract.isEmpty()) draft.erase("material_contract");
            else draft["material_contract"] = contract.toStdString();
            if (editsPresetRouting()) (void)routingDraft();
        }
        if (!preserve_declaration) {
            interface_row->apply(draft);
            if (auto clear = output_ops->clearColors(); !clear.is_null())
                draft["clear_colors"] = std::move(clear);
        }
        return draft;
    }

    // The short word an axis wears on the panel. The full name and the
    // sentence stay in the tooltip, and in the summary when the axis is the
    // reason a draft cannot be copied.
    static QString axisWord(const QString &axis) {
        static const std::map<QString, QString> words{
            {QStringLiteral("Field ownership"), QStringLiteral("fields")},
            {QStringLiteral("Same-graph name collision"),
             QStringLiteral("name")},
            {QStringLiteral("Target names"), QStringLiteral("targets")},
            {QStringLiteral("Pass shape"), QStringLiteral("shape")},
            {QStringLiteral("History input support"),
             QStringLiteral("history")},
            {QStringLiteral("Target usage compatibility"),
             QStringLiteral("usage")},
            {QStringLiteral("Input generation order"),
             QStringLiteral("order")},
            {QStringLiteral("Shader stem resolution"),
             QStringLiteral("shader")},
        };
        const auto found = words.find(axis);
        return found == words.end() ? axis : found->second;
    }

    void setAxis(QLabel &label, const QString &axis, const char *state,
                 const QString &detail) {
        const QString named = QString::fromLatin1(state);
        const QString state_text =
            named == QStringLiteral("valid")
                ? owner.tr("Valid")
                : named == QStringLiteral("invalid")
                      ? owner.tr("Invalid")
                      : owner.tr("Not checked");
        const QString mark = named == QStringLiteral("valid")
                                 ? QStringLiteral("✓")
                                 : named == QStringLiteral("invalid")
                                       ? QStringLiteral("✗")
                                       : QStringLiteral("–");
        label.setProperty("pelicanValidationState", named);
        label.setText(QStringLiteral("%1 %2").arg(mark, axisWord(axis)));
        label.setToolTip(owner.tr("%1: %2 — %3")
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
        // N-7 rule 2: "nothing is known yet" is not something to act on, so
        // the row is not there. The reason is still one hover away.
        if (!authoring_available) {
            setPanelNotice(omitted, {},
                           owner.tr("Omitted keys: not known because no "
                                    "authored declaration is available. The "
                                    "JSON still contains only fields "
                                    "represented by this form."));
            return;
        }
        if (!plan) {
            setPanelNotice(omitted, {},
                           owner.tr("Omitted keys: not known until a target "
                                    "graph is loaded. The JSON still contains "
                                    "only fields represented by this form."));
            return;
        }
        const auto graph_found = authored_passes.find(plan->graph);
        const auto pass_found =
            graph_found == authored_passes.end()
                ? std::map<std::string, Json, std::less<>>::const_iterator{}
                : graph_found->second.find(name->text().toStdString());
        if (graph_found == authored_passes.end() ||
            pass_found == graph_found->second.end()) {
            setPanelNotice(omitted, {},
                           owner.tr("Omitted keys: no existing declaration "
                                    "has this graph/name. This is a new "
                                    "partial draft and only represented "
                                    "fields are emitted."));
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
        setPanelNotice(
            omitted,
            missing.empty() ? QString{}
                            : owner.tr("omitted: %1")
                                  .arg(reported.join(QStringLiteral(", "))),
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
            (editingMaterialPass() ||
             (!vertex_shader->text().trimmed().isEmpty() &&
              (editingShadowPass() || !fragment_shader->text().trimmed().isEmpty())));
        // N-7 rule 2: one line, and only when there is something to do about
        // it. The sentence that says why is the tooltip; a draft with nothing
        // wrong says nothing at all.
        if (!invalid.isEmpty()) {
            validation_summary->setProperty(
                "pelicanValidationSummaryState",
                QStringLiteral("invalid"));
            validation_summary->setStyleSheet(
                QStringLiteral("color: #d94c3d;"));
            setPanelNotice(
                validation_summary,
                owner.tr("cannot copy: %1")
                    .arg(invalid.join(QStringLiteral(", "))),
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
            setPanelNotice(
                validation_summary, owner.tr("incomplete draft"),
                preservesPresetDeclaration() && !editingRasterPass()
                    ? owner.tr("Draft is incomplete; load a frame plan and "
                               "provide a name before copying.")
                    : owner.tr("Draft is incomplete; load a frame plan and "
                               "provide a name and both shader stems before "
                               "copying."));
            copy_json->setEnabled(false);
            return;
        }
        if (!not_checked.isEmpty()) {
            validation_summary->setProperty(
                "pelicanValidationSummaryState",
                QStringLiteral("partial"));
            validation_summary->setStyleSheet(
                QStringLiteral("color: #b36b00;"));
            QStringList short_names;
            for (const QString &axis_name : not_checked)
                short_names.push_back(axisWord(axis_name));
            setPanelNotice(
                validation_summary,
                owner.tr("partial: %1")
                    .arg(short_names.join(QStringLiteral(", "))),
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
        setPanelNotice(
            validation_summary, {},
            owner.tr("All displayed validation axes are Valid; copying is "
                     "available."));
        copy_json->setEnabled(true);
    }

    void updateDraft() {
        if (updating) {
            return;
        }
        syncPassSettings();
        Json draft;
        try {
            draft = draftJson();
        } catch (const std::exception &error) {
            json->setPlainText(
                owner.tr("Invalid authored JSON: %1")
                    .arg(QString::fromUtf8(error.what())));
            setAxis(*field_ownership, owner.tr("Field ownership"),
                    "invalid",
                    owner.tr("An authored JSON field is invalid: %1")
                        .arg(QString::fromUtf8(error.what())));
            updateValidationSummary();
            updateAuthoringButtons();
            return;
        }
        json->setPlainText(QString::fromStdString(draft.dump(2)));
        const QString graph_name =
            plan ? text(plan->graph) : owner.tr("(no frame plan)");
        graph->setText(graph_name);
        setPanelNotice(
            location,
            plan ? owner.tr("%1 passes[%2]")
                       .arg(graph_name)
                       .arg(position->value())
                 : QStringLiteral("—"),
            plan ? owner.tr("Graph '%1', passes[%2] (declaration_index %2)")
                       .arg(graph_name)
                       .arg(position->value())
                 : owner.tr("Load a frame plan to name the graph and "
                            "declaration_index."));

        try {
            (void)Pelican::validatePassFieldOwnership(
                draft, Pelican::PassFieldOwnershipCapabilities{},
                "pass editor draft");
            if (!preservesPresetDeclaration() && draft.contains("raster_state")) {
                (void)Pelican::parseFullscreenRasterFixedFunctionState(
                    draft, "Fullscreen pass '" +
                               draft.at("name").get<std::string>() + "'");
            }
            if (editingRasterPass()) {
                const auto &output = draft.at("output");
                const auto &color = output.at("color");
                (void)Pelican::parseRasterPassContract(draft,
                    color.is_array() ? color.size() : 1,
                    output.contains("depth") && !output.at("depth").is_null(), "raster pass editor");
            }
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
            auto shape_draft = draft;
            if (!shape_draft.at("output").contains("depth")) shape_draft["output"]["depth"] = nullptr;
            const auto violations = Pelican::evaluatePassShape(
                shape_policy,
                Pelican::passShapeObservationFromJson(
                    Pelican::validatePassFieldOwnership(shape_draft, {}, "pass editor draft"),
                    shape_draft, non_image_inputs));
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
            const bool collision = !editing_preset_pass && (authored_collision ||
                                   runtime_collision != plan->nodes.end());
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
            source_digest.size() == 64 &&
            (editing_preset_pass ? bool(authoring.editPresetPass) && passSourceDigest().size() == 64 && !load_edit_requested &&
                preset_passes.contains(*editing_preset_pass) : selected_anchor) &&
            (validation == QStringLiteral("valid") ||
             validation == QStringLiteral("partial")));
        remove_authored->setEnabled(
            authoring_rpc_available && authoring_available && idle &&
            source_digest.size() == 64 &&
            remove_fragment->currentIndex() >= 0);
    }

    void authoringError(const QString &message, const QString &line = {}) {
        authoring_status->setStyleSheet(
            QStringLiteral("color: #d94c3d;"));
        setPanelNotice(authoring_status,
                       line.isEmpty() ? owner.tr("authoring failed") : line,
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
        setPanelNotice(
            authoring_status, owner.tr("requesting context..."),
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
        if (editing_preset_pass) { requestEditPresetPass(); return; }
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
        setPanelNotice(
            authoring_status, owner.tr("submitting..."),
            owner.tr("Submitting the complete authored-pass candidate..."));
        updateAuthoringButtons();
    }

    void requestEditPresetPass() {
      if (!authoring.editPresetPass || !editing_preset_pass)
        return;
      try {
        auto draft = draftJson();
        Json params{{"base_source_digest", source_digest},
                    {"graph", editing_preset_pass->first},
                    {"pass_name", editing_preset_pass->second},
                    {"ray_query", rayQueryDeclaration()}};
        // F-6: the root's own pass is named by base_source_digest alone.
        if (!editingRootPass())
            params[editingFeaturePass() ? "base_feature_digest" : "base_preset_digest"] = passSourceDigest();
        if (preservesPresetDeclaration()) {
            if (editingMaterialPass()) {
                auto settings = Json::object();
                const auto collect = [&](const char *key) {
                    if (draft.contains(key)) {
                        if (!original_preset_pass.contains(key) || draft.at(key) != original_preset_pass.at(key)) settings[key] = draft.at(key);
                    } else if (original_preset_pass.contains(key)) settings[key] = nullptr;
                };
                for (const auto *key : materialSettingKeys) collect(key);
                collect("material_contract");
                if (!settings.empty()) params["material_settings"] = std::move(settings);
                if (editsPresetRouting()) {
                    const auto routing = routingDraft();
                    if (routing != preset_material_routing) params["material_routing"] = routing;
                }
            }
            for (const auto *key : {"input", "output", "shader", "draw", "raster_state"}) {
                if (draft.contains(key) && (!original_preset_pass.contains(key) || draft.at(key) != original_preset_pass.at(key)))
                    params[key] = draft.at(key);
            }
            if (editingRasterPass() && original_preset_pass.contains("raster_state") && !draft.contains("raster_state"))
                params["raster_state"] = nullptr;
            if (editsSkinnedShader() && original_preset_pass.value("shader", Json::object()).contains("skinned_vertex") &&
                !draft.value("shader", Json::object()).contains("skinned_vertex"))
                params["shader"]["skinned_vertex"] = nullptr;
        } else {
            auto output = original_preset_pass.value("output", Json::object());
            output["color"] = draft.at("output").at("color");
            output["depth"] = draft.at("output").at("depth");
            auto shader = original_preset_pass.value("shader", Json::object());
            shader["vertex"] = draft.at("shader").at("vertex");
            shader["fragment"] = draft.at("shader").at("fragment");
            params["input"] = draft.at("input");
            params["output"] = std::move(output);
            params["shader"] = std::move(shader);
            addPassSettingsEditParams(draft, params);
        }
        QString error;
        pending_authoring_kind = AuthoringRequest::edit;
        pending_authoring_request = authoring.editPresetPass(
            QJsonDocument::fromJson(QByteArray::fromStdString(params.dump()))
                .object(),
            &error);
        if (!pending_authoring_request)
          authoringError(error);
        updateAuthoringButtons();
      } catch (const std::exception &error) {
        authoringError(QString::fromUtf8(error.what()));
      }
    }

    void beginPresetPassEdit(const QString &graph_name, const QString &pass_name,
                             std::optional<QStringList> outputs) {
        if (pending_authoring_request || !pending_ticket.isEmpty()) {
            authoringError(owner.tr("Finish the current request before selecting a pass."));
            return;
        }
        // F-7: the form belongs to the pass asked for from here on. If the
        // engine's context refuses it, the refusal is all the form shows --
        // not the previous pass's draft under the new pass's refusal.
        clearDraft();
        input_filter = std::move(outputs);
        requested_insert_after.reset();
        populateRoleCandidates();
        editing_preset_pass = std::pair{graph_name.toStdString(), pass_name.toStdString()};
        load_edit_requested = true;
        authoring_available = false;
        requestAuthoringContext();
        requestRefresh();
    }

    void loadPresetPass() {
      if (!load_edit_requested || !editing_preset_pass || !plan ||
          !authoring_available || pending_request != 0)
        return;
      if (plan->graph != editing_preset_pass->first) {
        authoringError(owner.tr("The selected graph is no longer active."));
        return;
      }
      if (!preset_passes.contains(*editing_preset_pass)) {
        const auto found = feature_pass_info.find(*editing_preset_pass);
        if (config_kind == "direct" &&
            root_declared_passes.contains(*editing_preset_pass)) {
          // F-4: a direct root has no preset -- there is nothing to fork.
          // F-6: the passes it declares are edited in it (root_passes); this
          // is a declaration the engine did not offer as editable.
          authoringError(
              owner.tr("%1 is declared in the project's own rendering "
                       "config%2, which selects no preset, and the engine "
                       "offers no editable declaration of it. Edit that "
                       "file by hand.")
                  .arg(text(editing_preset_pass->second),
                       root_reference.empty()
                           ? QString{}
                           : QStringLiteral(" (%1)").arg(text(root_reference))),
              owner.tr("%1: not editable in a direct root")
                  .arg(text(editing_preset_pass->second)));
          return;
        }
        authoringError(found != feature_pass_info.end()
            ? text(found->second.value("feature_edit_reason", std::string{"This feature node has no editable project pass declaration."}))
            : owner.tr("This node has no editable project preset or feature pass declaration."));
        return;
      }
      const auto &declaration = preset_passes.at(*editing_preset_pass);
      const bool preserve_declaration = declaration.value("type", std::string{}) != "fullscreen";
      const auto output = declaration.value("output", Json::object());
      if (!preserve_declaration && output.contains("depth") && !output.at("depth").is_null() &&
          !output.at("depth").is_string()) {
        authoringError(
            owner.tr("This depth attachment requires JSON editing."));
        return;
      }
      clearDraft();
      updating = true;
      original_preset_pass = declaration;
      showPresetFields(preserve_declaration);
      if (editingMaterialPass()) {
        material_contract->setCurrentIndex(material_contract->findData(text(declaration.value("material_contract", std::string{}))));
        auto settings = Json::object();
        for (const auto *key : materialSettingKeys)
            if (declaration.contains(key)) settings[key] = declaration.at(key);
        material_settings->setPlainText(text(settings.dump(2)));
        material_routing->setPlainText(preset_material_routing.is_null() ? QString{} : text(preset_material_routing.dump(2)));
      }
      const auto query = declaration.value("ray_query", Json(false));
      ray_query->setChecked(query.is_object() || (query.is_boolean() && query.get<bool>()));
      ray_query_scene->setText(query.is_object()
          ? text(query.at("acceleration_structure").get<std::string>()) : QStringLiteral("scene"));
      name->setText(text(editing_preset_pass->second));
      name->setReadOnly(true);
      position->setEnabled(false);
      raster_state->setEnabled(editingRasterPass());
      if (!preserve_declaration || editingRasterPass()) {
        vertex_shader->setText(text(declaration.at("shader").at("vertex").get<std::string>()));
        fragment_shader->setText(text(declaration.at("shader").at("fragment").get<std::string>()));
      }
      if (editingRasterPass()) {
        draw->setPlainText(text(declaration.at("draw").dump(2)));
        if (declaration.contains("raster_state")) raster_state->setPlainText(text(declaration.at("raster_state").dump(2)));
      }
      if (editingAuxiliaryPass()) {
        const auto shader = declaration.value("shader", Json::object());
        const auto default_shader = "engine://" + declaration.at("type").get<std::string>();
        vertex_shader->setText(text(shader.value("vertex", default_shader)));
        fragment_shader->setText(text(shader.value("fragment", editingShadowPass() ? std::string{} : default_shader)));
        skinned_vertex_shader->setText(text(shader.value("skinned_vertex", std::string{})));
      }
      for (const auto &input : declaration.value("input", Json::array()))
        inputs->addItem(text(input.get<std::string>()));
      const auto color = output.value("color", Json::array());
      const auto entries = color.is_null() ? Json::array() : color.is_array() ? color : Json::array({color});
      for (const auto &entry : entries)
        colors->addItem(text(entry.is_string()
                                 ? entry.get<std::string>()
                                 : entry.at("target").get<std::string>()));
      if (std::any_of(entries.begin(), entries.end(),
                      [](const auto &entry) { return entry.is_object(); }))
        color_attachment_references->setPlainText(text(color.dump()));
      if (output.contains("depth") && !output.at("depth").is_null()) {
        const auto &encoded = output.at("depth");
        const auto target = text(encoded.is_string() ? encoded.get<std::string>() : encoded.at("target").get<std::string>());
        if (depth->findText(target) < 0)
          depth->addItem(target);
        depth->setCurrentText(target);
      }
      if (!preserve_declaration) interface_row->load(declaration);
      output_ops->loadClearColors(declaration);
      load_edit_requested = false;
      updating = false;
      save->setText(editingFeaturePass() ? owner.tr("Save feature") : owner.tr("Save preset"));
      save->setToolTip(editingFeaturePass()
          ? authoredPassSaveToolTip() + QStringLiteral("\n") +
                owner.tr("Updates %1 wherever this feature is used.").arg(text(feature_pass_info.at(*editing_preset_pass).value("feature_source_reference", std::string{})))
          : authoredPassSaveToolTip());
      if (editingRootPass()) showRootPassSave();
      updateDraft();
    }

    void requestRemoveAuthoredPass() {
      if (!remove_authored->isEnabled())
        return;
      const auto reference = remove_fragment->currentData().toString();
      if (reference.isEmpty())
        return;
      const QJsonObject params{
          {QStringLiteral("base_source_digest"),
           QString::fromStdString(source_digest)},
          {QStringLiteral("fragment_reference"), reference},
      };
      QString error;
      pending_authoring_kind = AuthoringRequest::remove;
      pending_authoring_request = authoring.removeAuthoredPass(params, &error);
      if (pending_authoring_request == 0) {
        pending_authoring_kind = AuthoringRequest::none;
        authoringError(error);
        return;
      }
      authoring_status->setStyleSheet({});
      setPanelNotice(authoring_status, owner.tr("submitting removal..."),
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

        preset_passes.clear();
        root_passes.clear();
        feature_pass_info.clear();
        preset_material_routing = context.value("preset_material_routing", Json(nullptr));
        preset_digest = context.value("pipeline_preset", Json::object()).value("source_digest", Json::object()).value("hex", std::string{});
        decltype(authored_passes) next_passes;
        decltype(authored_pass_counts) next_counts;
        decltype(anchors) next_anchors;
        decltype(root_declared_passes) next_root_declared;
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
                if (pass_value.value("editable_preset_pass", false)) preset_passes.emplace(std::pair{graph_name, pass_name}, pass_value.at("preset_declaration"));
                if (pass_value.value("editable_root_pass", false)) {
                    preset_passes.emplace(std::pair{graph_name, pass_name}, pass_value.at("root_declaration"));
                    root_passes.emplace(graph_name, pass_name);
                }
                if (pass_value.contains("feature_edit_reason") || pass_value.value("editable_feature_pass", false))
                    feature_pass_info.emplace(std::pair{graph_name, pass_name}, pass_value);
                if (const auto provenance = pass_value.find("provenance");
                    provenance != pass_value.end() && provenance->is_object() &&
                    provenance->value("source", std::string{}) == "project")
                    next_root_declared.emplace(graph_name, pass_name);
                if (pass_value.value("editable_feature_pass", false))
                    preset_passes.insert_or_assign(std::pair{graph_name, pass_name}, pass_value.at("feature_declaration"));
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
        root_declared_passes = std::move(next_root_declared);
        authoring_available = true;
        const auto kind = context.value("config_kind", std::string{"direct"});
        config_kind = kind;
        root_reference = context.value("source_reference", std::string{});
        authoring_status->setStyleSheet({});
        setPanelNotice(
            authoring_status,
            owner.tr("%1 · %2 graph(s)")
                .arg(text(kind))
                .arg(static_cast<qulonglong>(authored_passes.size())),
            owner.tr("Engine authoring context loaded (%1): %2 graph(s), "
                     "%3 managed fragment(s).")
                    .arg(text(kind))
                    .arg(static_cast<qulonglong>(authored_passes.size()))
                    .arg(remove_fragment->count()) +
                QStringLiteral("\n") +
                (provenance_summary.isEmpty()
                     ? owner.tr("The engine returned no pass provenance "
                                "entries.")
                     : owner.tr("Resolved pass provenance: %1")
                           .arg(provenance_summary.join(
                               QStringLiteral(", ")))));
        configurePosition();
        if (editing_preset_pass) load_edit_requested = true;
        loadPresetPass();
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
            if (completed_kind == AuthoringRequest::edit ||
                completed_kind == AuthoringRequest::add ||
                completed_kind == AuthoringRequest::remove) {
                if (status != "accepted" ||
                    !response.contains("ticket") ||
                    !response.at("ticket").is_string()) {
                    rejectAuthoring(response);
                    return;
                }
                pending_ticket =
                    text(response.at("ticket").get_ref<const std::string &>());
                authoring_status->setStyleSheet({});
                setPanelNotice(authoring_status,
                               owner.tr("accepted; waiting..."),
                               owner.tr("Candidate accepted; waiting for "
                                        "engine publication..."));
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
                    rejectAuthoring(response);
                    return;
                }
                pending_ticket.clear();
                compile_log->clear();
                authoring_status->setStyleSheet({});
                setPanelNotice(authoring_status, owner.tr("committed"),
                               owner.tr("Authored pass transaction committed "
                                        "and published."));
                updateAuthoringButtons();
                if (editing_preset_pass) { load_edit_requested = true; authoring_available = false; }
                // N-8: the canvas's "after this node" is spent once the pass
                // is in; the next open defaults to the end of the graph again.
                requested_insert_after.reset();
                requestAuthoringContext();
                requestRefresh();
                if (committed_action) committed_action();
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
        authoring_status->setStyleSheet(
            QStringLiteral("color: #b36b00;"));
        setPanelNotice(
            authoring_status, owner.tr("authoring context unavailable"),
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
        if (&sequence == colors) reconcileColorAttachments();
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
        auto input_names = candidatesForRole(Pelican::PassShapeResourceRole::input);
        if (input_filter) {
            std::erase_if(input_names, [&](const auto &name) {
                return !input_filter->contains(QString::fromStdString(name));
            });
        }
        populateCandidates(*input_candidate, input_names);
        setPanelNotice(
            connection_hint,
            !input_filter.has_value()
                ? QString{}
                : input_names.empty() ? owner.tr("no usable outputs")
                                      : owner.tr("limited to the dragged producer"),
            input_names.empty()
                ? owner.tr("The producer has no outputs usable as inputs in this graph. Existing inputs are unchanged.")
                : owner.tr("Input choices are limited to the dragged producer. Add or remove inputs, then save the pass."));
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
            if (&sequence == colors) reconcileColorAttachments();
            updateDraft();
        }
    }

    void clearDraft() {
        updating = true;
        save->setToolTip(authoredPassSaveToolTip());
        original_preset_pass = Json::object();
        showPresetFields(false);
        name->clear();
        vertex_shader->clear();
        fragment_shader->clear();
        skinned_vertex_shader->clear();
        ray_query->setChecked(false);
        ray_query_scene->setText("scene");
        raster_state->clear();
        draw->clear();
        material_contract->setCurrentIndex(0);
        material_settings->clear();
        material_routing->clear();
        color_attachment_references->clear();
        inputs->clear();
        colors->clear();
        depth->setCurrentIndex(-1);
        position->setValue(0);
        // F-3: a new draft starts without the last draft's shader log or its
        // half-typed shader name.
        compile_log->clear();
        form->setRowVisible(shader_template_row, false);
        resetPassSettings();
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
        int value = static_cast<int>(capped);
        // N-8: the canvas asked for a pass after a named node, so the position
        // is the engine's own anchor for "after that one" rather than the end
        // of the graph. The engine's anchor list is what decides; a name it
        // does not offer leaves the default alone rather than guessing.
        if (requested_insert_after && plan && authoring_available) {
            const auto graph_anchors = anchors.find(plan->graph);
            if (graph_anchors != anchors.end()) {
                const std::string token = "after:" + *requested_insert_after;
                const auto found = std::ranges::find(graph_anchors->second,
                                                     token, &Anchor::insert);
                if (found != graph_anchors->second.end())
                    value = found->position;
            }
        }
        position->setValue(value);
    }

    void beginAuthoredPassInsert(const QString &graph_name,
                                 const QString &after_pass) {
        if (pending_authoring_request || !pending_ticket.isEmpty()) {
            authoringError(owner.tr(
                "Finish the current request before adding a pass."));
            return;
        }
        input_filter.reset();
        populateRoleCandidates();
        editing_preset_pass.reset();
        load_edit_requested = false;
        requested_insert_after = after_pass.toStdString();
        (void)graph_name;
        clearDraft();
        authoring_available = false;
        requestAuthoringContext();
        requestRefresh();
        updateDraft();
    }

    void invalidateFramePlanContext() {
        if (editing_preset_pass) load_edit_requested = true;
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
                if (editing_preset_pass) load_edit_requested = true;
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
            setPanelNotice(
                plan_status,
                changed ? owner.tr("draft discarded")
                        : owner.tr("%1 · %2 resources")
                              .arg(text(plan->graph))
                              .arg(static_cast<qulonglong>(
                                  next_binding.resources.size())),
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
            setPanelNotice(plan_status, owner.tr("refresh failed"),
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
            setPanelNotice(plan_status, owner.tr("refresh failed"),
                           owner.tr("Frame plan refresh failed: %1")
                               .arg(error));
            return;
        }
        refresh->setEnabled(false);
        plan_status->setStyleSheet({});
        setPanelNotice(plan_status, owner.tr("requesting..."),
                       owner.tr("Requesting the current frame plan..."));
    }

    void receiveRefreshResult(qint64 request_id,
                              const QByteArray &result_json) {
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        consumeResult(result_json);
        loadPresetPass();
    }

    void receiveRefreshFailure(qint64 request_id, const QString &message) {
        if (request_id != pending_request) {
            return;
        }
        pending_request = 0;
        invalidateFramePlanContext();
        refresh->setEnabled(refresh_available);
        plan_status->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        setPanelNotice(plan_status, owner.tr("refresh failed"),
                       owner.tr("Frame plan refresh failed: %1")
                           .arg(message));
    }

    void setRefreshAvailable(bool available, const QString &reason) {
        pending_request = 0;
        refresh_available = available;
        refresh->setEnabled(available);
        if (available) {
            plan_status->setStyleSheet({});
            setPanelNotice(
                plan_status, owner.tr("ready; press Refresh"),
                owner.tr("Frame plan RPC available; press Refresh."));
            return;
        }
        invalidateFramePlanContext();
        plan_status->setStyleSheet(QStringLiteral("color: #b36b00;"));
        setPanelNotice(
            plan_status, owner.tr("no frame plan"),
            reason.isEmpty()
                ? owner.tr("Frame plan unavailable.")
                : owner.tr("Frame plan unavailable: %1").arg(reason));
    }

    void consumeAuthoringConfig(const QByteArray &config_json) {
        try {
            source_digest.clear();
            anchors.clear();
            remove_fragment->clear();
            const Json document = Json::parse(
                config_json.constData(),
                config_json.constData() + config_json.size());
            if (!document.is_object()) {
                throw std::runtime_error(
                    "rendering config must be a JSON object");
            }
            // P-2a-4b: the authored passes of a `pelican.graph` root live in
            // its `rendering` section, not at the root.
            const Json &root = Pelican::renderingSectionOf(document);
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
            setPanelNotice(
                authoring_status,
                owner.tr("projection · %1 graph(s)")
                    .arg(static_cast<qulonglong>(authored_passes.size())),
                owner.tr("Projection-test authoring context loaded: %1 "
                         "graph(s), %2 pass(es). Saving remains disabled "
                         "without an engine digest and anchors.")
                        .arg(static_cast<qulonglong>(authored_passes.size()))
                        .arg(static_cast<qulonglong>(pass_count)) +
                    QStringLiteral("\n") +
                    owner.tr("Authored graph/pass declarations: %1")
                        .arg(authored_names.join(QStringLiteral(", "))));
        } catch (const std::exception &error) {
            authored_passes.clear();
            authored_pass_counts.clear();
            authoring_available = false;
            authoring_status->setStyleSheet(
                QStringLiteral("color: #b36b00;"));
            setPanelNotice(
                authoring_status, owner.tr("authoring context unavailable"),
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

void FullscreenPassWidget::setCommittedAction(std::function<void()> action) {
    impl_->committed_action = std::move(action);
}

void FullscreenPassWidget::editPresetPass(const QString &graph, const QString &pass_name) {
    impl_->beginPresetPassEdit(graph, pass_name, std::nullopt);
}

void FullscreenPassWidget::editPresetPassConnection(const QString &graph, const QString &pass_name,
                                                   const QStringList &outputs) {
    impl_->beginPresetPassEdit(graph, pass_name, outputs);
}

void FullscreenPassWidget::insertAuthoredPassAfter(const QString &graph,
                                                   const QString &after_pass) {
    impl_->beginAuthoredPassInsert(graph, after_pass);
}

void FullscreenPassWidget::setProjectRoot(
    const std::filesystem::path &project_root) {
    impl_->project_root = project_root;
    impl_->updateShaderTemplateAvailability();
}

void FullscreenPassWidget::setShaderSourceOpenAction(
    std::function<bool(const std::filesystem::path &)> action) {
    if (!action) {
        throw std::invalid_argument(
            "shader source open action must not be empty");
    }
    impl_->shader_open_action = std::move(action);
}

void FullscreenPassWidget::showShaderReloadLog(const QString &message) {
    auto &impl = *impl_;
    if (message.isEmpty()) {
        if (!impl.reload_log_text.isEmpty() &&
            impl.compile_log->logText() == impl.reload_log_text)
            impl.compile_log->clear();
        impl.reload_log_text.clear();
        return;
    }
    // Only an open Pass Editor takes the log; the canvas marks are the rest.
    if (!isVisible()) return;
    if (impl.compile_log->showMessage(message))
        impl.reload_log_text = impl.compile_log->logText();
}

void FullscreenPassWidget::receiveResult(const QByteArray &result_json) {
    impl_->pending_request = 0;
    impl_->consumeResult(result_json);
}

void FullscreenPassWidget::receiveAuthoringConfig(
    const QByteArray &config_json) {
    impl_->consumeAuthoringConfig(config_json);
}

} // namespace PelicanStudio
