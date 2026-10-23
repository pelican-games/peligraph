#include "renderingsettingswidget.hpp"
#include "rendereditticketclient.hpp"

#include "panelhelp.hpp"
#include "renderconfigdigest.hpp"
#include "featurerefusalfix.hpp"
#include "rendereditcompletion.hpp"
#include "studiotheme.hpp"
#include "passforms/shaderdefinesform.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

// A combo entry's data is its value as JSON text; empty for "the engine's
// default" (the key left out of the section).
QString text(const std::string &value) { return QString::fromStdString(value); }

QString compact(const Json &value) { return text(value.dump()); }

QJsonObject toQt(const Json &object) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(object.dump())).object();
}

// What each combo offers. The engine lists none of these (EL-3 takes each
// value whole and judges it by composing the candidate); they are the values
// docs/manual/06_rendering.md names. A value the root holds that is not here
// is added as an entry of its own, so the combo always shows what is written.
struct Choice {
    QString label;
    std::optional<Json> value;
};

// UX-15a, decision 2: a draw_sort entry is its opaque / transparent pair; its
// xr_view_policy is a combo of its own beside it, so the pair's entries do not
// carry one (and the root's value is matched with its policy left out).
Json withoutViewPolicy(Json draw_sort) {
    if (draw_sort.is_object()) draw_sort.erase("xr_view_policy");
    return draw_sort;
}

std::vector<Choice> choicesFor(const std::string &key) {
    if (key == "draw_sort")
        return {{QObject::tr("Engine default"), std::nullopt},
                {QObject::tr("State batched / back to front"),
                 Json{{"opaque", {{"provider", "state_batched_v1"}}},
                      {"transparent", {{"provider", "back_to_front_v1"}}}}}};
    if (key == "render_strategy")
        return {{QObject::tr("Engine default (builtin.authored_config_v1)"), std::nullopt}};
    if (key == "resolver_version")
        return {{QObject::tr("Engine default (2)"), std::nullopt}, {QStringLiteral("2"), Json(2)}};
    if (key == "multisampling")
        return {{QObject::tr("Off (engine default)"), std::nullopt},
                {QObject::tr("2 samples"), Json{{"samples", 2}}},
                {QObject::tr("4 samples"), Json{{"samples", 4}}},
                {QObject::tr("8 samples"), Json{{"samples", 8}}},
                {QObject::tr("Automatic"), Json{{"mode", "automatic"}}}};
    return {};
}

// UX-10, decision 7 (engine EL-6): get_registered_providers, the providers
// this engine (and its game) registered, by kind. A provider is listed by
// name; a game's is marked.
struct Provider {
    std::string name;
    bool game = false;
};

std::vector<Provider> providersOf(const Json &registered, const char *kind) {
    std::vector<Provider> providers;
    const auto found = registered.find(kind);
    if (found == registered.end() || !found->is_array()) return providers;
    for (const auto &entry : *found)
        if (entry.is_object() && entry.contains("name") && entry.at("name").is_string())
            providers.push_back({entry.at("name").get<std::string>(),
                                 entry.value("source", std::string{}) == "game"});
    return providers;
}

QString providerLabel(const Provider &provider) {
    return provider.game ? QObject::tr("%1 (game)").arg(text(provider.name)) : text(provider.name);
}

// UX-15a, decisions 1 and 3: the name a render strategy or a graph transform
// gets when nobody typed one -- its provider's, with _2, _3... after it when
// `taken` already has that (graph transform names are unique across the
// preset's and the root's).
std::string defaultNameFor(const std::string &provider, const std::vector<std::string> &taken) {
    const auto free = [&](const std::string &name) {
        return std::find(taken.begin(), taken.end(), name) == taken.end();
    };
    if (free(provider)) return provider;
    for (int suffix = 2;; ++suffix)
        if (auto name = provider + "_" + std::to_string(suffix); free(name)) return name;
}

// The draw_sort and render_strategy entries made from the registered
// providers: every opaque / transparent pair of draw sort providers, and
// each render strategy provider in place of the one `current` names (its
// other keys, such as the name the provider reads, kept). Empty when the
// engine listed none of that kind: the fixed entries stay.
std::vector<Choice> providerChoices(const std::string &key, const Json &registered,
                                    const Json *current) {
    std::vector<Choice> choices;
    if (key == "draw_sort") {
        const auto providers = providersOf(registered, "draw_sort");
        if (providers.empty()) return choices;
        choices.push_back({QObject::tr("Engine default"), std::nullopt});
        for (const auto &opaque : providers)
            for (const auto &transparent : providers)
                choices.push_back(
                    {QObject::tr("%1 / %2").arg(providerLabel(opaque), providerLabel(transparent)),
                     Json{{"opaque", {{"provider", opaque.name}}},
                          {"transparent", {{"provider", transparent.name}}}}});
        return choices;
    }
    if (key == "render_strategy") {
        const auto providers = providersOf(registered, "render_strategy");
        if (providers.empty()) return choices;
        choices.push_back({QObject::tr("Engine default (builtin.authored_config_v1)"), std::nullopt});
        for (const auto &provider : providers) {
            Json value = current != nullptr && current->is_object() ? *current : Json::object();
            value["provider"] = provider.name;
            // UX-15a, decision 1: the engine requires a name; a root without
            // one gets the provider's.
            if (!value.contains("name") || !value.at("name").is_string() ||
                value.at("name").get<std::string>().empty())
                value["name"] = defaultNameFor(provider.name, {});
            choices.push_back({providerLabel(provider), std::move(value)});
        }
    }
    return choices;
}

const std::vector<std::string> &comboKeys() {
    static const std::vector<std::string> keys{"draw_sort", "render_strategy",
                                               "resolver_version", "multisampling"};
    return keys;
}

// UX-15a: graph_transforms and xr are fields now; each keeps its whole value
// as JSON under Advanced -- graph_transforms to read, xr to write too (its
// multiview_auto profiles are measured values no field holds).
const std::vector<std::string> &jsonKeys() {
    static const std::vector<std::string> keys{"graph_transforms", "xr"};
    return keys;
}

// UX-15a, decision 5: the four keys a preset's config can hold that this tab
// shows beside the root's.
const std::vector<std::string> &presetKeys() {
    static const std::vector<std::string> keys{"shader_defines", "snapshots", "draw_sort",
                                               "graph_transforms"};
    return keys;
}

// UX-15a, decision 3: graph_transforms[]'s provider when an entry names none.
const char *const DefaultTransformProvider = "builtin.identity_v1";

QString jsonLine(const Json &value) { return text(value.dump()); }

// `object[key]` when it is a string; empty otherwise.
std::string stringField(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>()
                                                         : std::string{};
}

// A one-line JSON object field: empty is nullopt (the key left out); an
// error says what is wrong.
std::optional<Json> objectFromLine(const QString &line, bool &bad) {
    bad = false;
    const auto typed = line.trimmed();
    if (typed.isEmpty()) return std::nullopt;
    auto value = Json::parse(typed.toStdString(), nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        bad = true;
        return std::nullopt;
    }
    return value;
}

// UX-10, decision 3: rendering.material_route_default, which
// set_material_route_default writes (the engine reads it once at start).
const char *const RouteDefaultKey = "material_route_default";
const char *const RouteDefaultMethod = "set_material_route_default";

// UX-10, decision 4: where a snapshot can be taken. v1 takes exactly one,
// after the opaque pass (featurecompose.cpp: the post_ldr region).
const char *const SnapshotAfter = "post_ldr";
const char *const SnapshotName = "opaque_snapshot";

// EL-3: the reasons a root that selects a preset refuses a key, as the engine
// gives them (renderconfigeditor.cpp, renderingKeyRefusal).
std::optional<QString> presetRootRefusal(const std::string &key) {
    if (key == "multisampling")
        return QObject::tr("preset_root_takes_pipeline_settings_msaa: a rendering section "
                           "that selects a pipeline preset takes multisampling only as "
                           "pipeline.settings.msaa");
    if (key == "resolver_version")
        return QObject::tr("not_in_preset_root_keys: a rendering section that selects a "
                           "pipeline preset does not take resolver_version; it belongs in "
                           "the preset's config or in a direct root's rendering section");
    return std::nullopt;
}

// UX-12b, decisions 3 and 5: refusals said as what to do about them.
std::optional<QString> refusalSentence(const Json &response) {
    const auto error = response.is_object() ? response.value("error", Json::object())
                                            : Json::object();
    if (const auto line = snapshotAfterUnknownLine(error)) return text(*line);
    if (const auto line = materialRouteUnservedLine(error)) return text(*line);
    return std::nullopt;
}

