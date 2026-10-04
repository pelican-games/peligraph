#include "studioplayerarguments.hpp"
#include "../../project/renderfeatureoverlay.hpp"

#include <string_view>
#include <utility>

namespace PelicanStudio {
namespace {

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.starts_with(prefix);
}

bool isFlag(std::string_view argument, std::string_view flag) {
    return argument == flag ||
           (argument.size() > flag.size() && argument.starts_with(flag) &&
            argument[flag.size()] == '=');
}

} // namespace

StudioPlayerLaunch studioPlayerLaunch(std::vector<std::string> configured,
                                      const std::string &project_root) {
    std::vector<std::string> additional;
    additional.reserve(configured.size());
    bool has_editor_transform_preset = false;
    bool has_log_file = false;
    bool has_headless = false;
    bool has_embedded = false;
    bool has_start_state = false;
    // PM-3b: what runs the game from its first frame, which an engine that
    // starts in edit refuses (offline.mode_conflict@1, PM-1b). The first one
    // configured is the one named.
    std::string runs_from_first_frame;
    for (std::size_t index = 0; index < configured.size(); ++index) {
        const auto &argument = configured[index];
        if (isFlag(argument, "--start-state")) has_start_state = true;
        for (const std::string_view flag :
             {"--replay", "--record-input", "--bake-camera-output", "--start-time"}) {
            if (runs_from_first_frame.empty() && isFlag(argument, flag))
                runs_from_first_frame = std::string{flag};
        }
        if (argument == "--rpc") {
            continue;
        }
        // BS-1s: always passed, once.
        if (argument == "--render-fallback") {
            continue;
        }
        if (argument == "--project") {
            if (index + 1 < configured.size()) {
                ++index;
            }
            continue;
        }
        if (startsWith(argument, "--project=")) {
            continue;
        }
        if (argument == "--feature-overlay") {
            if (index + 1 < configured.size()) {
                ++index;
            }
            continue;
        }
        if (startsWith(argument, "--feature-overlay=")) {
            continue;
        }
        if (argument == "--editor-transform" ||
            startsWith(argument, "--editor-transform=")) {
            has_editor_transform_preset = true;
        }
        if (argument == "--log-file" || startsWith(argument, "--log-file=")) {
            has_log_file = true;
        }
        if (argument == "--headless") {
            has_headless = true;
        }
        if (argument == "--embedded") {
            has_embedded = true;
        }
        additional.push_back(argument);
    }

    // BS-1s, decision 1 (engine BS-1): a rendering configuration the engine
    // cannot compose at start does not stop the engine Studio starts. It
    // draws one flat colour, says why in get_status.render_pipeline, and
    // takes the fixed configuration through reload_render_documents or a
    // render edit; Studio shows the refusal instead of a stopped viewport.
    std::vector<std::string> arguments{
        "--rpc",
        "--render-fallback",
        "--project",
        project_root,
        "--feature-overlay",
        std::string{Pelican::editorFeatureOverlayReference},
    };
    // EW-2: the engine's window belongs inside Studio from its first frame.
    // --embedded makes the engine create it hidden and unfocused, so nothing
    // shows outside Studio while the project loads; Studio shows it when it
    // embeds it. A configured --headless run has no window to host, and the
    // engine refuses the two together, so the flag is left out then.
    if (!has_headless && !has_embedded) {
        arguments.emplace_back("--embedded");
    }
    if (!has_editor_transform_preset) {
        arguments.emplace_back("--editor-transform");
    }
    // Each engine Studio starts writes its own log under the project it opened,
    // rather than every player appending to one pelican.log next to the
    // executable. A configured --log-file wins.
    if (!has_log_file) {
        arguments.emplace_back("--log-file");
        arguments.emplace_back(project_root + "/.pelican/logs/player.log");
    }
    // PM-3b (design_play_mode 2, 12.2): the engine Studio starts waits in
    // edit -- its behaviors asleep, its game clock still -- until Play. A
    // configured start state wins; a configured argument that runs the game
    // from its first frame cannot be combined with edit, so edit is left out
    // and the launch says why.
    std::string notice;
    if (!has_start_state) {
        if (runs_from_first_frame.empty()) {
            arguments.emplace_back("--start-state");
            arguments.emplace_back("edit");
        } else {
            notice = "The engine starts playing, not in edit: " + runs_from_first_frame +
                     " in PELICAN_STUDIO_PLAYER_ARGUMENTS runs the game from its first frame.";
        }
    }
    arguments.insert(arguments.end(), additional.begin(), additional.end());
    return {.arguments = std::move(arguments), .start_state_notice = std::move(notice)};
}

std::vector<std::string> studioPlayerArgumentStrings(
    std::vector<std::string> configured,
    const std::string &project_root) {
    return studioPlayerLaunch(std::move(configured), project_root).arguments;
}

} // namespace PelicanStudio
