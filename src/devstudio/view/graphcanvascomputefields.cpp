#include "graphcanvascomputefields.hpp"

#include "fullscreenshadertemplate.hpp"
#include "studiotheme.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

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
    status_->hide();
    copy_->hide();
    hide();
}

void GraphCanvasComputeFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    if (node == nullptr || node->op != "render.compute") {
        node_.clear();
        scope_.clear();
        shown_.clear();
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
    followEditable();
    show();
}

void GraphCanvasComputeFields::followEditable() {
    shader_->setEnabled(model_.canEdit());
    new_shader_->setEnabled(model_.canEdit() && !project_root_.empty());
    create_shader_->setEnabled(model_.canEdit() && !project_root_.empty());
    schedule_->followEditable();
}

void GraphCanvasComputeFields::setProjectRoot(const std::filesystem::path &root) {
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

} // namespace PelicanStudio
