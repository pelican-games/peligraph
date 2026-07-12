#include "bakecameracommand.hpp"

#include <argparse/argparse.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican::DevCli {

namespace {

std::filesystem::path canonicalExisting(const std::filesystem::path &path, std::string_view label) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    if (error) {
        throw std::runtime_error(std::string{label} + " not found: " + path.string());
    }
    return canonical;
}

std::filesystem::path projectRoot(const std::filesystem::path &argument) {
    const auto path = canonicalExisting(argument, "--project");
    if (std::filesystem::is_directory(path)) {
        if (!std::filesystem::is_regular_file(path / "project.json")) {
            throw std::runtime_error("--project directory has no project.json: " + path.string());
        }
        return path;
    }
    if (path.filename() != "project.json") {
        throw std::runtime_error("--project file must be named project.json");
    }
    return path.parent_path();
}

void validateDeliveryName(std::string_view name) {
    if (name.empty()) {
        throw std::runtime_error("--name must not be empty");
    }
    for (const auto character : name) {
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '_';
        if (!valid) {
            throw std::runtime_error("--name must match [a-zA-Z0-9_]+");
        }
    }
}

std::string quoteCommandArgument(const std::filesystem::path &path) {
    const auto value = path.string();
    if (value.find('"') != std::string::npos) {
        throw std::runtime_error("process argument contains an unsupported quote: " + value);
    }
    return '"' + value + '"';
}

std::string fileSha256(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error("camera bake output was not created: " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    const auto value = contents.str();
    return picosha2::hash256_hex_string(value.begin(), value.end());
}

std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

void writeManifest(const std::filesystem::path &path, const std::filesystem::path &project,
                   const std::filesystem::path &replay, const std::string &output_name,
                   const std::string &sha256) {
    const auto manifest = nlohmann::json{
        {"created", utcTimestamp()},
        {"outputs", nlohmann::json::array({nlohmann::json{{"file", output_name},
                                                           {"schema", "pelican.transform_seq"},
                                                           {"sha256", sha256},
                                                           {"version", 1}}})},
        {"schema", "pelican.import"},
        {"source", {{"file", (project / "project.json").generic_string()},
                    {"replay", replay.generic_string()},
                    {"session", "camera_replay_bake"}}},
        {"tool", {{"name", "pelican_cli"}, {"version", "wp89-v1"}}},
        {"version", 1},
    };
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
        throw std::runtime_error("failed to write camera bake manifest: " + path.string());
    }
    output << manifest.dump(2) << '\n';
}

} // namespace

int runBakeCameraCommand(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Cli bake-camera");
    program.add_argument("--replay").required().metavar("path.jsonl");
    program.add_argument("--project").required().metavar("dir|project.json");
    program.add_argument("--player").default_value(std::string{}).metavar("pelican_player");
    program.add_argument("--name").default_value(std::string{"camera_recording"}).metavar("delivery_name");

    try {
        program.parse_args(argc, argv);
        const auto replay = canonicalExisting(program.get<std::string>("--replay"), "--replay");
        const auto project = projectRoot(program.get<std::string>("--project"));
        const auto name = program.get<std::string>("--name");
        validateDeliveryName(name);

        std::filesystem::path player;
        const auto player_argument = program.get<std::string>("--player");
        if (!player_argument.empty()) {
            player = canonicalExisting(player_argument, "--player");
        } else {
            auto executable = std::filesystem::absolute(argv[0]);
#ifdef _WIN32
            player = canonicalExisting(executable.parent_path() / "pelican_player.exe", "pelican_player");
#else
            player = canonicalExisting(executable.parent_path() / "pelican_player", "pelican_player");
#endif
        }

        const auto delivery = project / "imports" / "pelican-camera" / name;
        std::error_code error;
        std::filesystem::create_directories(delivery, error);
        if (error) {
            throw std::runtime_error("failed to create camera delivery directory: " + delivery.string() +
                                     " (" + error.message() + ")");
        }
        const std::string sequence_name = "camera.transform_seq.jsonl";
        const auto sequence_path = delivery / sequence_name;

        auto command = quoteCommandArgument(player) + " --headless --project " +
                       quoteCommandArgument(project) + " --size 64x64 --replay " +
                       quoteCommandArgument(replay) + " --bake-camera-output " +
                       quoteCommandArgument(sequence_path);
#ifdef _WIN32
        // cmd.exe consumes the first quote as its own command-string delimiter.
        // An outer pair is therefore required when the executable path itself is quoted.
        command = '"' + command + '"';
#endif
        const auto process_result = std::system(command.c_str());
        if (process_result != 0) {
            throw std::runtime_error("pelican_player camera replay failed with exit code " +
                                     std::to_string(process_result));
        }

        const auto sha256 = fileSha256(sequence_path);
        writeManifest(delivery / "manifest.json", project, replay, sequence_name, sha256);
        std::cout << delivery.generic_string() << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        std::cerr << program;
        return -1;
    }
}

} // namespace Pelican::DevCli
