#include "graphcanvasposefields.hpp"

#include "studiotheme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString qtext(const std::string &value) { return QString::fromStdString(value); }

std::string lowerExtension(const std::filesystem::path &path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
    return extension;
}

std::string stringParameter(const Json &parameters, const char *key) {
    const auto found = parameters.find(key);
    return found != parameters.end() && found->is_string() ? found->get<std::string>()
                                                           : std::string{};
}

} // namespace

std::vector<std::string> projectRigReferences(const std::filesystem::path &project_root) {
    std::vector<std::string> references;
    if (project_root.empty()) return references;
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk{
        project_root, std::filesystem::directory_options::skip_permission_denied, error};
    for (auto end = std::filesystem::recursive_directory_iterator{}; !error && walk != end;
         walk.increment(error)) {
        std::error_code kind;
        if (walk->is_directory(kind)) {
            const auto name = walk->path().filename().string();
            if (!name.empty() && name.front() == '.') walk.disable_recursion_pending();
            continue;
        }
        const auto extension = lowerExtension(walk->path());
        if (extension != ".gltf" && extension != ".glb" && extension != ".vrm") continue;
        const auto relative = walk->path().lexically_relative(project_root).generic_u8string();
        if (relative.empty() || relative.starts_with(u8"..")) continue;
        references.push_back("project://" + std::string{relative.begin(), relative.end()});
    }
    std::ranges::sort(references);
    return references;
}

std::vector<std::string> gltfAnimationNames(const std::filesystem::path &file) {
    std::vector<std::string> names;
    std::ifstream input{file, std::ios::binary};
    if (!input) return names;
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    std::string text = bytes;
    // A binary glTF: the 12-byte header, then the JSON chunk first.
    if (bytes.size() >= 20 && bytes.compare(0, 4, "glTF") == 0) {
        std::uint32_t length = 0;
        std::uint32_t type = 0;
        std::memcpy(&length, bytes.data() + 12, 4);
        std::memcpy(&type, bytes.data() + 16, 4);
        if (type != 0x4E4F534Au || 20ull + length > bytes.size()) return names;
        text = bytes.substr(20, length);
    }
    const auto document = Json::parse(text, nullptr, false);
    if (!document.is_object()) return names;
    const auto animations = document.find("animations");
    if (animations == document.end() || !animations->is_array()) return names;
    for (const auto &animation : *animations) {
        const auto name = animation.is_object() ? stringParameter(animation, "name") : std::string{};
        if (!name.empty() && std::ranges::find(names, name) == names.end()) names.push_back(name);
    }
    return names;
}

std::filesystem::path projectReferenceFile(const std::filesystem::path &project_root,
                                           const std::string &reference) {
    constexpr std::string_view prefix = "project://";
    if (project_root.empty() || !reference.starts_with(prefix)) return {};
    const auto relative = reference.substr(prefix.size());
    return project_root / std::filesystem::path{std::u8string{relative.begin(), relative.end()}};
}

GraphCanvasPoseFields::GraphCanvasPoseFields(GraphCanvasModel &model,
                                             std::function<void()> submitted, QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)} {
    setObjectName(QStringLiteral("pelican.graphCanvasPose"));
    auto *form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    const auto caption = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setFont(studioSmallFont());
        return label;
    };
    rig_ = new QComboBox(this);
    rig_->setObjectName(QStringLiteral("pelican.graphCanvasPose.rig"));
    rig_->setToolTip(tr("The model whose skeleton the clip is sampled on: a glTF or VRM file of "
                        "the project."));
    form->addRow(caption(tr("Rig")), rig_);
    clip_ = new QComboBox(this);
    clip_->setObjectName(QStringLiteral("pelican.graphCanvasPose.clip"));
    clip_->setToolTip(tr("The animation of the rig's file that is sampled."));
    form->addRow(caption(tr("Clip")), clip_);
    speed_ = new QDoubleSpinBox(this);
    speed_->setObjectName(QStringLiteral("pelican.graphCanvasPose.speed"));
    speed_->setRange(0.0, 1000.0);
    speed_->setDecimals(3);
    speed_->setSingleStep(0.1);
    speed_->setKeyboardTracking(false);
    speed_->setToolTip(tr("Playback speed: 1 is the clip's own pace."));
    form->addRow(caption(tr("Speed")), speed_);
    loop_ = new QCheckBox(tr("Loop"), this);
    loop_->setObjectName(QStringLiteral("pelican.graphCanvasPose.loop"));
    loop_->setToolTip(tr("Whether the clip starts again at its end."));
    form->addRow(caption(QString{}), loop_);
    note_ = new QLabel(this);
    note_->setObjectName(QStringLiteral("pelican.graphCanvasPose.note"));
    note_->setFont(studioSmallFont());
    note_->setWordWrap(true);
    form->addRow(note_);
    QObject::connect(rig_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        const auto rig = rig_->currentData().toString().toStdString();
        if (!rig.empty()) commit("rig", rig);
    });
    QObject::connect(clip_, &QComboBox::currentIndexChanged, this, [this] {
        if (loading_) return;
        const auto clip = clip_->currentData().toString().toStdString();
        if (!clip.empty()) commit("clip", clip);
    });
    QObject::connect(speed_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                     [this](double value) {
                         if (!loading_) commit("speed", value);
                     });
    QObject::connect(loop_, &QCheckBox::toggled, this, [this](bool value) {
        if (!loading_) commit("loop", value);
    });
    hide();
}

void GraphCanvasPoseFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    if (node == nullptr || node->op != "pose.clip_sample") {
        node_.clear();
        hide();
        return;
    }
    const bool same = node_ == node->id && scope_ == scope && parameters_ == node->parameters;
    node_ = node->id;
    scope_ = scope;
    parameters_ = node->parameters.is_object() ? node->parameters : Json::object();
    if (!same) load();
    followEditable();
    show();
}

void GraphCanvasPoseFields::followEditable() {
    const bool editable = !node_.empty() && model_.canEdit();
    rig_->setEnabled(editable);
    clip_->setEnabled(editable);
    speed_->setEnabled(editable);
    loop_->setEnabled(editable);
}

void GraphCanvasPoseFields::load() {
    loading_ = true;
    const QSignalBlocker block_rig{rig_};
    const QSignalBlocker block_clip{clip_};
    const QSignalBlocker block_speed{speed_};
    const QSignalBlocker block_loop{loop_};
    const auto rig = stringParameter(parameters_, "rig");
    auto rigs = projectRigReferences(project_root_);
    if (!rig.empty() && std::ranges::find(rigs, rig) == rigs.end()) rigs.insert(rigs.begin(), rig);
    rig_->clear();
    for (const auto &reference : rigs) rig_->addItem(qtext(reference), qtext(reference));
    rig_->setCurrentIndex(rig_->findData(qtext(rig)));
    loadClips(rigs.empty());
    const auto speed = parameters_.find("speed");
    speed_->setValue(speed != parameters_.end() && speed->is_number() ? speed->get<double>() : 1.0);
    const auto loop = parameters_.find("loop");
    loop_->setChecked(loop != parameters_.end() && loop->is_boolean() ? loop->get<bool>() : true);
    loading_ = false;
}

// The clips of the rig the node names, and the line that says what is amiss.
void GraphCanvasPoseFields::loadClips(bool no_rigs) {
    const bool was_loading = std::exchange(loading_, true);
    const QSignalBlocker block_clip{clip_};
    const auto rig = stringParameter(parameters_, "rig");
    const auto clip = stringParameter(parameters_, "clip");
    auto clips = gltfAnimationNames(projectReferenceFile(project_root_, rig));
    QString note;
    if (no_rigs)
        note = tr("The project has no .gltf, .glb or .vrm file to sample.");
    else if (rig.empty())
        note = tr("Pick the rig the clip is sampled on.");
    else if (clips.empty())
        note = tr("%1 lists no animation Studio can read.").arg(qtext(rig));
    if (!clip.empty() && std::ranges::find(clips, clip) == clips.end()) {
        clips.insert(clips.begin(), clip);
        if (note.isEmpty()) note = tr("%1 is not an animation of %2.").arg(qtext(clip), qtext(rig));
    }
    clip_->clear();
    for (const auto &name : clips) clip_->addItem(qtext(name), qtext(name));
    clip_->setCurrentIndex(clip_->findData(qtext(clip)));
    note_->setText(note);
    note_->setVisible(!note.isEmpty());
    loading_ = was_loading;
}

void GraphCanvasPoseFields::commit(const std::string &key, const nlohmann::json &value) {
    if (node_.empty()) return;
    const auto stored = parameters_.find(key);
    if (stored != parameters_.end() && *stored == value) return;
    if (!model_.setNodeParameter(scope_, node_, key, value)) return;
    parameters_[key] = value;
    // A new rig lists its own clips.
    if (key == "rig") loadClips(false);
    if (submitted_) submitted_();
}

} // namespace PelicanStudio
