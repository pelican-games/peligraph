#include "graphcanvascomputefields.hpp"

#include "featureparameterform.hpp"
#include "fullscreenshadertemplate.hpp"
#include "graphcanvasparameterfields.hpp"
#include "studiotheme.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <fstream>
#include <map>
#include <utility>

namespace PelicanStudio {
namespace {

QString qtext(const std::string &value) { return QString::fromStdString(value); }

QString computeFieldName(const QString &field) {
    return QStringLiteral("pelican.graphCanvasCompute.") + field;
}

// The shader a node runs as its Shader field names it: the project stem, or
// whatever `shader` holds (an engine shader).
std::string shownShader(const nlohmann::json &parameters) {
    auto stem = GraphCanvasModel::computeShaderStem(parameters);
    if (stem.empty() && parameters.is_object())
        if (const auto shader = parameters.find("shader"); shader != parameters.end() && shader->is_string())
            stem = shader->get<std::string>();
    return stem;
}

} // namespace

GraphCanvasComputeFields::GraphCanvasComputeFields(GraphCanvasModel &model,
                                                   std::function<void()> submitted, QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)} {
    setObjectName(QStringLiteral("pelican.graphCanvasCompute"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(form);
    shader_ = new QComboBox(this);
    shader_->setObjectName(computeFieldName(QStringLiteral("shader")));
    shader_->setToolTip(tr("The shader the task runs: the project's compute shaders and ray tracing "
                           "sets. Picking another one makes the node's ports the new shader's."));
    shader_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    shader_->setMinimumContentsLength(12);
    auto *caption = new QLabel(tr("Shader"), this);
    caption->setFont(studioSmallFont());
    // CT-6s, decision 3: New... beside it.
    auto *shader_row = new QWidget(this);
    auto *shader_layout = new QHBoxLayout(shader_row);
    shader_layout->setContentsMargins(0, 0, 0, 0);
    shader_layout->addWidget(shader_, 1);
    new_shader_ = new QPushButton(tr("New…"), shader_row);
    new_shader_->setObjectName(computeFieldName(QStringLiteral("newShader")));
    new_shader_->setToolTip(tr("Write a starting set of shaders into the project's shaders folder, "
                               "and run the task with it."));
    new_shader_->setCheckable(true);
    new_shader_->setEnabled(false);
    shader_layout->addWidget(new_shader_);
    form->addRow(caption, shader_row);
    connect(shader_, &QComboBox::currentIndexChanged, this, [this] { commit(); });
    template_row_ = new QWidget(this);
    template_row_->setObjectName(computeFieldName(QStringLiteral("shaderTemplateRow")));
    auto *template_layout = new QGridLayout(template_row_);
    template_layout->setContentsMargins(0, 0, 0, 0);
    template_kind_ = new QComboBox(template_row_);
    template_kind_->setObjectName(computeFieldName(QStringLiteral("shaderTemplateKind")));
    template_kind_->addItem(qtext(std::string{RayTracingTemplateLabel}), QStringLiteral("ray_tracing"));
    template_kind_->setToolTip(tr("Ray tracing: shaders/<name>.rgen, .rmiss and .rchit. One ray per "
                                  "pixel of the image the task writes, coloured by the normal of the "
                                  "surface it hits."));
    template_name_ = new QLineEdit(template_row_);
    template_name_->setObjectName(computeFieldName(QStringLiteral("shaderTemplateName")));
    template_name_->setToolTip(tr("The files' name in the project's shaders folder (a free one is "
                                  "proposed). Letters, digits, '_' and '-'."));
    create_shader_ = new QPushButton(tr("Create"), template_row_);
    create_shader_->setObjectName(computeFieldName(QStringLiteral("createShader")));
    template_notice_ = new QLabel(template_row_);
    template_notice_->setObjectName(computeFieldName(QStringLiteral("shaderTemplateNotice")));
    template_notice_->setFont(studioSmallFont());
    template_notice_->setWordWrap(true);
    template_notice_->hide();
    template_layout->addWidget(template_kind_, 0, 0, 1, 2);
    template_layout->addWidget(template_name_, 1, 0);
    template_layout->addWidget(create_shader_, 1, 1);
    template_layout->addWidget(template_notice_, 2, 0, 1, 2);
    form->addRow(QString{}, template_row_);
    form->setRowVisible(template_row_, false);
    connect(new_shader_, &QPushButton::toggled, this, [this, form](bool on) {
        form->setRowVisible(template_row_, on);
        if (on) {
            templateNotice({}, false);
            proposeTemplateName();
        }
    });
    connect(create_shader_, &QPushButton::clicked, this, [this] { createTemplate(); });
    connect(template_name_, &QLineEdit::returnPressed, this, [this] { createTemplate(); });
    auto *status_row = new QHBoxLayout;
    status_row->setContentsMargins(0, 0, 0, 0);
    status_ = new QLabel(this);
    status_->setObjectName(computeFieldName(QStringLiteral("status")));
    status_->setFont(studioSmallFont());
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_row->addWidget(status_, 1);
    copy_ = new QPushButton(tr("Copy message"), this);
    copy_->setObjectName(computeFieldName(QStringLiteral("copyMessage")));
    copy_->setToolTip(tr("Copy the engine's whole message"));
    connect(copy_, &QPushButton::clicked, this, [this] {
        if (auto *clipboard = QApplication::clipboard()) clipboard->setText(message_);
    });
    status_row->addWidget(copy_, 0, Qt::AlignTop);
    layout->addLayout(status_row);
    schedule_ = new GraphCanvasScheduleRow(
        model_, QStringLiteral("pelican.graphCanvasCompute"),
        [this] {
            if (submitted_) submitted_();
        },
        this);
    auto *schedule_caption = new QLabel(tr("Schedule"), this);
    schedule_caption->setFont(studioSmallFont());
    form->addRow(schedule_caption, schedule_);
    // RTL-2s, decision 1.
    ray_tracing_ = new GraphCanvasRayTracingFields(
        model_,
        [this] {
            if (submitted_) submitted_();
        },
        this);
    layout->addWidget(ray_tracing_);
    status_->hide();
    copy_->hide();
    hide();
}

void GraphCanvasComputeFields::setOpenRayLayout(std::function<void()> open) {
    ray_tracing_->setOpenRayLayout(std::move(open));
}

void GraphCanvasComputeFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    if (node == nullptr || node->op != "render.compute") {
        node_.clear();
        scope_.clear();
        shown_.clear();
        ray_tracing_->showNode(nullptr, {});
        hide();
        return;
    }
    node_ = node->id;
    scope_ = scope;
    const auto parameters = model_.nodeParameters(scope_, node_);
    const auto reference = shownShader(parameters);
    // Its ports and whether the engine could read it: asked once (the
    // canvas's poll sends it); a stem asked already is not asked again.
    if (reference.starts_with("project://") && !model_.shaderInterface(reference))
        model_.askShaderInterface(reference);
    QStringList parts{qtext(scope_), qtext(node_), qtext(reference),
                      QString::number(model_.shaderInterfaceRevision())};
    for (const auto &choice : model_.computeShaderChoices()) parts << qtext(choice);
    parts << QStringLiteral("|");
    for (const auto &choice : model_.projectRayTracingChoices()) parts << qtext(choice);
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    if (shown != shown_) {
        shown_ = shown;
        load(reference);
    }
    // PS-1s: when it last ran comes with the plan, not the document.
    schedule_->showNode(scope_, node_);
    // RTL-2s, decision 1: its ray tracing, in the layout's terms.
    ray_tracing_->showNode(node, scope);
    followEditable();
    show();
}

void GraphCanvasComputeFields::followEditable() {
    shader_->setEnabled(model_.canEdit());
    new_shader_->setEnabled(model_.canEdit() && !project_root_.empty());
    create_shader_->setEnabled(model_.canEdit() && !project_root_.empty());
    schedule_->followEditable();
    ray_tracing_->followEditable();
}

void GraphCanvasComputeFields::setProjectRoot(const std::filesystem::path &root) {
    ray_tracing_->setProjectRoot(root);
    if (project_root_ == root) return;
    project_root_ = root;
    auto_template_name_.clear();
    template_name_->clear();
    followEditable();
}

void GraphCanvasComputeFields::proposeTemplateName() {
    if (project_root_.empty()) return;
    // A name typed over the proposal is the author's; the proposal is a free
    // one, so nothing has to be typed.
    const auto current = template_name_->text().trimmed().toStdString();
    if (!current.empty() && current != auto_template_name_) return;
    auto_template_name_ = rayTracingTemplateName(project_root_);
    template_name_->setText(qtext(auto_template_name_));
}

void GraphCanvasComputeFields::templateNotice(const QString &text, bool failure) {
    template_notice_->setStyleSheet(failure ? QStringLiteral("color: #d94c3d;") : QString{});
    template_notice_->setText(text);
    template_notice_->setVisible(!text.isEmpty());
}

void GraphCanvasComputeFields::createTemplate() {
    if (node_.empty() || project_root_.empty() || !model_.canEdit()) return;
    const auto name = template_name_->text().trimmed().toStdString();
    if (!isValidShaderTemplateName(name)) {
        templateNotice(tr("A shader name is letters, digits, '_' and '-'."), true);
        return;
    }
    RayTracingTemplateWrite written;
    try {
        // RTL-2s, decision 2: in a project with a ray group layout, the set
        // is the layout's form.
        if (const auto layout = rayLayoutTemplateNamesFor(model_))
            written = writeRayTracingTemplate(project_root_, name, *layout);
        else
            written = writeRayTracingTemplate(project_root_, name);
    } catch (const std::exception &error) {
        templateNotice(tr("Could not write the shaders: %1").arg(QString::fromUtf8(error.what())), true);
        return;
    }
    const auto files = QStringLiteral("shaders/%1.rgen, .rmiss and .rchit").arg(qtext(name));
    if (!written.created) {
        templateNotice(tr("shaders/%1 already has a .rgen, .rmiss or .rchit and was left as it is: "
                          "name the new set otherwise.")
                           .arg(qtext(name)),
                       true);
        return;
    }
    if (shaders_changed_) shaders_changed_();
    // The task traces with it once the engine has read its ports.
    if (!model_.setComputeShader(scope_, node_, written.reference)) {
        templateNotice(tr("Wrote %1, but the task could not take it now: pick it from the Shader "
                          "list.")
                           .arg(files),
                       true);
        return;
    }
    // Used: the next New... proposes the next free name.
    auto_template_name_ = name;
    templateNotice(tr("Wrote %1; the task traces with %2.").arg(files, qtext(written.reference)), false);
    shown_.clear();
    if (submitted_) submitted_();
}

void GraphCanvasComputeFields::load(const std::string &reference) {
    {
        const QSignalBlocker block{shader_};
        shader_->clear();
        for (const auto &choice : model_.computeShaderChoices())
            shader_->addItem(qtext(choice), qtext(choice));
        for (const auto &choice : model_.projectRayTracingChoices())
            shader_->addItem(tr("%1  (ray tracing)").arg(qtext(choice)), qtext(choice));
        // What the document holds is shown even when the list lacks it.
        int index = shader_->findData(qtext(reference));
        if (index < 0 && !reference.empty()) {
            shader_->insertItem(0, qtext(reference), qtext(reference));
            index = 0;
        }
        shader_->setCurrentIndex(index);
    }
    syncStatus(reference);
}

void GraphCanvasComputeFields::syncStatus(const std::string &reference) {
    message_.clear();
    QString line;
    if (reference.starts_with("project://")) {
        const auto known = model_.shaderInterface(reference);
        if (!known || known->status == "building") {
            line = tr("Reading the shader's ports...");
        } else if (!known->ready()) {
            line = tr("The engine could not read %1: %2").arg(qtext(reference), qtext(known->error));
            message_ = qtext(known->error);
        } else if (!known->reflection_error.empty()) {
            line = qtext(GraphCanvasModel::reflectionErrorSentence(reference, known->reflection_error));
            message_ = qtext(known->reflection_error);
        }
    }
    status_->setText(line);
    status_->setStyleSheet(message_.isEmpty() ? QStringLiteral("color: #7f8c8d;")
                                              : QStringLiteral("color: #c0392b;"));
    status_->setVisible(!line.isEmpty());
    copy_->setVisible(!message_.isEmpty());
}

void GraphCanvasComputeFields::commit() {
    const auto picked = shader_->currentData().toString().toStdString();
    if (picked.empty() || node_.empty()) return;
    if (shownShader(model_.nodeParameters(scope_, node_)) == picked) return;
    if (model_.setComputeShader(scope_, node_, picked)) {
        if (submitted_) submitted_();
        return;
    }
    // Refused here: the field shows the document again.
    shown_.clear();
    load(shownShader(model_.nodeParameters(scope_, node_)));
}

// --- RTL-2s: a ray tracing task of a project with a ray group layout.

namespace {

using Json = nlohmann::json;

std::string textOf(const Json &value, const char *key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

QString rayFieldName(const QString &field) { return QStringLiteral("pelican.graphCanvasRayTracing.") + field; }

constexpr auto kRayInvalidStyle = "QComboBox, QCheckBox { border: 1px solid #d94c3d; }";

// The layout's form of a value: a shader's reference, or none.
std::string shaderOf(const Json &value) { return value.is_string() ? value.get<std::string>() : std::string{}; }

} // namespace

bool rayLayoutSectionShown(const GraphCanvasModel &model, const Json &parameters) {
    const auto &read = model.rayLayoutRead();
    return parameters.is_object() && parameters.contains("ray_tracing") && read && read->layout.usable() &&
           !rayLayoutRayTypes(read->layout.document).empty();
}

std::optional<RayLayoutTemplateNames> rayLayoutTemplateNamesFor(const GraphCanvasModel &model) {
    const auto &read = model.rayLayoutRead();
    if (!read || !read->layout.usable()) return std::nullopt;
    return rayLayoutTemplateNamesFromDocument(read->layout.document);
}

struct GraphCanvasRayTracingFields::Parts {
    QVBoxLayout *outer = nullptr;
    // Without a layout: one line and Enable hit groups.
    QWidget *no_layout = nullptr;
    QLabel *no_layout_text = nullptr;
    QPushButton *enable = nullptr;
    // The rows, made again whenever the layout, the ray tracing or the
    // project's shaders change.
    QWidget *body = nullptr;
    QLabel *notice = nullptr;
    QLabel *problem = nullptr;
    QComboBox *raygen = nullptr;
    QLabel *traced_label = nullptr;
    std::vector<std::pair<std::string, QCheckBox *>> traced;
    std::map<std::string, QComboBox *, std::less<>> misses;
    struct Cell {
        std::string ray_type;
        std::string group;
        std::map<std::string, QComboBox *, std::less<>> stages;
    };
    std::vector<Cell> cells;
    std::vector<std::pair<std::string, GraphParameterEditor *>> generic;
    // RTL-2s-c: the raygen's switches.
    QCheckBox *trace_second = nullptr;
    QCheckBox *call_callable = nullptr;
    // The layout as get_ray_layout answered it, and its names.
    RayLayoutRead read;
    std::vector<std::string> ray_types;
    std::vector<std::string> misses_names;
    Json ray_tracing = Json::object();
    std::vector<std::pair<std::string, std::string>> shaders;
};

GraphCanvasRayTracingFields::GraphCanvasRayTracingFields(GraphCanvasModel &model, std::function<void()> submitted,
                                                         QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)}, parts_{std::make_unique<Parts>()} {
    setObjectName(QStringLiteral("pelican.graphCanvasRayTracing"));
    auto &parts = *parts_;
    parts.outer = new QVBoxLayout(this);
    parts.outer->setContentsMargins(0, 4, 0, 0);
    parts.outer->setSpacing(3);
    parts.no_layout = new QWidget(this);
    auto *line = new QHBoxLayout(parts.no_layout);
    line->setContentsMargins(0, 0, 0, 0);
    parts.no_layout_text = new QLabel(
        tr("Every object hits the same closest-hit shader here. To give objects hit shaders of their own, "
           "enable hit groups."),
        parts.no_layout);
    parts.no_layout_text->setObjectName(rayFieldName(QStringLiteral("noLayout")));
    parts.no_layout_text->setFont(studioSmallFont());
    parts.no_layout_text->setWordWrap(true);
    line->addWidget(parts.no_layout_text, 1);
    parts.enable = new QPushButton(tr("Enable hit groups…"), parts.no_layout);
    parts.enable->setObjectName(rayFieldName(QStringLiteral("enableHitGroups")));
    parts.enable->setToolTip(tr("Open the Ray layout, where hit groups are enabled."));
    connect(parts.enable, &QPushButton::clicked, this, [this] {
        if (open_ray_layout_) open_ray_layout_();
    });
    line->addWidget(parts.enable, 0, Qt::AlignTop);
    parts.outer->addWidget(parts.no_layout);
    parts.no_layout->hide();
    parts.notice = new QLabel(this);
    parts.notice->setObjectName(rayFieldName(QStringLiteral("notice")));
    parts.notice->setFont(studioSmallFont());
    parts.notice->setWordWrap(true);
    parts.notice->hide();
    parts.problem = new QLabel(this);
    parts.problem->setObjectName(rayFieldName(QStringLiteral("problem")));
    parts.problem->setFont(studioSmallFont());
    parts.problem->setWordWrap(true);
    parts.problem->setStyleSheet(QStringLiteral("color: #d94c3d;"));
    parts.problem->setTextInteractionFlags(Qt::TextSelectableByMouse);
    parts.problem->hide();
    parts.outer->addWidget(parts.problem);
    parts.outer->addWidget(parts.notice);
    hide();
}

GraphCanvasRayTracingFields::~GraphCanvasRayTracingFields() = default;

void GraphCanvasRayTracingFields::setOpenRayLayout(std::function<void()> open) {
    open_ray_layout_ = std::move(open);
    parts_->enable->setEnabled(static_cast<bool>(open_ray_layout_));
}

void GraphCanvasRayTracingFields::setProjectRoot(const std::filesystem::path &root) {
    if (project_root_ == root) return;
    project_root_ = root;
    shown_.clear();
}

void GraphCanvasRayTracingFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    auto &parts = *parts_;
    const auto parameters = node != nullptr ? model_.nodeParameters(scope, node->id) : Json{};
    const bool tracing = node != nullptr && node->op == "render.compute" && parameters.is_object() &&
                         parameters.contains("ray_tracing") && parameters.at("ray_tracing").is_object();
    if (!tracing) {
        node_.clear();
        scope_.clear();
        shown_.clear();
        hide();
        return;
    }
    if (node_ != node->id || scope_ != scope) parts.notice->hide();
    node_ = node->id;
    scope_ = scope;
    // The layout's names come with the authoring context, asked for once per
    // reading of the document.
    model_.askRenderAuthoringForChoices();
    // RTL-2s-c: get_ray_layout, as the Ray layout section reads it.
    const auto &known_read = model_.rayLayoutRead();
    const bool known = known_read.has_value();
    const RayLayoutRead read = known ? *known_read : RayLayoutRead{};
    const bool present = read.layout.usable() && !rayLayoutRayTypes(read.layout.document).empty();
    std::vector<std::pair<std::string, std::string>> shaders;
    for (const auto &[reference, stage] : projectShaderReferences(project_root_))
        shaders.emplace_back(reference.toStdString(), stage.toStdString());
    for (const auto &shader : model_.engineShaders()) shaders.emplace_back(shader.reference, shader.stage);
    const auto &ray_tracing = parameters.at("ray_tracing");
    const auto &refusal = model_.parameterRefusal();
    QStringList key{qtext(scope), qtext(node->id), qtext(ray_tracing.dump()),
                    qtext(read.layout.digest + read.layout.document.dump() + read.mode.value_or("")),
                    qtext(project_root_.generic_string()),
                    refusal ? qtext(refusal->node + "|" + refusal->parameter + "|" + refusal->sentence)
                            : QString{}};
    for (const auto &[reference, stage] : shaders) key << qtext(reference + ":" + stage);
    const auto shown = key.join(QChar{0x1f}).toStdString();
    if (!known) {
        // Not read yet: nothing to say until it is.
        parts.no_layout->hide();
        if (parts.body != nullptr) parts.body->hide();
        hide();
        return;
    }
    if (!present) {
        // RTL-2s, decision 1: the form without a layout stays the generic
        // fields'; one line points at hit groups.
        if (parts.body != nullptr) parts.body->hide();
        const bool broken = read.layout.named;
        parts.no_layout_text->setText(
            !broken ? tr("Every object hits the same closest-hit shader here. To give objects hit shaders of "
                         "their own, enable hit groups.")
                    : tr("The ray group layout cannot be read: %1")
                          .arg(qtext(read.layout.error_message.empty() ? read.layout.error_code
                                                                       : read.layout.error_message)));
        parts.enable->setVisible(!broken);
        parts.no_layout->show();
        parts.problem->hide();
        shown_.clear();
        show();
        return;
    }
    parts.no_layout->hide();
    if (shown != shown_) {
        shown_ = shown;
        parts.shaders = std::move(shaders);
        rebuild(read, ray_tracing);
    }
    showProblem();
    followEditable();
    show();
}

