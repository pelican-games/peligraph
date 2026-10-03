#include "graphcanvasfoldedpassfields.hpp"

#include "fullscreenpasssettings.hpp"
#include "graphcanvasrenderfields.hpp"
#include "passforms/attachmentopsform.hpp"
#include "passforms/materialsettingsform.hpp"
#include "passforms/shaderdefinesform.hpp"
#include "presetpassedit.hpp"
#include "studiotheme.hpp"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <initializer_list>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString qtext(const std::string &value) { return QString::fromStdString(value); }

const QString &foldedPrefix() {
    static const QString prefix = QStringLiteral("pelican.graphCanvasFoldedPass");
    return prefix;
}

QString childName(const QString &suffix) { return foldedPrefix() + QLatin1Char('.') + suffix; }

// GL-5b: the data of an output's last entry, New target….
constexpr char kNewTargetMarker[] = "\x1f" "new_target";

QLabel *caption(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setFont(studioSmallFont());
    return label;
}

// The Pass Editor's parts are as wide as their longest choice; beside the
// canvas they take what the column gives them (as the Pass fields' do).
void compact(QWidget &parent) {
    for (auto *combo : parent.findChildren<QComboBox *>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(6);
    }
    for (auto *line : parent.findChildren<QLineEdit *>()) line->setMinimumWidth(40);
}

std::string stageOf(const Json &declaration, const char *stage) {
    const auto shader = declaration.value("shader", Json::object());
    return shader.is_object() ? shader.value(stage, std::string{}) : std::string{};
}

} // namespace