QString refusalLine(const Json &response) {
    if (const auto sentence = refusalSentence(response)) return *sentence;
    const auto error = response.is_object() ? response.value("error", Json::object())
                                            : Json::object();
    const auto message = error.is_object() ? error.value("message", std::string{}) : std::string{};
    const auto code = error.is_object() ? error.value("code", std::string{}) : std::string{};
    if (!message.empty() && !code.empty()) return text(message + " (" + code + ")");
    if (!message.empty()) return text(message);
    if (!code.empty()) return text(code);
    return QObject::tr("The engine refused the rendering settings edit.");
}

} // namespace

const std::vector<std::string> &RenderingSettingsWidget::editableKeys() {
    // EL-3's eight, in the order the panel shows them.
    static const std::vector<std::string> keys{
        "shader_defines", "draw_sort",  "render_strategy",  "resolver_version",
        "multisampling",  "snapshots", "graph_transforms", "xr"};
    return keys;
}

struct RenderingSettingsWidget::Impl {
    enum class Request { Read, Submit };

    // One edit_rendering_keys: one key, in `set` or in `remove`. UX-10: or
    // one set_material_route_default (`method`), whose value is the key's.
    struct Edit {
        std::string key;
        std::optional<Json> value;
        std::string method;
    };

    RenderingSettingsWidget &owner;
    EmbeddedViewport &viewport;
    RenderEditTicketClient client;
    std::shared_ptr<RenderConfigDigest> digest_hub;
    std::function<void(const QString &)> engine_restart_action;
    bool restart_asked = false;

    QLabel *notice = nullptr;
    QPushButton *refresh_button = nullptr;
    ShaderDefinesForm *defines = nullptr;
    std::map<std::string, QComboBox *> combos;
    // UX-15a: graph_transforms and xr whole, under Advanced (xr with Apply).
    std::map<std::string, QPlainTextEdit *> json_fields;
    QPushButton *xr_apply = nullptr;
    // --- UX-15a, decision 1: render_strategy's name and parameters.
    QLineEdit *strategy_name = nullptr;
    QLineEdit *strategy_parameters = nullptr;
    // --- UX-15a, decision 2: draw_sort.xr_view_policy.
    QComboBox *draw_sort_policy = nullptr;
    // --- UX-15a, decision 3: one row per graph transform.
    struct TransformRow {
        QWidget *row = nullptr;
        QComboBox *provider = nullptr;
        QLineEdit *name = nullptr;
        QLineEdit *parameters = nullptr;
        QPushButton *up = nullptr;
        QPushButton *down = nullptr;
        QPushButton *remove = nullptr;
    };
    QWidget *transforms_box = nullptr;
    QVBoxLayout *transform_rows_layout = nullptr;
    std::vector<TransformRow> transform_rows;
    QPushButton *transform_add = nullptr;
    // --- UX-15a, decision 4: xr.multiview_auto, and its
    // minimum_gain_percent.
    QCheckBox *multiview_auto = nullptr;
    QDoubleSpinBox *minimum_gain = nullptr;
    // --- UX-15a, decision 5: what the selected preset holds, in grey.
    QLabel *preset_line = nullptr;
    std::map<std::string, QWidget *> preset_boxes;
    std::map<std::string, std::vector<QLabel *>> preset_rows;
    // The preset's config (get_render_documents' document of kind
    // "preset"); null when the root selects none, or the engine's own (that
    // document is listed only for a project preset).
    Json preset_config = nullptr;
    std::string preset_name;
    // --- UX-10, decision 3: Auto / Forward.
    QComboBox *route_default = nullptr;
    // --- UX-10, decision 4: one row per snapshot, and + to add one.
    struct SnapshotRow {
        QWidget *row = nullptr;
        QLineEdit *name = nullptr;
        QComboBox *after = nullptr;
        QPushButton *remove = nullptr;
    };
    QWidget *snapshots_box = nullptr;
    QVBoxLayout *snapshot_rows_layout = nullptr;
    std::vector<SnapshotRow> snapshot_rows;
    QPushButton *snapshot_add = nullptr;
    // --- and xr.view_execution, with the whole xr under Advanced.
    QComboBox *xr_view_execution = nullptr;
    QPushButton *xr_advanced = nullptr;
    // RTL-2s: where another section (the Ray layout) goes, under the form.
    QVBoxLayout *body_layout = nullptr;
    int section_at = 0;

    // --- What was read: the root's digest and its rendering section.
    std::string digest;
    Json rendering = Json::object();
    bool have_root = false;
    bool selects_preset = false;
    // Keys the engine refused here (rendering_key_not_allowed_here): the
    // reason it gave.
    std::map<std::string, QString> refused_here;
    // UX-10, decision 7: get_registered_providers, asked on each read (a
    // game reload changes it); null until answered, or when this engine
    // does not have it (the fixed entries stay).
    qint64 providers_request = 0;
    Json registered = nullptr;
    // UX-12b, decision 3: get_render_authoring_context's
    // snapshot_after_candidates, asked on each read like the providers; empty
    // until answered (the one place v1 took, post_ldr, is offered then).
    qint64 context_request = 0;
    std::vector<std::string> snapshot_candidates;

    std::optional<Edit> submitted;
    // A change made while another was in flight: sent once that one is done
    // and the root read again (the latest change of the field wins).
    std::optional<Edit> queued;
    bool retried = false;
    bool read_when_idle = false;
    bool filling = false;