Json GraphCanvasRayTracingFields::current() const { return parts_->ray_tracing; }

void GraphCanvasRayTracingFields::followEditable() {
    if (parts_->body != nullptr) parts_->body->setEnabled(model_.canEdit() && !node_.empty());
}

void GraphCanvasRayTracingFields::send(Json ray_tracing) {
    if (node_.empty() || !model_.canEdit()) return;
    // The layout's form: the form without one is refused beside it.
    ray_tracing.erase("miss");
    ray_tracing.erase("closesthit");
    auto parameters = model_.nodeParameters(scope_, node_);
    if (!parameters.is_object()) return;
    parameters["ray_tracing"] = ray_tracing;
    parts_->ray_tracing = ray_tracing;
    if (model_.setNodeParameters(scope_, node_, parameters)) {
        if (submitted_) submitted_();
        return;
    }
    // Not sent: the rows show the document again.
    shown_.clear();
}

void GraphCanvasRayTracingFields::createShader(const RayLayoutTemplatePlace &place,
                                               const std::function<void(const std::string &)> &put) {
    auto &parts = *parts_;
    const auto names = rayLayoutTemplateNamesFor(model_);
    if (!names || project_root_.empty()) {
        parts.notice->setText(tr("No project to write the shader into."));
        parts.notice->show();
        return;
    }
    std::string base = node_;
    switch (place.stage) {
    case RayLayoutTemplateStage::raygen: break;
    case RayLayoutTemplateStage::miss: base += "_" + place.miss; break;
    case RayLayoutTemplateStage::closest_hit:
    case RayLayoutTemplateStage::any_hit: base += "_" + place.ray_type + "_" + place.group; break;
    case RayLayoutTemplateStage::intersection: base += "_" + place.group; break;
    case RayLayoutTemplateStage::callable: base += "_callable"; break;
    }
    RayLayoutTemplateWrite written;
    try {
        const auto name = rayLayoutTemplateName(project_root_, place.stage, base);
        written = writeRayLayoutTemplate(project_root_, name, *names, place);
        parts.notice->setText(tr("Wrote shaders/%1%2.")
                                  .arg(qtext(name), qtext(std::string{rayLayoutTemplateExtension(place.stage)})));
    } catch (const std::exception &error) {
        parts.notice->setText(tr("Could not write the shader: %1").arg(QString::fromUtf8(error.what())));
        parts.notice->show();
        return;
    }
    parts.notice->show();
    if (!written.created) return;
    if (shaders_changed_) shaders_changed_();
    put(written.reference);
}