GraphCanvasFoldedPassFields::GraphCanvasFoldedPassFields(GraphCanvasModel &model,
                                                         GraphCanvasReadCapability graph, Send send,
                                                         QWidget *parent)
    : QWidget{parent}, model_{model}, graph_{std::move(graph)}, send_{std::move(send)} {
    setObjectName(foldedPrefix());
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 4);
    heading_ = new QLabel(this);
    heading_->setObjectName(childName(QStringLiteral("heading")));
    heading_->setWordWrap(true);
    QFont bold = heading_->font();
    bold.setBold(true);
    heading_->setFont(bold);
    layout->addWidget(heading_);

    auto *fields = new QWidget(this);
    auto *form = new QFormLayout(fields);
    form->setContentsMargins(0, 0, 0, 0);
    // Decision 2: the shaders, picked from the Pass Editor's list.
    shaders_ = new QWidget(fields);
    auto *shader_form = new QFormLayout(shaders_);
    shader_form->setContentsMargins(0, 0, 0, 0);
    vertex_ = new QComboBox(shaders_);
    vertex_->setObjectName(childName(QStringLiteral("vertex")));
    fragment_ = new QComboBox(shaders_);
    fragment_->setObjectName(childName(QStringLiteral("fragment")));
    shader_form->addRow(caption(tr("Vertex"), shaders_), vertex_);
    shader_form->addRow(caption(tr("Fragment"), shaders_), fragment_);
    form->addRow(shaders_);
    inputs_ = new QLabel(fields);
    inputs_->setObjectName(childName(QStringLiteral("inputs")));
    inputs_->setWordWrap(true);
    inputs_->setToolTip(tr("The pass's input, in binding order. Drop a line on the node's "
                           "sockets to change it."));
    form->addRow(caption(tr("Inputs"), fields), inputs_);
    // GL-5b, decision 3: what each colour output writes, picked again from
    // the declared targets (fillOutputs).
    outputs_ = new QWidget(fields);
    outputs_->setObjectName(childName(QStringLiteral("outputs")));
    auto *outputs_layout = new QVBoxLayout(outputs_);
    outputs_layout->setContentsMargins(0, 0, 0, 0);
    form->addRow(caption(tr("Writes"), fields), outputs_);
    defines_ = new ShaderDefinesForm(foldedPrefix(), fields);
    form->addRow(caption(tr("Defines"), fields), defines_);
    interface_ = new FullscreenPassInterfaceRow(fields, foldedPrefix());
    form->addRow(caption(tr("Frame data"), fields), interface_);
    settings_ = new MaterialSettingsForm(foldedPrefix(), fields);
    form->addRow(settings_);
    ops_ = new AttachmentOpsForm(foldedPrefix(), fields);
    ops_caption_ = caption(tr("Attachments"), fields);
    form->addRow(ops_caption_, ops_);
    layout->addWidget(fields);

    // Decision 5: the pass's graph-wide decisions, as the Frame Plan's
    // right-click writes them.
    planning_ = new QWidget(this);
    planning_->setObjectName(childName(QStringLiteral("planning")));
    auto *planning_layout = new QVBoxLayout(planning_);
    planning_layout->setContentsMargins(0, 2, 0, 0);
    planning_layout->addWidget(caption(tr("Decisions"), planning_));
    serial_ = new QCheckBox(tr("Serial"), planning_);
    serial_->setObjectName(childName(QStringLiteral("serial")));
    serial_->setToolTip(tr("Run it alone: the planner neither runs it beside another node nor "
                           "fuses it (target_planning, edit_target_planning)."));
    isolate_ = new QCheckBox(tr("Isolate"), planning_);
    isolate_->setObjectName(childName(QStringLiteral("isolate")));
    isolate_->setToolTip(tr("Serial, and none of the images it reads or writes shares memory "
                            "with another."));
    // As wide as their words: a box is pressed on its words, not beside them.
    planning_layout->addWidget(serial_, 0, Qt::AlignLeft);
    planning_layout->addWidget(isolate_, 0, Qt::AlignLeft);
    no_alias_box_ = new QWidget(planning_);
    auto *alias_layout = new QVBoxLayout(no_alias_box_);
    alias_layout->setContentsMargins(0, 0, 0, 0);
    planning_layout->addWidget(no_alias_box_);
    layout->addWidget(planning_);
    connect(serial_, &QCheckBox::clicked, this,
            [this](bool on) { togglePlanning("node", pass_, "serial", on); });
    connect(isolate_, &QCheckBox::clicked, this,
            [this](bool on) { togglePlanning("node", pass_, "isolate", on); });

    // A pick is one edit at once (the lists are filled under a signal
    // blocker); a value typed (a define, a clear colour,
    // a material setting) is one edit when the typing stops.
    typing_ = new QTimer(this);
    typing_->setSingleShot(true);
    typing_->setInterval(500);
    connect(typing_, &QTimer::timeout, this, [this] { commit(); });
    for (auto *combo : {vertex_, fragment_})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            typing_->stop();
            commit();
        });
    for (PassForm *passform : std::initializer_list<PassForm *>{defines_, ops_, settings_})
        connect(passform, &PassForm::changed, this, [this] { formChanged(); });
    interface_->setChangedAction([this] { formChanged(); });
    compact(*fields);
    hide();
}

GraphCanvasFoldedPassFields::~GraphCanvasFoldedPassFields() = default;

FramePlanAuthoring GraphCanvasFoldedPassFields::authoring() const {
    const auto revision = model_.authoringRevision();
    if (!authoring_ || authoring_->first != revision)
        authoring_ = std::pair{revision, framePlanAuthoringFromContext(model_.renderAuthoringContext())};
    return authoring_->second;
}

const FramePlanAuthoringPass *GraphCanvasFoldedPassFields::row(
    const FramePlanAuthoring &authoring) const {
    const auto *found = authoring.find(model_.planGraph(), pass_);
    // Decision 1: only what the root's rendering section declares, of the
    // two kinds whose parts the Pass Editor has here.
    if (found == nullptr || !found->root) return nullptr;
    const auto type = found->editable_declaration.value("type", std::string{});
    return type == "fullscreen" || type == "material" ? found : nullptr;
}