    Impl(RenderingSettingsWidget &widget, EmbeddedViewport &embedded)
        : owner{widget}, viewport{embedded}, client{embedded, &widget} {
        owner.setObjectName(QStringLiteral("pelican.renderingSettings"));
        owner.setFont(studioPanelFont());
        auto *outer = new QVBoxLayout(&owner);
        outer->setContentsMargins(0, 0, 0, 0);
        auto *scroll = new QScrollArea(&owner);
        scroll->setObjectName(QStringLiteral("pelican.renderingSettings.scroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *body = new QWidget(scroll);
        auto *layout = new QVBoxLayout(body);

        auto *heading = new QHBoxLayout;
        auto *title = new QLabel(owner.tr("Rendering settings"), body);
        title->setToolTip(owner.tr(
            "The root's rendering section keys no other panel writes. A change is "
            "saved at once and applied at a frame boundary."));
        heading->addWidget(title, 1);
        heading->addWidget(makePanelHelpButton(
            body, "pelican.renderingSettings.help", ManualRenderingSettingsAnchor,
            owner.tr("The root's rendering section keys no other panel writes "
                     "(edit_rendering_keys). A change is saved at once.")));
        layout->addLayout(heading);

        // UX-15a, decision 5: a root that selects a preset says so, once.
        preset_line = new QLabel(body);
        preset_line->setObjectName(QStringLiteral("pelican.renderingSettings.preset"));
        preset_line->setWordWrap(true);
        preset_line->hide();
        layout->addWidget(preset_line);

        auto *defines_label = new QLabel(owner.tr("Shader defines"), body);
        defines_label->setToolTip(owner.tr(
            "rendering.shader_defines: added to the compile of every shader."));
        layout->addWidget(defines_label);
        layout->addWidget(presetBox("shader_defines", body));
        defines = new ShaderDefinesForm(QStringLiteral("pelican.renderingSettings"), body);
        defines->setRenderingSection(true);
        layout->addWidget(defines);
        QObject::connect(defines, &PassForm::changed, &owner, [this] { definesChanged(); });

        auto *form = new QFormLayout;
        const std::map<std::string, QString> labels{
            {"draw_sort", owner.tr("Draw sort")},
            {"render_strategy", owner.tr("Render strategy")},
            {"resolver_version", owner.tr("Resolver version")},
            {"multisampling", owner.tr("Multisampling")},
            {"snapshots", owner.tr("Snapshots")},
            {"graph_transforms", owner.tr("Graph transforms")},
            {"xr", owner.tr("XR")},
        };
        for (const auto &key : comboKeys()) {
            auto *combo = new QComboBox(body);
            combo->setObjectName(QStringLiteral("pelican.renderingSettings.") + text(key));
            combo->setToolTip(QStringLiteral("rendering.") + text(key));
            combos[key] = combo;
            QObject::connect(combo, &QComboBox::currentIndexChanged, &owner,
                             [this, key] { comboChanged(key); });
            if (key == "draw_sort") {
                // UX-15a, decision 2: the pair, and beside it how XR sorts.
                auto *box = new QWidget(body);
                auto *box_layout = new QVBoxLayout(box);
                box_layout->setContentsMargins(0, 0, 0, 0);
                box_layout->setSpacing(2);
                box_layout->addWidget(presetBox(key, box));
                auto *line = new QHBoxLayout;
                line->addWidget(combo, 2);
                draw_sort_policy = new QComboBox(box);
                draw_sort_policy->setObjectName(
                    QStringLiteral("pelican.renderingSettings.draw_sort.xrViewPolicy"));
                draw_sort_policy->addItem(owner.tr("Logical view center"),
                                          QStringLiteral("logical_view_center"));
                draw_sort_policy->addItem(owner.tr("Per view"), QStringLiteral("per_view"));
                line->addWidget(draw_sort_policy, 1);
                box_layout->addLayout(line);
                QObject::connect(draw_sort_policy, &QComboBox::currentIndexChanged, &owner,
                                 [this] { viewPolicyChanged(); });
                form->addRow(labels.at(key), box);
            } else if (key == "render_strategy") {
                // UX-15a, decision 1: the provider, and the name and
                // parameters it is given.
                auto *box = new QWidget(body);
                auto *box_layout = new QVBoxLayout(box);
                box_layout->setContentsMargins(0, 0, 0, 0);
                box_layout->setSpacing(2);
                box_layout->addWidget(combo);
                auto *line = new QHBoxLayout;
                strategy_name = new QLineEdit(box);
                strategy_name->setObjectName(
                    QStringLiteral("pelican.renderingSettings.render_strategy.name"));
                strategy_name->setPlaceholderText(owner.tr("name"));
                line->addWidget(strategy_name, 1);
                strategy_parameters = new QLineEdit(box);
                strategy_parameters->setObjectName(
                    QStringLiteral("pelican.renderingSettings.render_strategy.parameters"));
                strategy_parameters->setPlaceholderText(owner.tr("parameters (JSON); empty: none"));
                line->addWidget(strategy_parameters, 1);
                box_layout->addLayout(line);
                for (auto *field : {strategy_name, strategy_parameters})
                    QObject::connect(field, &QLineEdit::editingFinished, &owner,
                                     [this] { strategyFieldChanged(); });
                form->addRow(labels.at(key), box);
            } else {
                form->addRow(labels.at(key), combo);
            }
        }
        // UX-10, decision 3: the route materials take by default.
        route_default = new QComboBox(body);
        route_default->setObjectName(QStringLiteral("pelican.renderingSettings.") +
                                     text(RouteDefaultKey));
        route_default->addItem(owner.tr("Auto (engine default)"), QStringLiteral("auto"));
        route_default->addItem(owner.tr("Forward"), QStringLiteral("forward"));
        form->addRow(owner.tr("Material route default"), route_default);
        QObject::connect(route_default, &QComboBox::currentIndexChanged, &owner,
                         [this] { routeChanged(); });
        // UX-10, decision 4: snapshots as rows, not JSON.
        snapshots_box = new QWidget(body);
        snapshots_box->setObjectName(QStringLiteral("pelican.renderingSettings.snapshots"));
        auto *snapshots_layout = new QVBoxLayout(snapshots_box);
        snapshots_layout->setContentsMargins(0, 0, 0, 0);
        snapshots_layout->setSpacing(2);
        auto *snapshots_outer = new QWidget(body);
        auto *snapshots_outer_layout = new QVBoxLayout(snapshots_outer);
        snapshots_outer_layout->setContentsMargins(0, 0, 0, 0);
        snapshots_outer_layout->setSpacing(2);
        snapshots_outer_layout->addWidget(presetBox("snapshots", snapshots_outer));
        snapshots_outer_layout->addWidget(snapshots_box);
        snapshot_rows_layout = new QVBoxLayout;
        snapshot_rows_layout->setContentsMargins(0, 0, 0, 0);
        snapshots_layout->addLayout(snapshot_rows_layout);
        snapshot_add = new QPushButton(QStringLiteral("+"), snapshots_box);
        snapshot_add->setObjectName(QStringLiteral("pelican.renderingSettings.snapshots.add"));
        snapshot_add->setMaximumWidth(32);
        snapshots_layout->addWidget(snapshot_add, 0, Qt::AlignLeft);
        form->addRow(labels.at("snapshots"), snapshots_outer);
        QObject::connect(snapshot_add, &QPushButton::clicked, &owner, [this] { addSnapshot(); });
        // The whole value of each JSON key under Advanced: xr's is written
        // with Apply, graph_transforms' is read (its rows write it).
        for (const auto &key : jsonKeys()) {
            auto *field = new QPlainTextEdit(body);
            field->setObjectName(QStringLiteral("pelican.renderingSettings.") + text(key));
            field->setReadOnly(key != "xr");
            field->setPlaceholderText(owner.tr("Engine default (empty)"));
            field->setMaximumHeight(70);
            field->hide();
            json_fields[key] = field;
        }
        const auto advanced = [this, body](const std::string &key, const QString &tip) {
            auto *button = new QPushButton(owner.tr("Advanced"), body);
            button->setObjectName(QStringLiteral("pelican.renderingSettings.") + text(key) +
                                  QStringLiteral(".advanced"));
            button->setCheckable(true);
            button->setFlat(true);
            button->setToolTip(tip);
            QObject::connect(button, &QPushButton::toggled, json_fields.at(key),
                             &QWidget::setVisible);
            return button;
        };
        // UX-15a, decision 3: graph_transforms as rows -- the preset's in
        // grey above, the root's after them (the engine appends them).
        {
            auto *outer_box = new QWidget(body);
            auto *outer_layout = new QVBoxLayout(outer_box);
            outer_layout->setContentsMargins(0, 0, 0, 0);
            outer_layout->setSpacing(2);
            outer_layout->addWidget(presetBox("graph_transforms", outer_box));
            transforms_box = new QWidget(outer_box);
            transforms_box->setObjectName(
                QStringLiteral("pelican.renderingSettings.graph_transforms.rows"));
            auto *rows_layout = new QVBoxLayout(transforms_box);
            rows_layout->setContentsMargins(0, 0, 0, 0);
            rows_layout->setSpacing(2);
            transform_rows_layout = new QVBoxLayout;
            transform_rows_layout->setContentsMargins(0, 0, 0, 0);
            rows_layout->addLayout(transform_rows_layout);
            auto *buttons = new QHBoxLayout;
            transform_add = new QPushButton(QStringLiteral("+"), transforms_box);
            transform_add->setObjectName(
                QStringLiteral("pelican.renderingSettings.graph_transforms.add"));
            transform_add->setMaximumWidth(32);
            transform_add->setToolTip(owner.tr(
                "Add a graph transform: the first provider the engine lists, named after it"));
            buttons->addWidget(transform_add);
            buttons->addStretch(1);
            buttons->addWidget(advanced("graph_transforms",
                                        owner.tr("rendering.graph_transforms as JSON, to read")));
            rows_layout->addLayout(buttons);
            outer_layout->addWidget(transforms_box);
            outer_layout->addWidget(json_fields.at("graph_transforms"));
            QObject::connect(transform_add, &QPushButton::clicked, &owner,
                             [this] { addTransform(); });
            form->addRow(labels.at("graph_transforms"), outer_box);
        }
        // UX-10, decision 4: the view execution is picked; UX-15a, decision
        // 4: multiview_auto is a check with its minimum gain; the whole xr
        // (its profiles) is JSON under Advanced, with Apply. The check, the
        // gain and the JSON write the same key: a commit reads the root back
        // into all three.
        {
            auto *xr_box = new QWidget(body);
            auto *xr_layout = new QVBoxLayout(xr_box);
            xr_layout->setContentsMargins(0, 0, 0, 0);
            xr_layout->setSpacing(2);
            auto *pick = new QHBoxLayout;
            xr_view_execution = new QComboBox(xr_box);
            xr_view_execution->setObjectName(
                QStringLiteral("pelican.renderingSettings.xr.viewExecution"));
            xr_view_execution->addItem(owner.tr("Auto (engine default)"), QString{});
            xr_view_execution->addItem(owner.tr("Sequential"), QStringLiteral("sequential"));
            xr_view_execution->addItem(owner.tr("Multiview"), QStringLiteral("multiview"));
            pick->addWidget(xr_view_execution, 1);
            multiview_auto = new QCheckBox(owner.tr("Measured gate"), xr_box);
            multiview_auto->setObjectName(
                QStringLiteral("pelican.renderingSettings.xr.multiviewAuto"));
            pick->addWidget(multiview_auto);
            minimum_gain = new QDoubleSpinBox(xr_box);
            minimum_gain->setObjectName(
                QStringLiteral("pelican.renderingSettings.xr.minimumGainPercent"));
            // The engine's parser: [0, 100), 0 when left out.
            minimum_gain->setRange(0.0, 99.99);
            minimum_gain->setDecimals(2);
            minimum_gain->setSuffix(QStringLiteral(" %"));
            minimum_gain->setKeyboardTracking(false);
            pick->addWidget(minimum_gain);
            xr_advanced = advanced("xr", owner.tr("rendering.xr as JSON (multiview_auto profiles); "
                                                  "Apply writes it"));
            pick->addWidget(xr_advanced);
            xr_layout->addLayout(pick);
            auto *json_row = new QWidget(xr_box);
            auto *json_layout = new QHBoxLayout(json_row);
            json_layout->setContentsMargins(0, 0, 0, 0);
            auto *xr_json = json_fields.at("xr");
            xr_json->setParent(json_row);
            xr_json->show();
            json_layout->addWidget(xr_json, 1);
            xr_apply = new QPushButton(owner.tr("Apply"), json_row);
            xr_apply->setObjectName(QStringLiteral("pelican.renderingSettings.xr.apply"));
            json_layout->addWidget(xr_apply, 0, Qt::AlignTop);
            json_row->hide();
            QObject::disconnect(xr_advanced, &QPushButton::toggled, xr_json, nullptr);
            QObject::connect(xr_advanced, &QPushButton::toggled, json_row, &QWidget::setVisible);
            xr_layout->addWidget(json_row);
            QObject::connect(xr_apply, &QPushButton::clicked, &owner, [this] { xrApplied(); });
            QObject::connect(minimum_gain, &QDoubleSpinBox::valueChanged, &owner,
                             [this] { minimumGainChanged(); });
            QObject::connect(xr_view_execution, &QComboBox::currentIndexChanged, &owner,
                             [this] { viewExecutionChanged(); });
            QObject::connect(multiview_auto, &QCheckBox::toggled, &owner,
                             [this] { multiviewAutoChanged(); });
            form->addRow(labels.at("xr"), xr_box);
        }
        layout->addLayout(form);
        body_layout = layout;
        section_at = layout->count();
        layout->addStretch(1);

        notice = new QLabel(body);
        notice->setObjectName(QStringLiteral("pelican.renderingSettings.notice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        layout->addWidget(notice);

        auto *actions = new QHBoxLayout;
        refresh_button = new QPushButton(owner.tr("Refresh"), body);
        refresh_button->setObjectName(QStringLiteral("pelican.renderingSettings.refresh"));
        actions->addWidget(refresh_button);
        actions->addStretch(1);
        layout->addLayout(actions);
        QObject::connect(refresh_button, &QPushButton::clicked, &owner, [this] { read(true); });

        scroll->setWidget(body);
        outer->addWidget(scroll);



        QObject::connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable, &owner, [this] {
            forget();
            read(false);
        });
        client.unavailable = [this](const QString &message) {
                             forget();
                             have_root = false;
                             restart_asked = false;
                             showError(message);
                             controls();
                         };
        client.failed = [this](const QString &error) {
            submitted.reset();
            showError(error);
            followWhenIdle();
            controls();
        };
        client.completed = [this](const auto &outcome) { edited(outcome); };
        client.unhandledResult = [this](qint64 id, const QByteArray &bytes) { receive(id, bytes); };
        client.unhandledFailure = [this](qint64 id, const QString &message) { fail(id, message); };

        fill();
        if (viewport.rpcReady())
            read(false);
        else
            showInformation(owner.tr("Open a project to read its rendering settings."));
    }

    // --- The notice row.
    void showNotice(const QString &colour, const QString &message) {
        notice->setStyleSheet(colour);
        setPanelNotice(notice, oneLine(message), message);
    }
    void showError(const QString &message) { showNotice(QStringLiteral("color: #d94c3d;"), message); }
    void showInformation(const QString &message) {
        showNotice(QStringLiteral("color: #4f7cac;"), message);
    }
    void showSuccess(const QString &message) { showNotice(QStringLiteral("color: #388e3c;"), message); }

    [[nodiscard]] bool busy() const { return client.busy(); }

    void forget() {
        client.abandon();
        providers_request = 0;
        context_request = 0;
        submitted.reset();
        retried = false;
    }

    bool send(Request kind, const QString &method, const Json &params) {
        bool sent = false;
        if (kind == Request::Submit) sent = client.submit(method, params);
        else sent = client.read(method, params, [this](const auto &response) {
            consumeRead(response);
            followWhenIdle();
            controls();
        });
        controls();
        return sent;
    }

    // --- Reading the root.
    void read(bool say) {
        if (!viewport.rpcReady()) {
            showError(owner.tr("The engine RPC connection is not ready."));
            return;
        }
        if (busy()) {
            read_when_idle = true;
            return;
        }
        read_when_idle = false;
        if (send(Request::Read, QStringLiteral("get_render_documents"), Json::object()) && say)
            showInformation(owner.tr("Reading the rendering section..."));
        askProviders();
        askSnapshotCandidates();
    }

    // UX-12b, decision 3: where this pipeline takes a snapshot after.
    void askSnapshotCandidates() {
        if (context_request != 0) return;
        QString error;
        context_request = viewport.requestRpc(QStringLiteral("get_render_authoring_context"),
                                              QJsonObject{}, &error);
    }

    // The after entries a row offers: the candidates, else v1's one place.
    std::vector<std::string> snapshotAfters() const {
        return snapshot_candidates.empty() ? std::vector<std::string>{SnapshotAfter}
                                           : snapshot_candidates;
    }

    QString snapshotAfterLabel(const std::string &after) const {
        if (after == SnapshotAfter) return owner.tr("After opaque (post_ldr)");
        return owner.tr("After %1").arg(text(after));
    }

    // UX-10, decision 7: the providers the combos offer, read again with the
    // root (never kept across a game reload).
    void askProviders() {
        if (providers_request != 0) return;
        QString error;
        providers_request = viewport.requestRpc(QStringLiteral("get_registered_providers"),
                                                QJsonObject{}, &error);
    }

    void consumeRead(const Json &response) {
        if (response.value("status", std::string{}) == "rejected") {
            showError(refusalLine(response));
            return;
        }
        const auto documents = response.value("documents", Json::array());
        const Json *root = nullptr;
        if (documents.is_array())
            for (const auto &document : documents)
                if (document.is_object() && document.value("kind", std::string{}) == "root") {
                    root = &document;
                    break;
                }
        if (root == nullptr) {
            showError(owner.tr("The engine answered no root rendering document."));
            return;
        }
        const auto content =
            Json::parse(root->value("content", std::string{}), nullptr, false);
        rendering = Json::object();
        if (content.is_object()) {
            const auto section = content.find("rendering");
            if (section != content.end() && section->is_object()) rendering = *section;
        }
        // A rendering section that selects a preset names it as `pipeline`
        // (EL-3, editRenderingKeys).
        selects_preset = rendering.contains("pipeline");
        // UX-15a, decision 5: the preset's config, when the engine lists its
        // document (a project preset; the engine's own is not listed).
        preset_config = nullptr;
        preset_name.clear();
        if (selects_preset)
            for (const auto &document : documents)
                if (document.is_object() && document.value("kind", std::string{}) == "preset") {
                    const auto preset =
                        Json::parse(document.value("content", std::string{}), nullptr, false);
                    if (preset.is_object() && preset.contains("config") &&
                        preset.at("config").is_object()) {
                        preset_config = preset.at("config");
                        preset_name = stringField(preset, "name");
                        if (preset_name.empty()) preset_name = stringField(document, "reference");
                    }
                    break;
                }
        digest = root->value("digest", std::string{});
        have_root = !digest.empty();
        if (digest_hub && have_root) digest_hub->publish(digest, &owner);
        fill();
        if (queued) {
            auto edit = std::move(*queued);
            queued.reset();
            submit(std::move(edit));
        }
    }

    // --- The fields.
    std::optional<QString> refusal(const std::string &key) const {
        if (const auto found = refused_here.find(key); found != refused_here.end())
            return found->second;
        if (selects_preset) return presetRootRefusal(key);
        return std::nullopt;
    }

    const Json *current(const std::string &key) const {
        const auto found = rendering.find(key);
        return found != rendering.end() ? &*found : nullptr;
    }

    void fill() {
        filling = true;
        for (const auto &[key, combo] : combos) {
            combo->clear();
            auto choices = registered.is_object()
                               ? providerChoices(key, registered, current(key))
                               : std::vector<Choice>{};
            if (choices.empty()) choices = choicesFor(key);
            for (const auto &choice : choices)
                combo->addItem(choice.label,
                               choice.value ? compact(*choice.value) : QString{});
            // The first entry is the key left out. UX-15a, decision 2: a
            // draw_sort is matched with its xr_view_policy left out.
            const auto *value = current(key);
            const QString wanted =
                value ? compact(key == "draw_sort" ? withoutViewPolicy(*value) : *value)
                      : QString{};
            int index = value ? combo->findData(wanted) : 0;
            if (index < 0) {
                // Written by hand or by another tool: shown as it is.
                combo->addItem(owner.tr("Custom: %1").arg(wanted), wanted);
                index = combo->count() - 1;
            }
            combo->setCurrentIndex(index);
        }
        defines->load(current("shader_defines") ? *current("shader_defines") : Json(nullptr));
        for (const auto &[key, field] : json_fields) {
            const auto *value = current(key);
            field->setPlainText(value ? text(value->dump(2)) : QString{});
        }
        route_default->setCurrentIndex(
            route_default->findData(text(routeDefault())) >= 0
                ? route_default->findData(text(routeDefault()))
                : 0);
        {
            const auto *xr = current("xr");
            const auto execution = xr != nullptr && xr->is_object()
                                       ? xr->value("view_execution", std::string{})
                                       : std::string{};
            int index = execution == "auto" ? 0 : xr_view_execution->findData(text(execution));
            if (index < 0) {
                xr_view_execution->addItem(owner.tr("Custom: %1").arg(text(execution)),
                                           text(execution));
                index = xr_view_execution->count() - 1;
            }
            xr_view_execution->setCurrentIndex(index);
            // UX-15a, decision 4.
            multiview_auto->setChecked(xr != nullptr && xr->is_object() &&
                                       xr->contains("multiview_auto"));
            double gain = 0.0;
            if (multiview_auto->isChecked())
                if (const auto &automatic = xr->at("multiview_auto"); automatic.is_object())
                    if (const auto found = automatic.find("minimum_gain_percent");
                        found != automatic.end() && found->is_number())
                        gain = found->get<double>();
            minimum_gain->setValue(gain);
        }
        // UX-15a, decision 1: the strategy's name and parameters.
        {
            const auto *strategy = current("render_strategy");
            const bool object = strategy != nullptr && strategy->is_object();
            strategy_name->setText(text(object ? stringField(*strategy, "name") : std::string{}));
            const auto parameters = object && strategy->contains("parameters")
                                        ? jsonLine(strategy->at("parameters"))
                                        : QString{};
            strategy_parameters->setText(parameters);
        }
        // UX-15a, decision 2: how XR sorts; absent is the engine's
        // logical_view_center.
        {
            const auto *draw_sort = current("draw_sort");
            const auto policy = draw_sort != nullptr && draw_sort->is_object()
                                    ? stringField(*draw_sort, "xr_view_policy")
                                    : std::string{};
            int index = policy.empty() ? 0 : draw_sort_policy->findData(text(policy));
            if (index < 0) {
                draw_sort_policy->addItem(owner.tr("Custom: %1").arg(text(policy)), text(policy));
                index = draw_sort_policy->count() - 1;
            }
            draw_sort_policy->setCurrentIndex(index);
        }
        fillSnapshots();
        fillTransforms();
        fillPreset();
        filling = false;
        controls();
    }

    // --- UX-15a, decision 5: the preset's values, in grey rows.
    QWidget *presetBox(const std::string &key, QWidget *parent) {
        auto *box = new QWidget(parent);
        box->setObjectName(QStringLiteral("pelican.renderingSettings.") + text(key) +
                           QStringLiteral(".preset"));
        auto *box_layout = new QVBoxLayout(box);
        box_layout->setContentsMargins(0, 0, 0, 0);
        box_layout->setSpacing(0);
        box->hide();
        preset_boxes[key] = box;
        return box;
    }

    QString presetTip() const {
        return owner.tr("Comes from preset %1; fork the preset to change it")
            .arg(text(preset_name));
    }

    // What the preset's `key` holds, a line a row.
    std::vector<QString> presetLines(const std::string &key) const {
        std::vector<QString> lines;
        if (!preset_config.is_object()) return lines;
        const auto found = preset_config.find(key);
        if (found == preset_config.end()) return lines;
        const auto &value = *found;
        const auto entryLine = [](const Json &entry) {
            return entry.is_string() ? text(entry.get<std::string>()) : jsonLine(entry);
        };
        if (key == "shader_defines" && value.is_array()) {
            for (const auto &entry : value) lines.push_back(entryLine(entry));
        } else if (key == "snapshots" && value.is_array()) {
            for (const auto &entry : value)
                lines.push_back(entry.is_object()
                                    ? owner.tr("%1, after %2")
                                          .arg(text(stringField(entry, "name")),
                                               text(stringField(entry, "after")))
                                    : jsonLine(entry));
        } else if (key == "graph_transforms" && value.is_array()) {
            for (const auto &entry : value) {
                if (!entry.is_object()) {
                    lines.push_back(jsonLine(entry));
                    continue;
                }
                auto provider = stringField(entry, "provider");
                if (provider.empty()) provider = DefaultTransformProvider;
                auto line = owner.tr("%1 (%2)").arg(text(stringField(entry, "name")), text(provider));
                if (entry.contains("parameters"))
                    line += QStringLiteral(" ") + jsonLine(entry.at("parameters"));
                lines.push_back(line);
            }
        } else if (key == "draw_sort" && value.is_object()) {
            const auto phase = [&](const char *name) {
                const auto found_phase = value.find(name);
                return found_phase != value.end() ? stringField(*found_phase, "provider")
                                                  : std::string{};
            };
            auto policy = stringField(value, "xr_view_policy");
            if (policy.empty()) policy = "logical_view_center";
            auto line = owner.tr("%1 / %2, XR %3")
                            .arg(text(phase("opaque")), text(phase("transparent")), text(policy));
            // The root's own draw_sort replaces the preset's.
            if (current("draw_sort") != nullptr) line += owner.tr(" (the root's below replaces it)");
            lines.push_back(line);
        } else {
            lines.push_back(jsonLine(value));
        }
        return lines;
    }

    void fillPreset() {
        const QString dim = QStringLiteral("color: %1;").arg(studioDimTextColor().name());
        for (const auto &key : presetKeys()) {
            auto &rows = preset_rows[key];
            for (auto *label : rows) {
                label->hide();
                label->setParent(nullptr);
                label->deleteLater();
            }
            rows.clear();
            auto *box = preset_boxes.at(key);
            const auto lines = presetLines(key);
            for (std::size_t index = 0; index < lines.size(); ++index) {
                auto *label = new QLabel(owner.tr("Preset: %1").arg(lines[index]), box);
                label->setObjectName(box->objectName() + QStringLiteral(".row%1").arg(index));
                label->setStyleSheet(dim);
                label->setToolTip(presetTip());
                label->setWordWrap(true);
                box->layout()->addWidget(label);
                rows.push_back(label);
            }
            box->setVisible(!rows.empty());
        }
        if (!selects_preset) {
            preset_line->hide();
            return;
        }
        const auto *pipeline = current("pipeline");
        const auto reference = pipeline != nullptr ? stringField(*pipeline, "preset") : std::string{};
        preset_line->setStyleSheet(dim);
        if (preset_config.is_object()) {
            preset_line->setText(owner.tr("This root selects preset %1: what it holds shows in "
                                          "grey. Fork the preset to change it.")
                                     .arg(text(preset_name)));
            preset_line->setToolTip(presetTip());
        } else {
            preset_line->setText(owner.tr("This root selects the engine's preset %1: its values "
                                          "are not listed here. Fork the preset to see and "
                                          "change them.")
                                     .arg(text(reference)));
            preset_line->setToolTip(text(reference));
        }
        preset_line->show();
    }

    // --- UX-15a, decision 1: the name and parameters of the strategy picked.
    void strategyFieldChanged() {
        if (filling) return;
        const auto *now = current("render_strategy");
        if (now == nullptr || !now->is_object()) return;
        Json value = *now;
        const auto name = strategy_name->text().trimmed().toStdString();
        if (name.empty()) {
            showError(owner.tr("A render strategy needs a name."));
            strategy_name->setText(text(stringField(*now, "name")));
            return;
        }
        value["name"] = name;
        bool bad = false;
        const auto parameters = objectFromLine(strategy_parameters->text(), bad);
        if (bad) {
            showError(owner.tr("The render strategy's parameters are not a JSON object."));
            return;
        }
        if (parameters)
            value["parameters"] = *parameters;
        else
            value.erase("parameters");
        edit("render_strategy", std::move(value));
    }

    // --- UX-15a, decision 2: how XR sorts the picked draw sort.
    void viewPolicyChanged() {
        if (filling) return;
        const auto *now = current("draw_sort");
        if (now == nullptr || !now->is_object()) return;
        Json value = *now;
        value["xr_view_policy"] = draw_sort_policy->currentData().toString().toStdString();
        edit("draw_sort", std::move(value));
    }

    // --- UX-15a, decision 4: xr.multiview_auto on (its defaults, the
    // profiles kept) or off.
    void multiviewAutoChanged() {
        if (filling) return;
        const auto *now = current("xr");
        Json xr = now != nullptr && now->is_object() ? *now : Json::object();
        if (multiview_auto->isChecked()) {
            if (!xr.contains("multiview_auto")) xr["multiview_auto"] = Json::object();
        } else {
            xr.erase("multiview_auto");
        }
        edit("xr", xr.empty() ? std::nullopt : std::optional<Json>{std::move(xr)});
    }

    // xr.multiview_auto.minimum_gain_percent; 0 (the engine's) is left out
    // when it was not written.
    void minimumGainChanged() {
        if (filling) return;
        const auto *now = current("xr");
        if (now == nullptr || !now->is_object() || !now->contains("multiview_auto") ||
            !now->at("multiview_auto").is_object())
            return;
        Json xr = *now;
        auto &automatic = xr["multiview_auto"];
        if (minimum_gain->value() == 0.0 && !automatic.contains("minimum_gain_percent"))
            return;
        automatic["minimum_gain_percent"] = minimum_gain->value();
        edit("xr", std::move(xr));
    }

    // The whole xr as typed under Advanced; empty leaves it out.
    void xrApplied() {
        const auto typed = json_fields.at("xr")->toPlainText().trimmed();
        if (typed.isEmpty()) {
            edit("xr", std::nullopt);
            return;
        }
        auto value = Json::parse(typed.toStdString(), nullptr, false);
        if (value.is_discarded() || !value.is_object()) {
            showError(owner.tr("rendering.xr is not a JSON object."));
            return;
        }
        edit("xr", std::move(value));
    }

    // --- UX-15a, decision 3: the graph transform rows.
    Json transformsRead() const {
        const auto *value = current("graph_transforms");
        return value != nullptr && value->is_array() ? *value : Json::array();
    }

    std::vector<Provider> transformProviders() const {
        auto providers = registered.is_object() ? providersOf(registered, "graph_transforms")
                                                : std::vector<Provider>{};
        if (providers.empty()) providers.push_back({DefaultTransformProvider, false});
        return providers;
    }

    // The names a new transform must not take: the preset's and the root's.
    std::vector<std::string> transformNames() const {
        std::vector<std::string> names;
        const auto collect = [&names](const Json &entries) {
            if (entries.is_array())
                for (const auto &entry : entries) names.push_back(stringField(entry, "name"));
        };
        collect(transformsRead());
        if (preset_config.is_object())
            collect(preset_config.value("graph_transforms", Json::array()));
        return names;
    }

    void fillTransforms() {
        for (auto &row : transform_rows) {
            row.row->hide();
            row.row->setParent(nullptr);
            row.row->deleteLater();
        }
        transform_rows.clear();
        const auto entries = transformsRead();
        const auto providers = transformProviders();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto &entry = entries[index];
            const auto prefix =
                QStringLiteral("pelican.renderingSettings.graph_transforms.row%1").arg(index);
            TransformRow row;
            row.row = new QWidget(transforms_box);
            auto *line = new QHBoxLayout(row.row);
            line->setContentsMargins(0, 0, 0, 0);
            row.provider = new QComboBox(row.row);
            row.provider->setObjectName(prefix + QStringLiteral(".provider"));
            row.provider->setToolTip(owner.tr("The provider that transforms the composed graph "
                                              "(get_registered_providers)"));
            const auto provider = stringField(entry, "provider");
            if (provider.empty())
                row.provider->addItem(owner.tr("Default (%1)").arg(text(DefaultTransformProvider)),
                                      QString{});
            for (const auto &each : providers)
                row.provider->addItem(providerLabel(each), text(each.name));
            if (!provider.empty() && row.provider->findData(text(provider)) < 0)
                row.provider->addItem(owner.tr("Custom: %1").arg(text(provider)), text(provider));
            row.provider->setCurrentIndex(std::max(0, row.provider->findData(text(provider))));
            line->addWidget(row.provider, 2);
            row.name = new QLineEdit(text(stringField(entry, "name")), row.row);
            row.name->setObjectName(prefix + QStringLiteral(".name"));
            row.name->setToolTip(owner.tr("The transform's name, unique among the preset's and "
                                          "the root's"));
            line->addWidget(row.name, 2);
            row.parameters = new QLineEdit(row.row);
            row.parameters->setObjectName(prefix + QStringLiteral(".parameters"));
            row.parameters->setPlaceholderText(owner.tr("parameters (JSON); empty: none"));
            if (entry.is_object() && entry.contains("parameters"))
                row.parameters->setText(jsonLine(entry.at("parameters")));
            line->addWidget(row.parameters, 2);
            const auto button = [&](const QString &label, const char *suffix, const QString &tip) {
                auto *pressed = new QPushButton(label, row.row);
                pressed->setObjectName(prefix + QString::fromLatin1(suffix));
                pressed->setToolTip(tip);
                pressed->setMaximumWidth(28);
                line->addWidget(pressed);
                return pressed;
            };
            row.up = button(QStringLiteral("\u2191"), ".up", owner.tr("Run this transform earlier"));
            row.down = button(QStringLiteral("\u2193"), ".down", owner.tr("Run this transform later"));
            row.remove = button(QStringLiteral("\u00d7"), ".remove", owner.tr("Remove this transform"));
            row.up->setEnabled(index > 0);
            row.down->setEnabled(index + 1 < entries.size());
            transform_rows_layout->addWidget(row.row);
            QObject::connect(row.provider, &QComboBox::currentIndexChanged, &owner,
                             [this] { transformsChanged(std::nullopt, 0); });
            for (auto *field : {row.name, row.parameters})
                QObject::connect(field, &QLineEdit::editingFinished, &owner,
                                 [this] { transformsChanged(std::nullopt, 0); });
            QObject::connect(row.up, &QPushButton::clicked, &owner,
                             [this, index] { transformsChanged(index, -1); });
            QObject::connect(row.down, &QPushButton::clicked, &owner,
                             [this, index] { transformsChanged(index, +1); });
            QObject::connect(row.remove, &QPushButton::clicked, &owner,
                             [this, index] { transformsChanged(index, 0); });
            transform_rows.push_back(row);
        }
    }

    // The rows as the key's value. `at` with `move` 0 is removed; with -1 /
    // +1 it trades places with the row before / after it.
    void transformsChanged(std::optional<std::size_t> at, int move) {
        if (filling) return;
        const auto entries = transformsRead();
        std::vector<Json> rows;
        for (std::size_t index = 0; index < transform_rows.size(); ++index) {
            const auto &row = transform_rows[index];
            Json entry = index < entries.size() && entries[index].is_object() ? entries[index]
                                                                              : Json::object();
            const auto name = row.name->text().trimmed().toStdString();
            if (name.empty()) {
                showError(owner.tr("A graph transform needs a name."));
                return;
            }
            entry["name"] = name;
            const auto provider = row.provider->currentData().toString().toStdString();
            if (provider.empty())
                entry.erase("provider");
            else
                entry["provider"] = provider;
            bool bad = false;
            const auto parameters = objectFromLine(row.parameters->text(), bad);
            if (bad) {
                showError(owner.tr("The parameters of graph transform %1 are not a JSON object.")
                              .arg(text(name)));
                return;
            }
            if (parameters)
                entry["parameters"] = *parameters;
            else
                entry.erase("parameters");
            rows.push_back(std::move(entry));
        }
        if (at && *at < rows.size()) {
            if (move == 0) {
                rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(*at));
            } else {
                const auto other = static_cast<std::ptrdiff_t>(*at) + move;
                if (other >= 0 && other < static_cast<std::ptrdiff_t>(rows.size()))
                    std::swap(rows[*at], rows[static_cast<std::size_t>(other)]);
            }
        }
        Json value = Json::array();
        for (auto &entry : rows) value.push_back(std::move(entry));
        edit("graph_transforms",
             value.empty() ? std::nullopt : std::optional<Json>{std::move(value)});
    }

    // +: the first provider the engine lists, named after it.
    void addTransform() {
        auto value = transformsRead();
        const auto provider = transformProviders().front().name;
        value.push_back({{"provider", provider}, {"name", defaultNameFor(provider, transformNames())}});
        edit("graph_transforms", std::move(value));
    }

    // --- UX-10, decision 3.
    std::string routeDefault() const {
        const auto *value = current(RouteDefaultKey);
        return value != nullptr && value->is_string() ? value->get<std::string>() : "auto";
    }

    void routeChanged() {
        if (filling) return;
        const auto wanted = route_default->currentData().toString().toStdString();
        if (wanted.empty() || wanted == routeDefault()) return;
        Edit change{RouteDefaultKey, Json(wanted), RouteDefaultMethod};
        if (busy() || !have_root) {
            queued = std::move(change);
            return;
        }
        submit(std::move(change));
    }

    // --- UX-10, decision 4: xr.view_execution.
    void viewExecutionChanged() {
        if (filling) return;
        const auto *now = current("xr");
        Json xr = now != nullptr && now->is_object() ? *now : Json::object();
        const auto wanted = xr_view_execution->currentData().toString().toStdString();
        if (wanted.empty())
            xr.erase("view_execution");
        else
            xr["view_execution"] = wanted;
        edit("xr", xr.empty() ? std::nullopt : std::optional<Json>{std::move(xr)});
    }

    // --- UX-10, decision 4: the snapshot rows.
    Json snapshotsRead() const {
        const auto *value = current("snapshots");
        return value != nullptr && value->is_array() ? *value : Json::array();
    }

    void fillSnapshots() {
        for (auto &row : snapshot_rows) {
            row.row->hide();
            row.row->deleteLater();
        }
        snapshot_rows.clear();
        const auto entries = snapshotsRead();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto &entry = entries[index];
            const auto prefix = QStringLiteral("pelican.renderingSettings.snapshots.row%1").arg(index);
            SnapshotRow row;
            row.row = new QWidget(snapshots_box);
            auto *line = new QHBoxLayout(row.row);
            line->setContentsMargins(0, 0, 0, 0);
            row.name = new QLineEdit(row.row);
            row.name->setObjectName(prefix + QStringLiteral(".name"));
            row.name->setToolTip(owner.tr("The snapshot's name: the render target a shader reads it by"));
            row.name->setText(entry.is_object() ? text(entry.value("name", std::string{})) : QString{});
            line->addWidget(row.name, 1);
            row.after = new QComboBox(row.row);
            row.after->setObjectName(prefix + QStringLiteral(".after"));
            row.after->setToolTip(owner.tr("Where the picture is copied: the passes and anchors "
                                           "this pipeline takes a snapshot after"));
            for (const auto &candidate : snapshotAfters())
                row.after->addItem(snapshotAfterLabel(candidate), text(candidate));
            const auto after = entry.is_object() ? entry.value("after", std::string{}) : std::string{};
            if (!after.empty() && row.after->findData(text(after)) < 0)
                row.after->addItem(owner.tr("Custom: %1").arg(text(after)), text(after));
            row.after->setCurrentIndex(std::max(0, row.after->findData(text(after))));
            line->addWidget(row.after, 1);
            row.remove = new QPushButton(QStringLiteral("\u00d7"), row.row);
            row.remove->setObjectName(prefix + QStringLiteral(".remove"));
            row.remove->setToolTip(owner.tr("Remove this snapshot"));
            row.remove->setMaximumWidth(28);
            line->addWidget(row.remove);
            snapshot_rows_layout->addWidget(row.row);
            QObject::connect(row.name, &QLineEdit::editingFinished, &owner,
                             [this] { snapshotsChanged(std::nullopt); });
            QObject::connect(row.after, &QComboBox::currentIndexChanged, &owner,
                             [this] { snapshotsChanged(std::nullopt); });
            QObject::connect(row.remove, &QPushButton::clicked, &owner,
                             [this, index] { snapshotsChanged(index); });
            snapshot_rows.push_back(row);
        }
    }