void GraphCanvasRayTracingFields::showProblem() {
    auto &parts = *parts_;
    const auto &refusal = model_.parameterRefusal();
    const bool ours = refusal && refusal->node == node_ && refusal->scope == scope_ &&
                      (refusal->parameter == "ray_tracing" || refusal->parameter.starts_with("ray_tracing."));
    parts.problem->setText(ours ? qtext(refusal->sentence) : QString{});
    parts.problem->setVisible(ours && !refusal->sentence.empty());
    if (!ours) return;
    // The place the engine named: ray_tracing.<field>[.<name>[.<name>]].
    std::vector<std::string> steps;
    for (std::size_t begin = 0; begin <= refusal->parameter.size();) {
        const auto end = std::min(refusal->parameter.find('.', begin), refusal->parameter.size());
        steps.push_back(refusal->parameter.substr(begin, end - begin));
        begin = end + 1;
    }
    const auto step = [&](std::size_t index) { return index < steps.size() ? steps[index] : std::string{}; };
    const auto red = [](QWidget *widget) {
        if (widget == nullptr) return;
        widget->setStyleSheet(QString::fromLatin1(kRayInvalidStyle));
        widget->setProperty("pelicanInvalid", true);
    };
    if (step(1) == "raygen") red(parts.raygen);
    if (step(1) == "traced_ray_types") {
        for (const auto &[type, check] : parts.traced)
            if (step(2).empty() || step(2) == type) red(check);
        if (parts.traced_label != nullptr) parts.traced_label->setStyleSheet(QStringLiteral("color: #d94c3d;"));
    }
    if (step(1) == "misses") {
        if (const auto found = parts.misses.find(step(2)); found != parts.misses.end()) red(found->second);
        else
            for (const auto &[miss, combo] : parts.misses) red(combo);
    }
    if (step(1) == "hit_shaders")
        for (auto &cell : parts.cells) {
            if ((!step(2).empty() && cell.ray_type != step(2)) || (!step(3).empty() && cell.group != step(3)))
                continue;
            for (const auto &[stage, combo] : cell.stages)
                if (step(4).empty() ? stage == "closesthit" || stage == "intersection" : stage == step(4))
                    red(combo);
        }
}