void GraphCanvasFoldedPassFields::showPass(const std::string &pass) {
    // A value typed and waiting for the pause goes to the pass it was typed
    // for before the column lets go of it.
    if (pass != pass_ && typing_->isActive()) {
        typing_->stop();
        commit();
    }
    pass_ = pass;
    const auto context = authoring();
    const auto *found = pass_.empty() || !model_.currentScope().empty() || model_.foldedView()
                            ? nullptr
                            : row(context);
    editable_ = found != nullptr;
    if (!editable_) {
        shown_.clear();
        choices_shown_.clear();
        hide();
        return;
    }
    const auto &declaration = found->editable_declaration;
    const auto key = pass_ + "\x1f" + declaration.dump();
    if (key != shown_ && !typing_->isActive()) {
        shown_ = key;
        load(*found);
    }
    fillShaderChoices(declaration);
    fillOutputs();
    showPlanning();
    readPlanning();
    show();
}

void GraphCanvasFoldedPassFields::load(const FramePlanAuthoringPass &row) {
    loading_ = true;
    original_ = row.editable_declaration;
    type_ = original_.value("type", std::string{});
    const bool fullscreen = type_ == "fullscreen";
    heading_->setText(tr("%1 (%2): from the rendering section").arg(qtext(pass_), qtext(type_)));
    heading_->setToolTip(tr("%1 is declared by the root's rendering section; a change here is "
                            "the Pass Editor's edit_preset_pass of it.")
                             .arg(qtext(pass_)));
    shaders_->setVisible(fullscreen);
    defines_->setVisible(fullscreen);
    interface_->setVisible(fullscreen);
    settings_->setVisible(!fullscreen);
    inputs_->setVisible(fullscreen);
    if (auto *form = qobject_cast<QFormLayout *>(defines_->parentWidget()->layout())) {
        form->setRowVisible(inputs_, fullscreen);
        form->setRowVisible(defines_, fullscreen);
        form->setRowVisible(interface_, fullscreen);
        form->setRowVisible(settings_, !fullscreen);
    }
    QStringList inputs;
    for (const auto &input : original_.value("input", Json::array()))
        if (input.is_string()) inputs.push_back(qtext(input.get<std::string>()));
    inputs_->setText(inputs.isEmpty() ? tr("none -- drop an image on the node's + input")
                                      : inputs.join(QStringLiteral(", ")));
    if (fullscreen) {
        defines_->setPassName(pass_);
        defines_->load(original_.value("shader", Json::object()).value("defines", Json(nullptr)));
        interface_->load(original_);
    } else {
        settings_->setResourceCandidates(model_.planTargets());
        settings_->setVariantCandidates(model_.materialVariants());
        settings_->setTagCandidates(model_.materialTags());
        settings_->load(original_);
    }
    const auto output = original_.value("output", Json::object());
    ops_->load(output.value("color", Json::array()), original_.value("clear_colors", Json(nullptr)),
               output.value("depth", Json(nullptr)));
    ops_->setDepthOpsShown(!fullscreen);
    ops_->show();
    choices_shown_.clear();
    fillShaderChoices(original_);
    outputs_shown_.clear();
    fillOutputs();
    compact(*this);
    loading_ = false;
    // What the forms give back untouched: a commit that still gives this
    // sends nothing.
    try {
        loaded_draft_ = draftJson(original_);
    } catch (const std::exception &) {
        loaded_draft_ = nullptr;
    }
}

void GraphCanvasFoldedPassFields::fillShaderChoices(const Json &declaration) {
    if (type_ != "fullscreen") return;
    // EL-4b: the lists are on screen; the engine's is asked for (once) and
    // replaces the fixed one when it arrives.
    model_.askEngineShaders();
    QStringList parts{qtext(stageOf(declaration, "vertex")), qtext(stageOf(declaration, "fragment"))};
    for (const auto &reference : model_.vertexShaderChoices()) parts << qtext(reference);
    parts << QStringLiteral("|");
    for (const auto &reference : model_.fragmentShaderChoices()) parts << qtext(reference);
    const auto key = parts.join(QChar{0x1f}).toStdString();
    if (key == choices_shown_) return;
    choices_shown_ = key;
    for (const auto &[combo, stage, choices] :
         {std::tuple{vertex_, "vertex", &model_.vertexShaderChoices()},
          std::tuple{fragment_, "fragment", &model_.fragmentShaderChoices()}}) {
        const QSignalBlocker block{combo};
        combo->clear();
        for (const auto &reference : *choices) addShaderChoice(*combo, reference);
        // What the pass draws with now, even when no list names it.
        const auto current = stageOf(declaration, stage);
        if (!current.empty() && combo->findData(qtext(current)) < 0) addShaderChoice(*combo, current);
        combo->setCurrentIndex(combo->findData(qtext(current)));
    }
}