    // The rows as the section's value, the one at `removed` left out; the
    // keys a row does not show are kept as they were.
    void snapshotsChanged(std::optional<std::size_t> removed) {
        if (filling) return;
        const auto entries = snapshotsRead();
        Json value = Json::array();
        for (std::size_t index = 0; index < snapshot_rows.size(); ++index) {
            if (removed && *removed == index) continue;
            Json entry = index < entries.size() && entries[index].is_object() ? entries[index]
                                                                              : Json::object();
            const auto name = snapshot_rows[index].name->text().trimmed().toStdString();
            if (name.empty()) {
                showError(owner.tr("A snapshot needs a name."));
                return;
            }
            entry["name"] = name;
            entry["after"] = snapshot_rows[index].after->currentData().toString().toStdString();
            value.push_back(std::move(entry));
        }
        edit("snapshots", value.empty() ? std::nullopt : std::optional<Json>{std::move(value)});
    }

    void addSnapshot() {
        auto value = snapshotsRead();
        value.push_back({{"name", SnapshotName}, {"after", snapshotAfters().front()}});
        edit("snapshots", std::move(value));
    }

    void controls() {
        const bool ready = viewport.rpcReady() && have_root;
        const auto gate = [this, ready](QWidget *widget, const std::string &key) {
            const auto refused = refusal(key);
            widget->setEnabled(ready && !refused);
            if (refused)
                widget->setToolTip(*refused);
            else
                widget->setToolTip(QStringLiteral("rendering.") + text(key));
        };
        for (const auto &[key, combo] : combos) gate(combo, key);
        for (const auto &[key, field] : json_fields) gate(field, key);
        // UX-15a, decisions 1 and 2: a strategy's name and parameters, and a
        // draw sort's XR policy, once there is one.
        const auto *strategy = current("render_strategy");
        const bool strategy_set = strategy != nullptr && strategy->is_object();
        for (auto *field : {strategy_name, strategy_parameters}) {
            gate(field, "render_strategy");
            field->setEnabled(field->isEnabled() && strategy_set);
        }
        if (!refusal("render_strategy")) {
            strategy_name->setToolTip(owner.tr("render_strategy.name: the strategy the provider "
                                               "makes (required; the provider's name unless "
                                               "typed)"));
            strategy_parameters->setToolTip(owner.tr("render_strategy.parameters as one JSON "
                                                     "object; empty leaves it out"));
        }
        gate(draw_sort_policy, "draw_sort");
        const auto *draw_sort = current("draw_sort");
        draw_sort_policy->setEnabled(draw_sort_policy->isEnabled() && draw_sort != nullptr &&
                                     draw_sort->is_object());
        if (!refusal("draw_sort"))
            draw_sort_policy->setToolTip(owner.tr(
                "draw_sort.xr_view_policy: in XR, sort once from the center of the views "
                "(Logical view center) or again for each view (Per view)"));
        // UX-15a, decision 3.
        gate(transforms_box, "graph_transforms");
        // UX-15a, decision 4.
        gate(multiview_auto, "xr");
        gate(minimum_gain, "xr");
        minimum_gain->setEnabled(minimum_gain->isEnabled() && multiview_auto->isChecked());
        xr_apply->setEnabled(json_fields.at("xr")->isEnabled());
        if (!refusal("xr")) {
            multiview_auto->setToolTip(owner.tr(
                "xr.multiview_auto: with view execution Auto, take multiview only where a "
                "measured profile says it is faster (its profiles are written under Advanced)"));
            minimum_gain->setToolTip(owner.tr(
                "xr.multiview_auto.minimum_gain_percent: how much faster a profile must measure "
                "multiview to take it (0, the engine's, when left out)"));
        }
        route_default->setEnabled(ready);
        route_default->setToolTip(owner.tr(
            "rendering.material_route_default: the pass a material without a route of its own "
            "draws in. The engine reads it at start, so a change restarts it."));
        gate(xr_view_execution, "xr");
        gate(snapshots_box, "snapshots");
        // v1 takes exactly one snapshot.
        snapshot_add->setEnabled(snapshots_box->isEnabled() && snapshot_rows.empty());
        snapshot_add->setToolTip(snapshot_rows.empty()
                                     ? owner.tr("Add a snapshot of the picture after the opaque pass")
                                     : owner.tr("This version takes one snapshot"));
        defines->setEnabled(ready);
        refresh_button->setEnabled(viewport.rpcReady() && !busy());
    }