void GraphCanvasRayTracingFields::rebuild(const RayLayoutRead &read, const Json &ray_tracing) {
    auto &parts = *parts_;
    if (parts.body != nullptr) {
        // From the event loop: a field's own signal may be what led here.
        parts.body->hide();
        for (auto *named : parts.body->findChildren<QWidget *>()) named->setObjectName({});
        parts.body->setObjectName({});
        parts.body->deleteLater();
    }
    parts.read = read;
    const auto &document = read.layout.document;
    parts.ray_types = rayLayoutRayTypes(document);
    parts.misses_names = rayLayoutMisses(document);
    const auto groups = rayLayoutGroups(document);
    const auto default_group = rayLayoutDefaultGroup(document);
    parts.ray_tracing = ray_tracing.is_object() ? ray_tracing : Json::object();
    parts.trace_second = nullptr;
    parts.call_callable = nullptr;
    parts.raygen = nullptr;
    parts.traced_label = nullptr;
    parts.traced.clear();
    parts.misses.clear();
    parts.cells.clear();
    parts.generic.clear();
    parts.body = new QWidget(this);
    parts.body->setObjectName(rayFieldName(QStringLiteral("body")));
    parts.outer->insertWidget(1, parts.body);
    auto *column = new QVBoxLayout(parts.body);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);
    auto *heading = new QLabel(tr("Ray tracing (ray layout %1)").arg(qtext(read.layout.reference)), parts.body);
    heading->setObjectName(rayFieldName(QStringLiteral("heading")));
    heading->setFont(studioSmallFont());
    column->addWidget(heading);
    if (read.restartPending()) {
        auto *waiting = new QLabel(tr("The engine runs without the layout until it restarts; the task is "
                                      "written in the layout's form."),
                                   parts.body);
        waiting->setObjectName(rayFieldName(QStringLiteral("waitingForRestart")));
        waiting->setFont(studioSmallFont());
        waiting->setWordWrap(true);
        column->addWidget(waiting);
    }
    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    column->addLayout(form);
    const auto caption = [&](const QString &text) {
        auto *label = new QLabel(text, parts.body);
        label->setFont(studioSmallFont());
        return label;
    };
    const auto rt = parts.ray_tracing;

    // A shader combo of one stage: the project's files and the engine's of
    // it, what the document holds even when they do not list it.
    const auto shaderCombo = [&](const QString &name, const std::string &stage, const std::string &value,
                                 bool required, const QString &none) {
        auto *combo = new QComboBox(parts.body);
        combo->setObjectName(name);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(10);
        if (!required || value.empty()) combo->addItem(none, QString{});
        bool listed = false;
        for (const auto &[reference, shader_stage] : parts.shaders) {
            if (shader_stage != stage || combo->findData(qtext(reference)) >= 0) continue;
            combo->addItem(qtext(reference), qtext(reference));
            listed = listed || reference == value;
        }
        if (!value.empty() && !listed) combo->insertItem(required ? 0 : 1, qtext(value), qtext(value));
        combo->setCurrentIndex(std::max(0, combo->findData(qtext(value))));
        if (required && value.empty()) {
            combo->setStyleSheet(QString::fromLatin1(kRayInvalidStyle));
            combo->setProperty("pelicanInvalid", true);
        }
        return combo;
    };
    const auto newButton = [&](const QString &name) {
        auto *button = new QPushButton(tr("New…"), parts.body);
        button->setObjectName(name);
        button->setAutoDefault(false);
        button->setToolTip(tr("Write a starting shader of this stage for this place into the project's shaders "
                              "folder, and use it here."));
        button->setEnabled(!project_root_.empty());
        return button;
    };
    const auto row = [&](QComboBox *combo, QPushButton *button) {
        auto *box = new QWidget(parts.body);
        auto *line = new QHBoxLayout(box);
        line->setContentsMargins(0, 0, 0, 0);
        line->addWidget(combo, 1);
        if (button != nullptr) line->addWidget(button);
        return box;
    };
    // A change: the ray tracing with it, sent.
    const auto later = [this](std::function<Json(Json)> change) {
        QTimer::singleShot(0, this, [this, change = std::move(change)] { send(change(current())); });
    };

    // Raygen.
    parts.raygen = shaderCombo(rayFieldName(QStringLiteral("raygen")), "raygen", textOf(rt, "raygen"), true,
                               tr("(choose a raygen shader)"));
    auto *raygen_new = newButton(rayFieldName(QStringLiteral("raygen.new")));
    form->addRow(caption(tr("Raygen")), row(parts.raygen, raygen_new));
    connect(parts.raygen, &QComboBox::currentIndexChanged, this, [this, later] {
        const auto picked = parts_->raygen->currentData().toString().toStdString();
        if (picked.empty()) return;
        later([picked](Json value) {
            value["raygen"] = picked;
            return value;
        });
    });
    connect(raygen_new, &QPushButton::clicked, this, [this, later] {
        createShader({RayLayoutTemplateStage::raygen, {}, {}, {}}, [later](const std::string &reference) {
            later([reference](Json value) {
                value["raygen"] = reference;
                return value;
            });
        });
    });

    // Traced ray types.
    std::vector<std::string> traced;
    for (const auto &type : rt.value("traced_ray_types", Json::array()))
        if (type.is_string()) traced.push_back(type.get<std::string>());
    auto *checks = new QWidget(parts.body);
    auto *checks_line = new QHBoxLayout(checks);
    checks_line->setContentsMargins(0, 0, 0, 0);
    for (const auto &type : parts.ray_types) {
        auto *check = new QCheckBox(qtext(type), checks);
        check->setObjectName(rayFieldName(QStringLiteral("traced.") + qtext(type)));
        check->setChecked(std::ranges::find(traced, type) != traced.end());
        checks_line->addWidget(check);
        parts.traced.emplace_back(type, check);
        connect(check, &QCheckBox::toggled, this, [this, later, type](bool on) {
            const auto order = parts_->ray_types;
            // RTL-3s, decision 5 (engine EB-3): checked, the ray type's row
            // is whole in the same edit -- a cell per hit group, in its own
            // payload (rayTracedRowFill writes the template's closest hits
            // it needs first); unchecked, its row goes with it.
            Json row = Json::object();
            if (on) {
                auto fill = rayTracedRowFill(current(), type, parts_->read.layout.document, project_root_);
                for (const auto &write : fill.writes) {
                    std::error_code ignored;
                    std::filesystem::create_directories(write.path.parent_path(), ignored);
                    std::ofstream output{write.path, std::ios::binary | std::ios::trunc};
                    output.write(write.content.data(), static_cast<std::streamsize>(write.content.size()));
                    if (!output) fill.notes.push_back("Could not write " + write.path.filename().string() + ".");
                }
                if (!fill.writes.empty() && shaders_changed_) shaders_changed_();
                QStringList lines;
                for (const auto &note : fill.notes) lines << qtext(note);
                parts_->notice->setText(lines.join(QLatin1Char('\n')));
                parts_->notice->setVisible(!lines.isEmpty());
                row = std::move(fill.row);
            }
            later([order, type, on, row](Json value) {
                std::vector<std::string> now;
                for (const auto &each : value.value("traced_ray_types", Json::array()))
                    if (each.is_string()) now.push_back(each.get<std::string>());
                auto hits = value.value("hit_shaders", Json::object());
                if (!hits.is_object()) hits = Json::object();
                if (on && std::ranges::find(now, type) == now.end()) {
                    hits[type] = row;
                    now.push_back(type);
                } else if (!on) {
                    std::erase(now, type);
                    hits.erase(type);
                }
                auto ordered = Json::array();
                for (const auto &each : order)
                    if (std::ranges::find(now, each) != now.end()) ordered.push_back(each);
                value["traced_ray_types"] = ordered;
                value["hit_shaders"] = hits;
                return value;
            });
        });
    }
    checks_line->addStretch(1);
    parts.traced_label = caption(tr("Traced ray types"));
    form->addRow(parts.traced_label, checks);

    // Miss shaders: one row per miss of the layout.
    const auto misses = rt.value("misses", Json::object());
    for (const auto &miss : parts.misses_names) {
        const auto value = misses.is_object() ? shaderOf(misses.value(miss, Json{})) : std::string{};
        auto *combo = shaderCombo(rayFieldName(QStringLiteral("miss.") + qtext(miss)), "miss", value, true,
                                  tr("(choose a miss shader)"));
        auto *button = newButton(rayFieldName(QStringLiteral("miss.") + qtext(miss) + QStringLiteral(".new")));
        parts.misses.emplace(miss, combo);
        form->addRow(caption(tr("Miss %1").arg(qtext(miss))), row(combo, button));
        const auto put = [later, miss](const std::string &reference) {
            later([miss, reference](Json value) {
                auto all = value.value("misses", Json::object());
                if (!all.is_object()) all = Json::object();
                if (reference.empty()) all.erase(miss);
                else all[miss] = reference;
                value["misses"] = all;
                return value;
            });
        };
        connect(combo, &QComboBox::currentIndexChanged, this, [combo, put] {
            const auto picked = combo->currentData().toString().toStdString();
            if (!picked.empty()) put(picked);
        });
        connect(button, &QPushButton::clicked, this,
                [this, miss, put] { createShader({RayLayoutTemplateStage::miss, {}, {}, miss}, put); });
    }

    // The hit shaders' grid: a row per traced ray type, a column per group.
    column->addWidget(caption(tr("Hit shaders (ray type × hit group)")));
    auto *grid_box = new QWidget(parts.body);
    grid_box->setObjectName(rayFieldName(QStringLiteral("hitGrid")));
    auto *grid = new QGridLayout(grid_box);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(4);
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const auto &group = groups[g].name;
        const auto &type = groups[g].type;
        auto *label = caption(qtext(group) + (type == "procedural" ? tr(" (procedural)") : QString{}) +
                              (group == default_group ? tr(" · default") : QString{}));
        label->setObjectName(rayFieldName(QStringLiteral("hitGroup.") + qtext(group)));
        grid->addWidget(label, 0, static_cast<int>(g) + 1);
    }
    const auto hits = rt.value("hit_shaders", Json::object());
    int grid_row = 1;
    for (const auto &type : parts.ray_types) {
        if (std::ranges::find(traced, type) == traced.end()) continue;
        auto *label = caption(qtext(type));
        label->setObjectName(rayFieldName(QStringLiteral("hitRayType.") + qtext(type)));
        grid->addWidget(label, grid_row, 0, Qt::AlignTop);
        for (std::size_t g = 0; g < groups.size(); ++g) {
            const auto &group = groups[g].name;
            const auto &group_type = groups[g].type;
            const bool procedural = group_type == "procedural";
            const auto cell_value = hits.is_object() && hits.contains(type) && hits.at(type).is_object() &&
                                            hits.at(type).contains(group) && hits.at(type).at(group).is_object()
                                        ? hits.at(type).at(group)
                                        : Json::object();
            auto *frame = new QFrame(grid_box);
            const auto cell_name = QStringLiteral("hit.") + qtext(type) + QLatin1Char{'.'} + qtext(group);
            frame->setObjectName(rayFieldName(cell_name));
            frame->setFrameShape(QFrame::StyledPanel);
            auto *lines = new QVBoxLayout(frame);
            lines->setContentsMargins(3, 3, 3, 3);
            lines->setSpacing(2);
            Parts::Cell cell{type, group, {}};
            struct StageRow {
                const char *field;
                const char *stage;
                RayLayoutTemplateStage template_stage;
                bool required;
                QString none;
            };
            std::vector<StageRow> stages{
                {"closesthit", "closesthit", RayLayoutTemplateStage::closest_hit, true, tr("(choose closest hit)")},
                {"anyhit", "anyhit", RayLayoutTemplateStage::any_hit, false, tr("(no any hit)")}};
            if (procedural)
                stages.push_back({"intersection", "intersection", RayLayoutTemplateStage::intersection, true,
                                  tr("(choose intersection)")});
            for (const auto &stage : stages) {
                const auto field_name = cell_name + QLatin1Char{'.'} + QString::fromLatin1(stage.field);
                auto *combo = shaderCombo(rayFieldName(field_name), stage.stage,
                                          shaderOf(cell_value.value(stage.field, Json{})), stage.required,
                                          stage.none);
                auto *button = newButton(rayFieldName(field_name + QStringLiteral(".new")));
                auto *line = new QWidget(frame);
                auto *line_layout = new QHBoxLayout(line);
                line_layout->setContentsMargins(0, 0, 0, 0);
                combo->setParent(line);
                button->setParent(line);
                line_layout->addWidget(combo, 1);
                line_layout->addWidget(button);
                lines->addWidget(line);
                cell.stages.emplace(stage.field, combo);
                const auto put = [later, type, group, field = std::string{stage.field}](const std::string &reference) {
                    later([type, group, field, reference](Json value) {
                        auto all = value.value("hit_shaders", Json::object());
                        if (!all.is_object()) all = Json::object();
                        auto &row_value = all[type];
                        if (!row_value.is_object()) row_value = Json::object();
                        auto &cell_json = row_value[group];
                        if (!cell_json.is_object()) cell_json = Json::object();
                        if (reference.empty()) cell_json.erase(field);
                        else cell_json[field] = reference;
                        if (cell_json.empty()) row_value.erase(group);
                        value["hit_shaders"] = all;
                        return value;
                    });
                };
                const bool required = stage.required;
                connect(combo, &QComboBox::currentIndexChanged, this, [combo, put, required] {
                    const auto picked = combo->currentData().toString().toStdString();
                    if (!picked.empty() || !required) put(picked);
                });
                const RayLayoutTemplatePlace place{stage.template_stage, type, group, {}};
                connect(button, &QPushButton::clicked, this, [this, place, put] { createShader(place, put); });
            }
            grid->addWidget(frame, grid_row, static_cast<int>(g) + 1);
            parts.cells.push_back(std::move(cell));
        }
        ++grid_row;
    }
    if (grid_row == 1) {
        auto *none = caption(tr("No ray type is traced: check one above."));
        none->setObjectName(rayFieldName(QStringLiteral("hitGridEmpty")));
        grid->addWidget(none, 1, 0, 1, static_cast<int>(groups.size()) + 1);
    }
    column->addWidget(grid_box);

    // Callable shaders, in index order (the index executeCallableEXT names).
    std::vector<std::string> callables;
    if (const auto found = rt.find("callable"); found != rt.end()) {
        if (found->is_string()) callables.push_back(found->get<std::string>());
        else if (found->is_array())
            for (const auto &each : *found)
                if (each.is_string()) callables.push_back(each.get<std::string>());
    }
    const auto writeCallables = [](Json value, const std::vector<std::string> &list) {
        if (list.empty()) value.erase("callable");
        else if (list.size() == 1) value["callable"] = list.front();
        else value["callable"] = list;
        return value;
    };
    auto *callable_box = new QWidget(parts.body);
    auto *callable_lines = new QVBoxLayout(callable_box);
    callable_lines->setContentsMargins(0, 0, 0, 0);
    callable_lines->setSpacing(2);
    for (std::size_t index = 0; index < callables.size(); ++index) {
        const auto name = QStringLiteral("callable.") + QString::number(index);
        auto *combo = shaderCombo(rayFieldName(name), "callable", callables[index], true, tr("(choose)"));
        auto *remove = new QToolButton(parts.body);
        remove->setObjectName(rayFieldName(name + QStringLiteral(".remove")));
        remove->setText(QStringLiteral("×"));
        remove->setToolTip(tr("Remove this callable (the ones after it move up an index)."));
        auto *line = new QWidget(callable_box);
        auto *line_layout = new QHBoxLayout(line);
        line_layout->setContentsMargins(0, 0, 0, 0);
        auto *index_label = caption(QString::number(index));
        index_label->setParent(line);
        combo->setParent(line);
        remove->setParent(line);
        line_layout->addWidget(index_label);
        line_layout->addWidget(combo, 1);
        line_layout->addWidget(remove);
        callable_lines->addWidget(line);
        connect(combo, &QComboBox::currentIndexChanged, this, [later, combo, index, callables, writeCallables] {
            const auto picked = combo->currentData().toString().toStdString();
            if (picked.empty()) return;
            auto list = callables;
            list[index] = picked;
            later([list, writeCallables](Json value) { return writeCallables(std::move(value), list); });
        });
        connect(remove, &QToolButton::clicked, this, [later, index, callables, writeCallables] {
            auto list = callables;
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
            later([list, writeCallables](Json value) { return writeCallables(std::move(value), list); });
        });
    }
    auto *callable_new = newButton(rayFieldName(QStringLiteral("callable.new")));
    callable_new->setToolTip(tr("Write a starting callable shader into the project's shaders folder, and add it "
                                "as the next callable."));
    auto *callable_add_line = new QWidget(callable_box);
    auto *callable_add_layout = new QHBoxLayout(callable_add_line);
    callable_add_layout->setContentsMargins(0, 0, 0, 0);
    // + adds a row of the first callable shader the project has.
    auto *callable_add = new QPushButton(QStringLiteral("+"), callable_add_line);
    callable_add->setObjectName(rayFieldName(QStringLiteral("callable.add")));
    callable_add->setAutoDefault(false);
    callable_add->setFixedWidth(28);
    callable_add->setToolTip(tr("Add a callable: the project's first callable shader, to pick another."));
    std::string first_callable;
    for (const auto &[reference, stage] : parts.shaders)
        if (stage == "callable") {
            first_callable = reference;
            break;
        }
    callable_add->setEnabled(!first_callable.empty());
    callable_new->setParent(callable_add_line);
    callable_add_layout->addWidget(callable_add);
    callable_add_layout->addWidget(callable_new);
    // RTL-2s-c: the root's callable record values (get/edit_ray_callable_records),
    // in the dialog Render Features' Records opens -- for a task whose
    // callable_record declares a schema.
    auto *callable_records = new QPushButton(tr("Callable records…"), callable_add_line);
    callable_records->setObjectName(rayFieldName(QStringLiteral("callableRecords")));
    callable_records->setAutoDefault(false);
    const bool has_schema = rt.contains("callable_record");
    callable_records->setEnabled(has_schema && static_cast<bool>(open_callable_records_));
    callable_records->setToolTip(has_schema ? tr("The values each callable's record starts with (the root's "
                                                 "ray_task_callable_records).")
                                            : tr("Give the task a callable_record (below) first: its records' "
                                                 "values are edited there."));
    callable_add_layout->addWidget(callable_records);
    connect(callable_records, &QPushButton::clicked, this, [this] {
        if (open_callable_records_) open_callable_records_();
    });
    callable_add_layout->addStretch(1);
    callable_lines->addWidget(callable_add_line);
    connect(callable_add, &QPushButton::clicked, this, [later, callables, first_callable, writeCallables] {
        auto list = callables;
        list.push_back(first_callable);
        later([list, writeCallables](Json value) { return writeCallables(std::move(value), list); });
    });
    connect(callable_new, &QPushButton::clicked, this, [this, later, callables, writeCallables] {
        createShader({RayLayoutTemplateStage::callable, {}, {}, {}},
                     [later, callables, writeCallables](const std::string &reference) {
                         auto list = callables;
                         list.push_back(reference);
                         later([list, writeCallables](Json value) { return writeCallables(std::move(value), list); });
                     });
    });
    auto *callable_form = new QFormLayout;
    callable_form->setContentsMargins(0, 0, 0, 0);
    callable_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    callable_form->addRow(caption(tr("Callable shaders")), callable_box);
    column->addLayout(callable_form);

    // RTL-2s-c: the raygen's switches -- the second traced ray type toward
    // the light, the colour through callable 0 -- written into the raygen
    // Studio wrote (raylayouttemplate's #define lines), so nothing is typed.
    {
        const auto raygen_file = rayShaderFile(project_root_, textOf(rt, "raygen"), "raygen");
        std::optional<std::string> raygen_text;
        if (raygen_file) {
            std::ifstream input{*raygen_file, std::ios::binary};
            if (input) raygen_text = std::string{std::istreambuf_iterator<char>{input}, {}};
        }
        const auto second = rayLayoutRaygenSwitch(raygen_text.value_or(""), RayTraceSecondSwitch);
        const auto callable = rayLayoutRaygenSwitch(raygen_text.value_or(""), RayCallCallableSwitch);
        const QString not_ours = tr("The raygen is not one Studio wrote (it has no #define %1 line): New... "
                                    "beside Raygen writes one.");
        auto *switches = new QWidget(parts.body);
        auto *switches_line = new QHBoxLayout(switches);
        switches_line->setContentsMargins(0, 0, 0, 0);
        const auto second_type = traced.size() > 1 ? traced[1] : std::string{};
        parts.trace_second = new QCheckBox(
            second_type.empty() ? tr("Trace a second ray type") : tr("Trace %1 toward the light").arg(qtext(second_type)),
            switches);
        parts.trace_second->setObjectName(rayFieldName(QStringLiteral("traceSecond")));
        parts.trace_second->setChecked(second.value_or(false));
        parts.trace_second->setEnabled(second.has_value() && !second_type.empty());
        parts.trace_second->setToolTip(!second ? not_ours.arg(QString::fromLatin1("TRACE_SECOND_RAY_TYPE"))
                                       : second_type.empty()
                                           ? tr("Trace a second ray type first: check one above.")
                                           : tr("The raygen also traces a ray of type %1 from the point it hit "
                                                "toward the light, and darkens the point when something is in the "
                                                "way.")
                                                 .arg(qtext(second_type)));
        parts.call_callable = new QCheckBox(tr("Call callable 0"), switches);
        parts.call_callable->setObjectName(rayFieldName(QStringLiteral("callCallable")));
        parts.call_callable->setChecked(callable.value_or(false));
        parts.call_callable->setEnabled(callable.has_value() && !callables.empty());
        parts.call_callable->setToolTip(!callable ? not_ours.arg(QString::fromLatin1("CALL_CALLABLE_0"))
                                        : callables.empty()
                                            ? tr("Add a callable first (Callable shaders).")
                                            : tr("The raygen passes the colour through the task's callable 0."));
        switches_line->addWidget(parts.trace_second);
        switches_line->addWidget(parts.call_callable);
        switches_line->addStretch(1);
        auto *switch_form = new QFormLayout;
        switch_form->setContentsMargins(0, 0, 0, 0);
        switch_form->addRow(caption(tr("Raygen")), switches);
        column->addLayout(switch_form);
        connect(parts.trace_second, &QCheckBox::toggled, this,
                [this](bool on) { setRaygenSwitch(RayTraceSecondSwitch, on); });
        connect(parts.call_callable, &QCheckBox::toggled, this,
                [this](bool on) { setRaygenSwitch(RayCallCallableSwitch, on); });
    }

    // The rest: the catalog's own editors.
    auto *rest = new QFormLayout;
    rest->setContentsMargins(0, 0, 0, 0);
    rest->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    column->addLayout(rest);
    if (const auto schema = model_.parameterSchema("render.compute", "ray_tracing")) {
        auto choices = std::make_shared<GraphParameterChoices>();
        choices->model = &model_;
        choices->scope = scope_;
        choices->node = node_;
        choices->op = "render.compute";
        choices->project_root = project_root_;
        for (const auto &field : schema->value("fields", Json::array())) {
            const auto name = textOf(field, "name");
            if (name != "callable_record" && name != "callable_records" && name != "max_recursion" &&
                name != "acceleration_structure")
                continue;
            auto *editor = new GraphParameterEditor(field, choices, rayFieldName(qtext(name)), {}, parts.body);
            editor->setValue(rt.value(name, Json{}));
            editor->refreshChoices(model_.nodeParameters(scope_, node_));
            editor->setEdited([later, editor, name](bool) {
                const auto value = editor->value();
                later([name, value](Json current_value) {
                    if (value.is_null()) current_value.erase(name);
                    else current_value[name] = value;
                    return current_value;
                });
            });
            auto *label = caption(qtext(name));
            label->setToolTip(editor->toolTip());
            rest->addRow(label, editor);
            parts.generic.emplace_back(name, editor);
        }
    }
}

