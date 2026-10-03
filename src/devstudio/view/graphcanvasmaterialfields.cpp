#include "graphcanvasmaterialfields.hpp"

#include "graphcanvasrenderfields.hpp"
#include "passforms/attachmentopsform.hpp"
#include "passforms/materialsettingsform.hpp"
#include "studiotheme.hpp"

#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString qtext(const std::string &value) { return QString::fromStdString(value); }

const QString &materialPrefix() {
    static const QString prefix = QStringLiteral("pelican.graphCanvasMaterial");
    return prefix;
}

std::string stringOf(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

// The engine's spellings folded to one ("Clear" and "clear" are one choice).
std::string lowerOp(const Json &object, const char *key) {
    auto value = stringOf(object, key);
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "dontcare") value = "dont_care";
    return value;
}

// The pass's attachment ops as the document holds them: what the ops rows
// were loaded from.
Json opsOf(const Json &parameters) {
    return Json{{"load", lowerOp(parameters, "color_load_op")},
                {"store", lowerOp(parameters, "color_store_op")},
                {"clear", parameters.is_object() ? parameters.value("clear_color", Json(nullptr))
                                                 : Json(nullptr)},
                {"depth_load", lowerOp(parameters, "depth_load_op")},
                {"depth_store", lowerOp(parameters, "depth_store_op")}};
}

void putOrErase(Json &object, const char *key, const std::string &value) {
    if (value.empty())
        object.erase(key);
    else
        object[key] = value;
}

std::vector<std::string> namesOf(const Json &names) {
    std::vector<std::string> result;
    if (names.is_array())
        for (const auto &name : names)
            if (name.is_string()) result.push_back(name.get<std::string>());
    return result;
}

bool sharesAny(const std::vector<std::string> &tags, const std::vector<std::string> &wanted) {
    return std::ranges::any_of(wanted, [&](const std::string &tag) {
        return std::ranges::find(tags, tag) != tags.end();
    });
}

// The Pass Editor's parts are as wide as their longest choice; beside the
// canvas they take what the column gives them (as the Pass fields' do).
void compactCombos(QWidget &parent) {
    for (auto *combo : parent.findChildren<QComboBox *>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(6);
    }
    for (auto *line : parent.findChildren<QLineEdit *>()) line->setMinimumWidth(40);
}

QLabel *caption(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setFont(studioSmallFont());
    return label;
}

} // namespace

GraphCanvasMaterialPassFields::GraphCanvasMaterialPassFields(GraphCanvasModel &model,
                                                             std::function<void()> submitted,
                                                             QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)} {
    setObjectName(materialPrefix());
    held_ = std::make_unique<GraphCanvasHeldEdits>(model_, this);
    held_->setSent([this] {
        if (submitted_) submitted_();
    });
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    fields_ = new QWidget(this);
    fields_->setObjectName(materialPrefix() + QStringLiteral(".fields"));
    auto *form = new QFormLayout(fields_);
    form->setContentsMargins(0, 0, 0, 0);
    // Decision 1: the Pass Editor's two forms, under the Graph's names. The
    // Contract and Variant above are the Draw fields' (pick-only).
    settings_ = new MaterialSettingsForm(materialPrefix(), fields_);
    settings_->setContractAndVariantShown(false);
    form->addRow(settings_);
    ops_ = new AttachmentOpsForm(materialPrefix(), fields_);
    ops_->setDepthOpsShown(true);
    ops_->setToolTip(tr("Load, clear and store of the pass's attachments: its color_load_op, "
                        "color_store_op and clear_color (every colour socket alike) and its "
                        "depth_load_op and depth_store_op."));
    form->addRow(caption(tr("Attachments"), fields_), ops_);
    layout->addWidget(fields_);

    // Decision 4: what the pass draws, as the frame plan and the engine's
    // materials say.
    drawn_heading_ = caption(tr("Materials drawn"), this);
    drawn_heading_->setObjectName(materialPrefix() + QStringLiteral(".drawnHeading"));
    layout->addWidget(drawn_heading_);
    drawn_ = new QTreeWidget(this);
    drawn_->setObjectName(materialPrefix() + QStringLiteral(".drawn"));
    drawn_->setColumnCount(2);
    drawn_->setHeaderLabels({tr("Material"), tr("Tags")});
    drawn_->setRootIsDecorated(false);
    drawn_->setMaximumHeight(140);
    // UX-12c: the material's name takes the width; its draws or tags what
    // they need.
    drawn_->header()->setStretchLastSection(false);
    drawn_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    drawn_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    drawn_->setToolTip(tr("The frame plan's routes to this pass and the engine's materials its "
                          "Tags and Variant take. A row opens the Frame Plan's Materials."));
    layout->addWidget(drawn_);
    connect(drawn_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        if (item == nullptr || !materials_action_) return;
        const auto pass = item->data(0, Qt::UserRole).toString();
        if (!pass.isEmpty()) materials_action_(pass);
    });

    // A typed value (an output's name, a binding, a clear colour, Other) is
    // one edit when the typing stops, not one a key.
    typing_ = new QTimer(this);
    typing_->setSingleShot(true);
    typing_->setInterval(500);
    connect(typing_, &QTimer::timeout, this, [this] { commit(); });
    connect(settings_, &PassForm::changed, this, [this] { formChanged(); });
    connect(ops_, &PassForm::changed, this, [this] { formChanged(); });
    compactCombos(*fields_);
    hide();
}