    void comboChanged(const std::string &key) {
        if (filling) return;
        const auto data = combos.at(key)->currentData().toString();
        if (data.isEmpty()) {
            edit(key, std::nullopt);
            return;
        }
        auto value = Json::parse(data.toStdString());
        // UX-15a, decision 2: the pair with the XR policy picked beside it.
        if (key == "draw_sort" && value.is_object())
            value["xr_view_policy"] = draw_sort_policy->currentData().toString().toStdString();
        edit(key, std::move(value));
    }

    void definesChanged() {
        if (filling) return;
        if (const auto refused = defines->refusal(); !refused.isEmpty()) {
            showError(refused);
            return;
        }
        const auto value = defines->defines();
        edit("shader_defines", value.is_null() ? std::nullopt : std::optional<Json>{value});
    }

    // --- Sending.
    void edit(const std::string &key, std::optional<Json> value) {
        const auto *now = current(key);
        if ((now == nullptr && !value) || (now != nullptr && value && *now == *value)) return;
        Edit change{key, std::move(value)};
        if (busy() || !have_root) {
            queued = std::move(change);
            return;
        }
        submit(std::move(change));
    }

    void submit(Edit change) {
        if (refusal(change.key)) return;
        Json params{{"base_source_digest", digest}};
        if (!change.method.empty() && change.value)
            params[change.key] = *change.value;
        else if (change.value)
            params["set"] = Json{{change.key, *change.value}};
        else
            params["remove"] = Json::array({change.key});
        const auto key = text(change.key);
        const auto method = change.method.empty() ? QStringLiteral("edit_rendering_keys")
                                                  : text(change.method);
        submitted = std::move(change);
        if (send(Request::Submit, method, params))
            showInformation(owner.tr("Saving %1...").arg(key));
        else
            submitted.reset();
    }





