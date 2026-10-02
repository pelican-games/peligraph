#include "graphcanvasposefields.hpp"

#include "graphcanvasparameterfields.hpp"
#include "studiotheme.hpp"

#include <QLabel>
#include <QVBoxLayout>

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
    : QWidget{parent} {
    setObjectName(QStringLiteral("pelican.graphCanvasPose"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // UX-12a, decision 5: the parameter fields of the op's table, under this
    // prefix; only clip is Studio's own list.
    fields_ = new GraphCanvasParameterFields(model, std::move(submitted), this,
                                             QStringLiteral("pelican.graphCanvasPose"));
    fields_->setObjectName(QStringLiteral("pelican.graphCanvasPose.fields"));
    fields_->setOps([](std::string_view op) { return op == "pose.clip_sample"; });
    fields_->setStrings([this](const std::string &name,
                               const Json &around) -> std::optional<std::vector<std::string>> {
        if (name != "clip") return std::nullopt;
        return gltfAnimationNames(projectReferenceFile(project_root_, stringParameter(around, "rig")));
    });
    fields_->setFollow([this](const Json &parameters) { showNote(parameters); });
    layout->addWidget(fields_);
    note_ = new QLabel(this);
    note_->setObjectName(QStringLiteral("pelican.graphCanvasPose.note"));
    note_->setFont(studioSmallFont());
    note_->setWordWrap(true);
    note_->hide();
    layout->addWidget(note_);
    hide();
}

void GraphCanvasPoseFields::setProjectRoot(const std::filesystem::path &root) {
    project_root_ = root;
    fields_->setProjectRoot(root);
}

bool GraphCanvasPoseFields::valuePending() const noexcept { return fields_->valuePending(); }

void GraphCanvasPoseFields::setPendingChanged(std::function<void()> changed) {
    fields_->setPendingChanged(std::move(changed));
}

void GraphCanvasPoseFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    if (node == nullptr || node->op != "pose.clip_sample") {
        fields_->showNode(nullptr, scope);
        hide();
        return;
    }
    fields_->showNode(node, scope);
    setVisible(fields_->isVisibleTo(this));
}

void GraphCanvasPoseFields::followEditable() { fields_->followEditable(); }

// The line that says what is amiss with the rig and the clip.
void GraphCanvasPoseFields::showNote(const Json &parameters) {
    const auto rig = stringParameter(parameters, "rig");
    const auto clip = stringParameter(parameters, "clip");
    const auto clips = gltfAnimationNames(projectReferenceFile(project_root_, rig));
    QString note;
    if (projectRigReferences(project_root_).empty() && rig.empty())
        note = tr("The project has no .gltf, .glb or .vrm file to sample: Import... one.");
    else if (rig.empty())
        note = tr("Pick the rig the clip is sampled on.");
    else if (clips.empty())
        note = tr("%1 lists no animation Studio can read.").arg(qtext(rig));
    else if (!clip.empty() && std::ranges::find(clips, clip) == clips.end())
        note = tr("%1 is not an animation of %2.").arg(qtext(clip), qtext(rig));
    note_->setText(note);
    note_->setVisible(!note.isEmpty());
}

} // namespace PelicanStudio