GraphCanvasMaterialPassFields::~GraphCanvasMaterialPassFields() = default;

bool GraphCanvasMaterialPassFields::valuePending() const noexcept { return held_->pending(); }

void GraphCanvasMaterialPassFields::setPendingChanged(std::function<void()> changed) {
    held_->setChanged(std::move(changed));
}

void GraphCanvasMaterialPassFields::setMaterialsAction(
    std::function<void(const QString &pass)> action) {
    materials_action_ = std::move(action);
}

const GraphCanvasPlanPass *GraphCanvasMaterialPassFields::planPassOf(const GraphCanvasModel &model,
                                                                     const std::string &node) {
    for (const auto &pass : model.planPasses()) {
        if (pass.origin) {
            if (pass.origin->node == node && pass.origin->loop.empty() &&
                pass.origin->instance_path.empty())
                return &pass;
            continue;
        }
        if (pass.name == node || pass.authored_name == node) return &pass;
    }
    return nullptr;
}

Json GraphCanvasMaterialPassFields::withForms(const Json &parameters,
                                              const MaterialSettingsForm &settings,
                                              const AttachmentOpsForm &ops,
                                              const Json &loaded_ops) {
    Json result = parameters.is_object() ? parameters : Json::object();
    const Json values = settings.settings();
    for (const auto &key : MaterialSettingsForm::keys()) {
        // The Draw fields' own.
        if (key == "material_contract" || key == "material_variant") continue;
        if (values.contains(key))
            result[key] = values.at(key);
        else
            result.erase(key);
    }
    // UX-12c: a binding not yet told what it reads is no edit (the engine
    // refuses an empty target); it goes once its Reads is picked, as a value
    // still being typed does.
    for (const char *kind : {"screen_inputs", "surface_resources"})
        if (const auto found = result.find(kind); found != result.end() && found->is_object())
            for (const auto &[name, value] : found->items())
                if (value.is_string() && value.get<std::string>().empty())
                    throw std::runtime_error("the binding " + name + " reads nothing yet");
    const auto entries = ops.color();
    const auto rows = entries.is_array() ? entries : Json::array({entries});
    if (!rows.empty()) {
        // The row that moved says what the pass's ops become: a graph pass
        // has one load and one store for all its colour sockets.
        const auto loaded_load = loaded_ops.value("load", std::string{});
        const auto loaded_store = loaded_ops.value("store", std::string{});
        auto load = loaded_load;
        auto store = loaded_store;
        for (const auto &entry : rows) {
            const auto row_load = entry.is_object() ? lowerOp(entry, "load_op") : std::string{};
            const auto row_store = entry.is_object() ? lowerOp(entry, "store_op") : std::string{};
            if (row_load != loaded_load) load = row_load;
            if (row_store != loaded_store) store = row_store;
        }
        putOrErase(result, "color_load_op", load);
        putOrErase(result, "color_store_op", store);
        const auto loaded_clear = loaded_ops.value("clear", Json(nullptr));
        Json clear = loaded_clear;
        const auto colors = ops.clearColors();
        bool any = false;
        if (colors.is_object())
            for (const auto &[target, value] : colors.items()) {
                (void)target;
                any = true;
                if (value != loaded_clear) clear = value;
            }
        if (!any) clear = nullptr;
        if (clear.is_null())
            result.erase("clear_color");
        else
            result["clear_color"] = clear;
    }
    const auto depth = ops.depth();
    if (!depth.is_null()) {
        putOrErase(result, "depth_load_op", depth.is_object() ? lowerOp(depth, "load_op") : "");
        putOrErase(result, "depth_store_op", depth.is_object() ? lowerOp(depth, "store_op") : "");
    }
    return result;
}