    void edited(const RenderEditTicketClient::Outcome &outcome) {
        using Kind = RenderEditTicketClient::Kind;
        if (outcome.kind == Kind::Accepted || outcome.kind == Kind::Pending) return;
        if (outcome.kind == Kind::Committed) consumeResult(outcome.result);
        else if (outcome.kind == Kind::Refused ||
                 (outcome.kind == Kind::Failed && outcome.result.is_object() && outcome.result.contains("error")))
            refused(outcome.result);
        else {
            showError(text(outcome.error));
            if (outcome.kind == Kind::Failed) {
                submitted.reset();
                read(false);
            }
        }
        followWhenIdle();
        controls();
    }

    void consumeResult(const Json &response) {
        // RSX, decision 1: the reading every panel uses. restart_required
        // hands the restart to the window before this returns.
        const auto completion = completeRenderEdit(response, [this](const std::string &line) {
            restart_asked = true;
            if (engine_restart_action) engine_restart_action(text(line));
            showInformation(text(line));
        });
        if (!completion.committed) {
            refused(response);
            return;
        }
        const auto change = std::exchange(submitted, std::nullopt);
        retried = false;
        if (!completion.source_digest.empty()) {
            digest = completion.source_digest;
        }
        // The root is read from the engine that comes back.
        if (completion.restart_required) return;
        const auto key = change ? text(change->key) : QString{};
        if (completion.no_change)
            showSuccess(owner.tr("%1 was already so.").arg(key));
        else
            showSuccess(owner.tr("Saved %1.").arg(key));
        read(false);
    }

