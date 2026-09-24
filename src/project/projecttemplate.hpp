#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace Pelican {

// F-2: the starting points a new project can be made from. The contents live
// in projecttemplate.cpp and nowhere else: `pelican_cli project init` and
// Studio's File > New Project... both call initializeProjectTemplate, so there
// is one answer to "what does a new project hold".
enum class ProjectTemplate {
    // A 3D scene on the hybrid_v1 preset with the shadow and sky features --
    // what `project init` has always written, byte for byte.
    Standard,
    // No preset and no features: one fullscreen pass, `present`, writes
    // `display` from project://shaders/present, a UV gradient. The frame the
    // engine draws is the one pass the project authored.
    Blank,
};

// The spellings the command line and the automation surface use.
[[nodiscard]] std::string_view projectTemplateName(ProjectTemplate kind) noexcept;
[[nodiscard]] std::optional<ProjectTemplate> projectTemplateFromName(std::string_view name) noexcept;
[[nodiscard]] std::span<const ProjectTemplate> projectTemplates() noexcept;

struct ProjectInitResult {
    std::filesystem::path project_root;
    int files_written = 0;
};

// Writes the chosen template into `project_dir`, creating the directory when
// it does not exist. A directory that already holds anything is refused with
// a std::runtime_error naming it; nothing is written in that case.
ProjectInitResult initializeProjectTemplate(const std::filesystem::path &project_dir,
                                            ProjectTemplate kind = ProjectTemplate::Standard);

} // namespace Pelican