void GraphCanvasMaterialPassFields::release() {
    // UX-9, decision 2: a value typed and waiting for the pause goes to the
    // pass it was typed for before the fields let go of it.
    if (!node_.empty() && typing_->isActive()) commit();
}

void GraphCanvasMaterialPassFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    if (node == nullptr || node->id != node_ || scope != scope_) release();
    if (node == nullptr || node->op != "render.material_pass") {
        node_.clear();
        scope_.clear();
        shown_.clear();
        drawn_shown_.clear();
        typing_->stop();
        hide();
        return;
    }
    const bool same = node->id == node_ && scope == scope_;
    node_ = node->id;
    scope_ = scope;
    const auto current = model_.nodeParameters(scope_, node_);
    QStringList parts{qtext(scope_), qtext(node_), qtext(current.dump())};
    for (const auto &list : {model_.materialTags(), model_.materialVariants(), model_.planTargets(),
                             model_.bufferChoices(scope_)}) {
        parts << QStringLiteral("|");
        for (const auto &entry : list) parts << qtext(entry);
    }
    for (const auto &port : node->inputs)
        if (port.connected) parts << qtext(port.port + "<" + port.source_node);
    // UX-12c: the binding names and the targets' types, once they arrive.
    for (const char *kind : {"screen_inputs", "surface_resources"}) {
        parts << QStringLiteral("|");
        for (const auto &[name, type] : model_.materialPassInputs(kind))
            parts << qtext(name) << qtext(type);
    }
    for (const auto &target : model_.planTargets())
        parts << qtext(model_.targetSemantic(target));
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    if (shown != shown_) {
        shown_ = shown;
        // What is being typed stays; a form that already says what the
        // document holds is not built again under the cursor.
        bool keep = false;
        if (same) {
            if (typing_->isActive()) {
                keep = true;
            } else {
                try {
                    keep = withForms(current, *settings_, *ops_, loaded_ops_) == current;
                } catch (const std::exception &) {
                    keep = true;
                }
            }
        }
        // The depth row names the image on the socket.
        std::string depth_image;
        for (const auto &port : node->inputs)
            if (port.port == "depth" && port.connected) depth_image = port.source_node;
        const bool depth_moved = depth_image != loaded_ops_.value("depth_image", std::string{});
        loaded_ops_["depth_image"] = depth_image;
        if (keep) {
            loading_ = true;
            offerInputNames();
            settings_->setTagCandidates(model_.materialTags());
            // The lists that came later than the document.
            if (auto variants = model_.materialVariants(); variants != variant_candidates_) {
                variant_candidates_ = std::move(variants);
                settings_->setVariantCandidates(variant_candidates_);
            }
            if (model_.planTargets() != target_candidates_) {
                target_candidates_ = model_.planTargets();
                settings_->setResourceCandidates(target_candidates_);
            }
            // UX-15a, decision 6: the render.buffer nodes GPU draws read.
            settings_->setBufferCandidates(model_.bufferChoices(scope_));
            loading_ = false;
            // One row's pick is the pass's op: read back, every row says it.
            auto ops_now = opsOf(current);
            ops_now["depth_image"] = depth_image;
            if (depth_moved || ops_now != loaded_ops_ ||
                GraphCanvasModel::materialPassColorSockets(current) != ops_sockets_)
                loadOps(current);
        } else {
            load(current);
        }
    }
    showDrawn(current);
    followEditable();
    show();
}