    void refused(const Json &response) {
        const auto error = response.value("error", Json::object());
        const auto code = error.is_object() ? error.value("code", std::string{}) : std::string{};
        // Q-13: another panel moved the document since it was read. Read it
        // again and send the change once more against the new revision.
        if (code == "external_modification" && submitted && !retried) {
            retried = true;
            queued = std::move(submitted);
            submitted.reset();
            showInformation(owner.tr(
                "The document changed since this panel read it; reading it again..."));
            read(false);
            return;
        }
        if (code == "rendering_key_not_allowed_here" && error.is_object()) {
            const auto key = error.value("key", std::string{});
            const auto reason = error.value("reason", std::string{});
            const auto message = error.value("message", std::string{});
            if (!key.empty())
                refused_here[key] = text(reason.empty() ? message : reason + ": " + message);
        }
        submitted.reset();
        retried = false;
        // UX-12b, decisions 3 and 5: those said whole (oneLine would cut
        // them at their length).
        if (const auto whole = refusalSentence(response)) {
            showError(*whole);
            setPanelNotice(notice, *whole, *whole);
        } else {
            showError(refusalLine(response));
        }
        // The fields go back to what the root holds.
        read(false);
    }

    // --- Replies.
    void receive(qint64 id, const QByteArray &bytes) {
        if (id != 0 && id == providers_request) {
            providers_request = 0;
            const auto response =
                Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
            registered = response.is_object() ? response : Json(nullptr);
            // The combos are made again, unless a change of theirs is on its way.
            if (!busy()) fill();
            return;
        }
        if (id != 0 && id == context_request) {
            context_request = 0;
            const auto response =
                Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
            std::vector<std::string> candidates;
            if (response.is_object())
                for (const auto &candidate :
                     response.value("snapshot_after_candidates", Json::array()))
                    if (candidate.is_string()) candidates.push_back(candidate.get<std::string>());
            if (candidates != snapshot_candidates) {
                snapshot_candidates = std::move(candidates);
                if (!busy()) fill();
            }
            return;
        }
    }