void GraphCanvasFoldedPassFields::followEditable(bool can_send) {
    can_send_ = can_send;
    for (QWidget *widget : std::initializer_list<QWidget *>{vertex_, fragment_, outputs_, defines_,
                                                          interface_, ops_, settings_, planning_})
        widget->setEnabled(can_send);
}

void GraphCanvasFoldedPassFields::fillOutputs() {
    const auto &current = ops_->colorTargets();
    // The declared colour targets: a depth target is no colour output.
    std::vector<std::string> declared;
    for (const auto &target : model_.declaredRenderTargets())
        if (!target.depth) declared.push_back(target.name);
    // Decision 5: New target… is named as the Pass Editor's is.
    const auto proposed = proposedRenderTargetName(authoring(), pass_);
    std::string key = pass_ + "\x1f" + proposed;
    for (const auto &name : current) key += "\x1f" + name;
    key += "\x1e";
    for (const auto &name : declared) key += "\x1f" + name;
    if (key == outputs_shown_) return;
    outputs_shown_ = key;
    // A combo may be the one whose pick led here: it goes once its signal is done.
    for (auto *combo : std::exchange(output_combos_, {})) {
        combo->setObjectName({});
        combo->hide();
        combo->deleteLater();
    }
    for (std::size_t output = 0; output < current.size(); ++output) {
        auto *combo = new QComboBox(outputs_);
        combo->setObjectName(childName(QStringLiteral("output.%1").arg(output)));
        combo->setToolTip(tr("The render target colour output %1 writes: a declared one, or a "
                             "new one (one edit_preset_pass).")
                              .arg(output));
        auto names = declared;
        if (std::ranges::find(names, current[output]) == names.end())
            names.insert(names.begin(), current[output]);
        for (const auto &name : names) combo->addItem(qtext(name), qtext(name));
        combo->addItem(tr("New target: %1…").arg(qtext(proposed)),
                       QString::fromLatin1(kNewTargetMarker));
        combo->setItemData(combo->count() - 1, qtext(proposed), Qt::UserRole + 1);
        combo->setItemData(combo->count() - 1,
                           tr("Opens New render target with this name; once it is declared, "
                              "this output writes it."),
                           Qt::ToolTipRole);
        combo->setCurrentIndex(combo->findData(qtext(current[output])));
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(6);
        outputs_->layout()->addWidget(combo);
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, output](int index) { outputPicked(output, index); });
        output_combos_.push_back(combo);
    }
}

void GraphCanvasFoldedPassFields::outputPicked(std::size_t output, int index) {
    if (loading_ || !editable_ || output >= output_combos_.size() ||
        output >= ops_->colorTargets().size())
        return;
    auto *combo = output_combos_[output];
    if (combo->itemData(index).toString() == QLatin1String{kNewTargetMarker}) {
        const auto name = combo->itemData(index, Qt::UserRole + 1).toString().toStdString();
        {
            const QSignalBlocker block{combo};
            combo->setCurrentIndex(combo->findData(qtext(ops_->colorTargets()[output])));
        }
        if (new_target_) new_target_(pass_, output, name);
        return;
    }
    retarget(output, combo->itemData(index).toString().toStdString());
}