// UX-12c, decision 3: the binding names the engine lists (MP-1), and the type
// each target offered to read carries, so a row offers the targets its name
// can read.
void GraphCanvasMaterialPassFields::offerInputNames() {
    for (const char *kind : {"screen_inputs", "surface_resources"})
        settings_->setResourceNameCandidates(kind, model_.materialPassInputs(kind));
    std::map<std::string, std::string> types;
    for (const auto &target : model_.planTargets())
        if (auto semantic = model_.targetSemantic(target); !semantic.empty())
            types.emplace(target, std::move(semantic));
    settings_->setResourceCandidateTypes(std::move(types));
}

void GraphCanvasMaterialPassFields::load(const Json &parameters) {
    loading_ = true;
    offerInputNames();
    settings_->setTagCandidates(model_.materialTags());
    variant_candidates_ = model_.materialVariants();
    target_candidates_ = model_.planTargets();
    settings_->setVariantCandidates(variant_candidates_);
    settings_->setResourceCandidates(target_candidates_);
    settings_->setBufferCandidates(model_.bufferChoices(scope_));
    settings_->load(parameters);
    loading_ = false;
    loadOps(parameters);
}

void GraphCanvasMaterialPassFields::loadOps(const Json &parameters) {
    loading_ = true;
    ops_sockets_ = GraphCanvasModel::materialPassColorSockets(parameters);
    const auto depth_image = loaded_ops_.value("depth_image", std::string{});
    loaded_ops_ = opsOf(parameters);
    loaded_ops_["depth_image"] = depth_image;
    Json entries = Json::array();
    Json clear = Json::object();
    for (const auto &socket : ops_sockets_) {
        Json entry{{"target", socket}};
        if (!loaded_ops_["load"].get<std::string>().empty()) entry["load_op"] = loaded_ops_["load"];
        if (!loaded_ops_["store"].get<std::string>().empty()) entry["store_op"] = loaded_ops_["store"];
        entries.push_back(entry.size() == 1 ? Json(socket) : entry);
        if (loaded_ops_["clear"].is_array()) clear[socket] = loaded_ops_["clear"];
    }
    Json depth{{"target", depth_image.empty() ? std::string{"depth"} : depth_image}};
    if (const auto op = lowerOp(parameters, "depth_load_op"); !op.empty()) depth["load_op"] = op;
    if (const auto op = lowerOp(parameters, "depth_store_op"); !op.empty()) depth["store_op"] = op;
    ops_->load(entries, clear.empty() ? Json(nullptr) : clear,
               depth.size() == 1 ? depth.at("target") : depth);
    ops_->setDepthOpsShown(true);
    // The rows a load built are new widgets.
    compactCombos(*fields_);
    loading_ = false;
}