void GraphCanvasRayTracingFields::setRaygenSwitch(std::string_view name, bool on) {
    auto &parts = *parts_;
    const auto rt = current();
    const auto raygen = textOf(rt, "raygen");
    const auto file = rayShaderFile(project_root_, raygen, "raygen");
    const auto say = [&](const QString &text) {
        parts.notice->setText(text);
        parts.notice->show();
    };
    if (!file) {
        say(tr("The raygen is not a file of the project."));
        return;
    }
    std::string source;
    {
        std::ifstream input{*file, std::ios::binary};
        if (!input) {
            say(tr("Could not read %1.").arg(qtext(file->filename().string())));
            return;
        }
        source = std::string{std::istreambuf_iterator<char>{input}, {}};
    }
    std::vector<std::string> traced;
    for (const auto &type : rt.value("traced_ray_types", Json::array()))
        if (type.is_string()) traced.push_back(type.get<std::string>());
    std::optional<std::string> written;
    QString line;
    if (name == RayTraceSecondSwitch && on && traced.size() > 1 &&
        countMacroUses(source, "PELICAN_RAY_TYPE_" + traced[1]) == 0) {
        // The raygen was written before the ray type was traced: written
        // again from the template, for the ray types the task traces now.
        const auto names = rayLayoutTemplateNamesFor(model_);
        if (!names) return;
        RayLayoutTemplatePlace place{RayLayoutTemplateStage::raygen, {}, {}, {}};
        place.second_ray_type = traced[1];
        const auto misses = parts.misses_names;
        place.second_miss = misses.size() > 1 ? misses[1] : std::string{};
        place.trace_second = true;
        place.call_callable = rayLayoutRaygenSwitch(source, RayCallCallableSwitch).value_or(false);
        written = rayLayoutShaderTemplate(*names, place, file->stem().string());
        line = tr("Wrote %1 again from the template: it traces %2 toward the light.")
                   .arg(qtext(file->filename().string()), qtext(traced[1]));
    } else {
        written = setRayLayoutRaygenSwitch(source, name, on);
        line = tr("%1: %2 is %3.").arg(qtext(file->filename().string()), qtext(std::string{name}),
                                     on ? QStringLiteral("1") : QStringLiteral("0"));
    }
    if (!written) {
        say(tr("%1 has no #define %2 line: it is not a raygen Studio wrote.")
                .arg(qtext(file->filename().string()), qtext(std::string{name})));
        return;
    }
    if (source.find("\r\n") != std::string::npos && written->find("\r\n") == std::string::npos) {
        std::string crlf;
        for (const char c : *written) {
            if (c == '\n') crlf += '\r';
            crlf += c;
        }
        written = std::move(crlf);
    }
    std::ofstream output{*file, std::ios::binary | std::ios::trunc};
    output.write(written->data(), static_cast<std::streamsize>(written->size()));
    output.close();
    if (!output) {
        say(tr("Could not write %1.").arg(qtext(file->filename().string())));
        return;
    }
    say(line);
    // RTL-3s, decision 4: on the render history, as a rename's shaders are.
    if (file_history_)
        file_history_(std::string{name} + (on ? " on" : " off") + " in " + file->filename().string(),
                      file->string(), source, *written);
    // The node's files are watched: the engine compiles the raygen again.
    if (shaders_changed_) shaders_changed_();
}

} // namespace PelicanStudio