void GraphCanvasFoldedPassFields::retarget(std::size_t output, const std::string &target) {
    if (output >= ops_->colorTargets().size() || ops_->colorTargets()[output] == target) return;
    // The output's Load / Store and clear colour go with it; the edit is the
    // Pass Editor's Save of the pass with that output changed.
    ops_->retargetColor(output, target);
    typing_->stop();
    commit();
}

void GraphCanvasFoldedPassFields::outputTargetCreated(const std::string &pass, std::size_t output,
                                                      const std::string &target) {
    if (pass != pass_ || !editable_) return;
    retarget(output, target);
}

void GraphCanvasFoldedPassFields::editRefused() {
    // What the root holds is shown again: the next showPass loads it.
    shown_.clear();
    outputs_shown_.clear();
}

Json GraphCanvasFoldedPassFields::draftJson(const Json &original) const {
    const auto type = original.value("type", std::string{});
    if (type == "fullscreen") {
        // The Pass Editor's draft of a fullscreen pass: built from the forms.
        Json draft{{"name", pass_}, {"type", "fullscreen"}};
        draft["input"] = original.value("input", Json::array());
        draft["output"] = Json{{"color", ops_->color()}, {"depth", ops_->depth()}};
        if (const auto refusal = defines_->refusal(); !refusal.isEmpty())
            throw std::runtime_error(refusal.toStdString());
        Json shader{{"vertex", vertex_->currentData().toString().toStdString()},
                    {"fragment", fragment_->currentData().toString().toStdString()}};
        if (const auto defines = defines_->defines(); !defines.is_null()) shader["defines"] = defines;
        draft["shader"] = std::move(shader);
        interface_->apply(draft);
        if (auto clear = ops_->clearColors(); !clear.is_null()) draft["clear_colors"] = std::move(clear);
        return draft;
    }
    // A material pass keeps its declaration; the forms' keys are written over it.
    Json draft = original;
    auto output = draft.value("output", Json::object());
    output["color"] = ops_->color();
    if (const auto depth = output.find("depth"); depth != output.end() && !depth->is_null())
        output["depth"] = ops_->depth();
    draft["output"] = std::move(output);
    const auto settings = settings_->settings();
    for (const auto &key : MaterialSettingsForm::keys()) {
        if (settings.contains(key) && !settings.at(key).is_null())
            draft[key] = settings.at(key);
        else
            draft.erase(key);
    }
    return draft;
}

FramePlanEditRequest GraphCanvasFoldedPassFields::passEditRequest() const {
    FramePlanEditRequest request;
    const auto context = authoring();
    const auto *found = row(context);
    const auto subject = found != nullptr ? presetPassEditSubject(context, model_.planGraph(), pass_)
                                          : std::nullopt;
    if (!subject) {
        request.refusal = pass_ + " is not declared by the rendering section any more.";
        return request;
    }
    PresetPassEditDraft edit{.original = found->editable_declaration,
                             .ray_query = presetPassRayQuery(found->editable_declaration)};
    try {
        edit.draft = draftJson(found->editable_declaration);
    } catch (const std::exception &error) {
        request.refusal = pass_ + ": " + error.what();
        return request;
    }
    request.method = "edit_preset_pass";
    request.params = presetPassEditParams(*subject, edit);
    request.notice = pass_ + " saved.";
    return request;
}

void GraphCanvasFoldedPassFields::formChanged() {
    if (loading_ || !editable_) return;
    typing_->start();
}

void GraphCanvasFoldedPassFields::commit() {
    if (loading_ || !editable_ || !send_) return;
    // Nothing moved since the forms were loaded: nothing is sent.
    try {
        if (draftJson(original_) == loaded_draft_) return;
    } catch (const std::exception &) {
        // A value that is not one yet: passEditRequest says why.
    }
    send_(passEditRequest(), [this] { return passEditRequest(); });
}