void GraphCanvasMaterialPassFields::showDrawnMaterials(const GraphCanvasPlanPass &pass) {
    const auto &drawn = *pass.drawn_materials;
    const auto materials = model_.materials();
    QStringList parts{QStringLiteral("drawn"), qtext(pass.name),
                      QString::number(static_cast<qulonglong>(drawn.frame)),
                      drawn.truncated ? QStringLiteral("t") : QStringLiteral("-")};
    for (const auto &material : drawn.materials)
        parts << qtext(material.id) << qtext(material.name)
              << QString::number(static_cast<qulonglong>(material.draws));
    for (const auto &material : materials)
        if (material.builtin) parts << qtext(material.id);
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    if (shown == drawn_shown_) return;
    drawn_shown_ = shown;

    const auto frame = static_cast<qulonglong>(drawn.frame);
    QString heading =
        tr("Materials drawn as of frame %1 -- %n draw(s)", nullptr, static_cast<int>(drawn.draws()))
            .arg(frame);
    if (drawn.truncated) heading += tr(" (first %1 shown)").arg(drawn.materials.size());
    drawn_heading_->setText(heading);
    drawn_heading_->setWordWrap(true);
    drawn_->clear();
    drawn_->setHeaderLabels({tr("Material"), tr("Draws")});
    drawn_->setToolTip(tr("What the pass drew in the frame the plan names: each material after "
                          "its variant and draw loop, and its draw commands over the pass's "
                          "views. A row opens the Frame Plan's Materials."));
    for (std::size_t index = 0; index < drawn.materials.size(); ++index) {
        const auto &material = drawn.materials[index];
        const auto known = std::ranges::find(materials, material.id, &GraphCanvasMaterial::id);
        const bool builtin = known != materials.end()
                                 ? known->builtin
                                 : material.name.starts_with("pelican.builtin.");
        const auto name = qtext(framePlanDrawnMaterialLabel(drawn, index));
        auto *item = new QTreeWidgetItem(drawn_);
        item->setText(0, builtin ? tr("%1 (built in)").arg(name) : name);
        item->setText(1, QString::number(static_cast<qulonglong>(material.draws)));
        item->setToolTip(0, qtext(material.id));
        item->setData(0, Qt::UserRole, qtext(pass.name));
    }
    if (drawn.materials.empty()) {
        auto *none = new QTreeWidgetItem(drawn_);
        none->setText(0, tr("It drew no material in frame %1").arg(frame));
        none->setFlags(none->flags() & ~Qt::ItemIsSelectable);
        // It still opens the Frame Plan's Materials (the routes to the pass).
        none->setData(0, Qt::UserRole, qtext(pass.name));
    }
}