    void fail(qint64 id, const QString &message) {
        // An engine without get_registered_providers (-32601): the fixed
        // entries stay, and nothing is said.
        if (id != 0 && id == providers_request) {
            providers_request = 0;
            registered = nullptr;
            if (!busy()) fill();
            return;
        }
        // An engine without the context (or a refusal): v1's one place stays.
        if (id != 0 && id == context_request) {
            context_request = 0;
            return;
        }
    }

    void followWhenIdle() {
        if (!busy() && read_when_idle) read(false);
    }

    // Another panel read or committed the document at `moved`.
    void documentMoved(const std::string &moved) {
        if (moved == digest) return;
        if (busy()) {
            read_when_idle = true;
            return;
        }
        read(false);
    }
};

RenderingSettingsWidget::RenderingSettingsWidget(EmbeddedViewport *viewport, QWidget *parent)
    : QWidget{parent} {
    if (viewport == nullptr)
        throw std::invalid_argument("RenderingSettingsWidget requires an embedded viewport");
    impl_ = std::make_unique<Impl>(*this, *viewport);
}

RenderingSettingsWidget::~RenderingSettingsWidget() = default;

void RenderingSettingsWidget::setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest) {
    impl_->client.setRenderConfigDigest(digest);
    impl_->digest_hub = std::move(digest);
    if (impl_->digest_hub)
        impl_->digest_hub->subscribe(this, [impl = impl_.get()](const std::string &moved) {
            impl->documentMoved(moved);
        });
}

void RenderingSettingsWidget::setRenderHistoryActions(const QList<QAction *> &actions) {
    addActions(actions);
}

void RenderingSettingsWidget::setEngineRestartAction(
    std::function<void(const QString &reason)> action) {
    impl_->engine_restart_action = std::move(action);
}

void RenderingSettingsWidget::engineRestartDeclined(const QString &message) {
    if (!std::exchange(impl_->restart_asked, false)) return;
    impl_->read(false);
    impl_->showInformation(message);
}

void RenderingSettingsWidget::refresh() { impl_->read(true); }

void RenderingSettingsWidget::addSection(QWidget *section) {
    if (section == nullptr || impl_->body_layout == nullptr) return;
    impl_->body_layout->insertWidget(impl_->section_at++, section);
}

} // namespace PelicanStudio