void GraphCanvasFoldedPassFields::showPlanning() {
    const bool known = graph_.requestEditTargetPlanning != nullptr;
    planning_->setVisible(known);
    if (!known) return;
    {
        const QSignalBlocker serial{serial_};
        const QSignalBlocker isolate{isolate_};
        serial_->setChecked(planning_state_.flag("node", pass_, "serial"));
        isolate_->setChecked(planning_state_.flag("node", pass_, "isolate"));
    }
    // No alias: each render target the pass writes.
    std::vector<std::string> written;
    const auto &passes = model_.planPasses();
    if (const auto pass = std::ranges::find(passes, pass_, &GraphCanvasPlanPass::name);
        pass != passes.end())
        for (const auto &resource : pass->writes)
            if (std::ranges::binary_search(model_.planTargets(), resource))
                written.push_back(resource);
    std::vector<std::string> shown;
    for (const auto &[resource, box] : no_alias_) shown.push_back(resource);
    std::vector<std::string> wanted = written;
    std::ranges::sort(wanted);
    if (shown != wanted) {
        for (auto &[resource, box] : no_alias_) delete box;
        no_alias_.clear();
        for (const auto &resource : written) {
            auto *box = new QCheckBox(tr("No alias: %1").arg(qtext(resource)), no_alias_box_);
            box->setObjectName(childName(QStringLiteral("noAlias.") + qtext(resource)));
            box->setToolTip(tr("%1 shares its memory with no other image.").arg(qtext(resource)));
            static_cast<QVBoxLayout *>(no_alias_box_->layout())->addWidget(box, 0, Qt::AlignLeft);
            connect(box, &QCheckBox::clicked, this, [this, resource](bool on) {
                togglePlanning("resource", resource, "no_alias", on);
            });
            no_alias_.emplace(resource, box);
        }
    }
    for (auto &[resource, box] : no_alias_) {
        const QSignalBlocker block{box};
        box->setChecked(planning_state_.flag("resource", resource, "no_alias"));
    }
}

void GraphCanvasFoldedPassFields::readPlanning() {
    if (!graph_.requestRootDocument || !graph_.requestEditTargetPlanning || planning_request_ != 0 ||
        model_.sourceReference().empty())
        return;
    const auto key = model_.digest() + "\x1f" + model_.planGraph();
    if (key == planning_read_) return;
    QString error;
    planning_request_ = graph_.requestRootDocument(qtext(model_.sourceReference()), &error);
    if (planning_request_ != 0) planning_read_ = key;
}

bool GraphCanvasFoldedPassFields::deliverResult(qint64 request_id, const QByteArray &bytes) {
    if (request_id == 0 || request_id != planning_request_) return false;
    planning_request_ = 0;
    const auto response = Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    if (response.is_object())
        for (const auto &document : response.value("documents", Json::array())) {
            if (!document.is_object() ||
                document.value("reference", std::string{}) != model_.sourceReference())
                continue;
            const auto content = Json::parse(document.value("content", std::string{}), nullptr, false);
            planning_state_ = GraphTargetPlanning::fromRoot(content, model_.planGraph());
        }
    if (editable_) showPlanning();
    // The document moved while it was read: read it again.
    readPlanning();
    return true;
}

void GraphCanvasFoldedPassFields::editFinished() {
    // A refused box goes back to what the root holds; after a commit the
    // root is read again, whatever its digest says yet.
    planning_read_.clear();
    if (!editable_) return;
    showPlanning();
    readPlanning();
}

void GraphCanvasFoldedPassFields::togglePlanning(const std::string &kind, const std::string &name,
                                                 const std::string &field, bool on) {
    if (!send_ || name.empty()) return;
    // The Frame Plan's right-click's edit, built against the context read
    // last (again after a refusal for a moved document).
    const auto request = [this, kind, name, field, on] {
        FramePlanEditRequest planning;
        planning.method = "edit_target_planning";
        planning.params = graphTargetPlanningParams(authoring().source_digest, model_.planGraph(),
                                                    kind, name, field, on);
        const std::string title = field == "no_alias" ? "No alias"
                                  : field == "serial"  ? "Serial"
                                                       : "Isolate";
        planning.notice = title + (on ? " set on " : " taken off ") + name + ".";
        return planning;
    };
    send_(request(), request);
}

} // namespace PelicanStudio