void GraphCanvasMaterialPassFields::showDrawn(const Json &parameters) {
    const auto *pass = planPassOf(model_, node_);
    // UX-12c, decision 1: what the plan says it drew, once it says.
    if (pass != nullptr && pass->drawn_materials) {
        showDrawnMaterials(*pass);
        return;
    }
    // The filter and variant as the plan says (a draw_loop's `where` is the
    // pass's filter there), else as the document does.
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    if (pass != nullptr && (!pass->material_include.empty() || !pass->material_exclude.empty())) {
        include = pass->material_include;
        exclude = pass->material_exclude;
    } else {
        const auto filter = parameters.is_object()
                                ? parameters.value("material_filter", Json::object())
                                : Json::object();
        if (filter.is_object()) {
            include = namesOf(filter.value("include", Json::array()));
            exclude = namesOf(filter.value("exclude", Json::array()));
        }
    }
    const auto variant = pass != nullptr && !pass->material_variant.empty()
                             ? pass->material_variant
                             : stringOf(parameters, "material_variant");
    const auto materials = model_.materials();
    const std::string plan_name = pass != nullptr ? pass->name : node_;

    QStringList parts{qtext(plan_name), qtext(variant)};
    if (pass != nullptr) {
        for (const auto &route : pass->routes) parts << qtext(route);
        parts << qtext(pass->contract)
              << (pass->draw_count ? QString::number(*pass->draw_count) : QStringLiteral("-"));
    }
    parts << QStringLiteral("|");
    for (const auto &tag : include) parts << qtext(tag);
    parts << QStringLiteral("|");
    for (const auto &tag : exclude) parts << qtext(tag);
    for (const auto &material : materials) {
        parts << qtext(material.id) << qtext(material.name);
        for (const auto &tag : material.tags) parts << qtext(tag);
        for (const auto &each : material.variants) parts << qtext(each);
    }
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    if (shown == drawn_shown_) return;
    drawn_shown_ = shown;
    // UX-12c, decision 1: the plan may have been read before the frame that
    // says; it is asked for again a few times.
    if (pass != nullptr) model_.followDrawnMaterials();

    // The plan has not said what the pass drew (no frame since the pass was
    // published, or an engine before MP-1): the rows are those its Tags and
    // Variant take, which the heading says.
    drawn_heading_->setText(pass != nullptr && pass->draw_count
                                ? tr("Materials drawn (%n draw(s) this frame) -- those its Tags "
                                     "and Variant take; the engine has not said what it drew",
                                     nullptr, static_cast<int>(*pass->draw_count))
                                : tr("Materials drawn -- those its Tags and Variant take; the "
                                     "engine has not said what it drew"));
    drawn_heading_->setWordWrap(true);
    drawn_->clear();
    drawn_->setHeaderLabels({tr("Material"), tr("Tags")});
    drawn_->setToolTip(tr("The frame plan's routes to this pass and the engine's materials its "
                          "Tags and Variant take. A row opens the Frame Plan's Materials."));
    const auto row = [&](const QString &material, const QString &tags, const QString &tip) {
        auto *item = new QTreeWidgetItem(drawn_);
        item->setText(0, material);
        item->setText(1, tags);
        item->setToolTip(0, tip);
        item->setData(0, Qt::UserRole, qtext(plan_name));
        return item;
    };
    if (pass != nullptr)
        for (const auto &route : pass->routes)
            row(tr("route %1").arg(qtext(route)), qtext(pass->contract),
                tr("material_routing sends route %1 to this pass").arg(qtext(route)));
    int drawn = 0;
    for (const auto &material : materials) {
        if (!include.empty() && !sharesAny(material.tags, include)) continue;
        if (sharesAny(material.tags, exclude)) continue;
        if (!variant.empty() &&
            std::ranges::find(material.variants, variant) == material.variants.end())
            continue;
        QStringList tags;
        for (const auto &tag : material.tags) tags << qtext(tag);
        row(material.builtin ? tr("%1 (built in)").arg(qtext(material.name)) : qtext(material.name),
            tags.join(QStringLiteral(", ")), qtext(material.id));
        ++drawn;
    }
    if (drawn == 0) {
        auto *none = row(materials.empty() ? tr("The engine has listed no materials yet")
                                           : tr("No material the Tags and Variant take"),
                         QString{}, QString{});
        none->setFlags(none->flags() & ~Qt::ItemIsSelectable);
    }
}

void GraphCanvasMaterialPassFields::followEditable() {
    // Q-18: off from the moment an edit goes out until it is read back.
    fields_->setEnabled(model_.canEdit());
    held_->follow();
}

void GraphCanvasMaterialPassFields::formChanged() {
    if (loading_ || node_.empty()) return;
    QWidget *focus = QApplication::focusWidget();
    const bool typing = focus != nullptr && fields_->isAncestorOf(focus) &&
                        (qobject_cast<QLineEdit *>(focus) != nullptr ||
                         qobject_cast<QPlainTextEdit *>(focus) != nullptr);
    if (typing)
        typing_->start();
    else
        commit();
}

void GraphCanvasMaterialPassFields::commit() {
    typing_->stop();
    if (node_.empty()) return;
    const auto base = model_.nodeParameters(scope_, node_);
    if (!base.is_object()) return;
    Json changed;
    try {
        changed = withForms(base, *settings_, *ops_, loaded_ops_);
    } catch (const std::exception &) {
        // Other, or a clear colour, still being typed: nothing goes yet.
        return;
    }
    if (changed == base) return;
    // UX-9, decision 2: given while a save is in flight, it waits for it,
    // for this pass.
    if (held_->sendOrHold(heldParametersEdit(model_, scope_, node_, base, changed))) {
        if (submitted_) submitted_();
        return;
    }
    // Refused: the fields show the document again -- from the event loop,
    // for the row whose signal is being handled is one a load replaces.
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!node_.empty()) load(model_.nodeParameters(scope_, node_));
        },
        Qt::QueuedConnection);
}

} // namespace PelicanStudio
