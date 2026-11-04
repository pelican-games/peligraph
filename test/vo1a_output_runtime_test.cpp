// VO-1a: output.file through a real headless player. A 4x4 UNORM render
// target is filled by a fullscreen pass so the raw pixels, the row order, the
// file numbering and the frame the pixels came from are all visible without a
// capture transform. The player is driven over line-delimited JSON-RPC.
#include "../src/project/projecttemplate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char *root_reference = "passes/main_rendering_config.json";
constexpr const char *output_prefix = "shot";

std::string readFile(const fs::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

void writeFile(const fs::path &path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << text;
}

Json sceneColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments",
             {{"reference", "scene"},
              {"transfer", "linear"},
              {"range", "extended"}}}};
}

struct TempProject {
    fs::path root;

    explicit TempProject(std::string_view name) {
        root = fs::temp_directory_path() /
               ("pelican_vo1a_" + std::string{name} + "_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        (void)Pelican::initializeProjectTemplate(
            root, Pelican::ProjectTemplate::Blank);
    }
    ~TempProject() {
        if (std::getenv("PELICAN_VO1A_KEEP") != nullptr) return;
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path rootPath() const { return root / root_reference; }
    fs::path logPath(std::string_view name) const {
        return root / ("vo1a." + std::string{name} + ".log");
    }
    fs::path outputPath() const {
        const auto path = root / "output";
        fs::create_directories(path);
        return path;
    }
    Json readRoot() const { return Json::parse(readFile(rootPath())); }
    void writeRoot(const Json &document) const {
        writeFile(rootPath(), document.dump(2) + "\n");
    }
    void writeShader() const {
        writeFile(root / "shaders" / "vo1a_frame.frag", R"glsl(#version 460
#extension GL_GOOGLE_include_directive : enable
#include "pelican_sets.glsl"
#include "pelican_view.glsl"
#include "pelican_frame.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);

void main() {
    uint frame = pelicanFrame.frame_index.x & 255u;
    // Alpha varies with both axes, so a writer that drops or reorders the
    // fourth channel is visible in the raw file.
    outColor = vec4(inUV.x, inUV.y, float(frame) / 255.0,
                    0.25 + 0.25 * (inUV.x + inUV.y));
}
)glsl");
    }
    void writeConstantBlueShader() const {
        writeFile(root / "shaders" / "vo1a_frame.frag", R"glsl(#version 460
#extension GL_GOOGLE_include_directive : enable
#include "pelican_sets.glsl"
#include "pelican_view.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);

void main() {
    outColor = vec4(inUV.x, inUV.y, 0.25, 1.0);
}
)glsl");
    }
};

Json targetNode(std::string id, std::string format) {
    return {{"id", std::move(id)},
            {"op", "render.image"},
            {"parameters",
             {{"type", sceneColorType()},
              {"format", std::move(format)},
              {"width", 4},
              {"height", 4},
              {"extent_scale", 1.0},
              {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}},
            {"outputs", Json::array({"resource"})}};
}

Json fillNode(std::string id, std::string target_id,
              std::string input_node = {}, std::string input_port = "resource") {
    (void)target_id;
    Json inputs = Json::object();
    if (!input_node.empty()) {
        inputs["color"] = {{"node", std::move(input_node)},
                           {"port", std::move(input_port)}};
    }
    return {{"id", std::move(id)},
            {"op", "render.fullscreen"},
            {"parameters",
             {{"pass_set", "main_render"},
              {"shader",
               {{"vertex", "engine://fullscreen"},
                {"fragment", "project://shaders/vo1a_frame"}}},
              {"ports",
               {{"color",
                 {{"kind", "image"},
                  {"type", sceneColorType()},
                  {"access", "write"},
                  {"attachment", "color"},
                  {"location", 0}}}}}}},
            {"inputs", std::move(inputs)},
            {"outputs", Json::array({"color"})}};
}

Json sinkNode(std::string name, std::string source = "fill",
              std::string port = "color", std::uint64_t every_n = 1,
              bool enabled = true) {
    return {{"id", "sink"},
            {"op", "output.file"},
            {"parameters",
             {{"name", std::move(name)},
              {"every_n", every_n},
              {"enabled", enabled}}},
            {"inputs",
             {{"source", {{"node", std::move(source)}, {"port", std::move(port)}}}}},
            {"outputs", Json::array()}};
}

void attachMainGraph(Json &root, Json target, Json fill, Json sink) {
    auto &nodes = root["nodes"];
    nodes.push_back(std::move(target));
    nodes.push_back(std::move(fill));
    nodes.push_back(std::move(sink));
}

struct FrameFile {
    fs::path path;
    std::uint64_t frame = 0;
    std::vector<std::uint8_t> bytes;

    std::uint32_t le32(std::size_t at) const {
        REQUIRE(bytes.size() >= at + 4);
        return static_cast<std::uint32_t>(bytes[at]) |
               (static_cast<std::uint32_t>(bytes[at + 1]) << 8u) |
               (static_cast<std::uint32_t>(bytes[at + 2]) << 16u) |
               (static_cast<std::uint32_t>(bytes[at + 3]) << 24u);
    }
    std::uint32_t width() const { return le32(4); }
    std::uint32_t height() const { return le32(8); }
    std::uint32_t format() const { return le32(12); }
    const std::uint8_t *pixel(std::uint32_t x, std::uint32_t y) const {
        return bytes.data() + 16 + (static_cast<std::size_t>(y) * width() + x) * 4;
    }
};

std::optional<std::uint64_t> fileFrame(const fs::path &path) {
    const auto name = path.filename().string();
    if (!name.starts_with(output_prefix)) return std::nullopt;
    if (name.size() < 5 || name.substr(name.size() - 5) != ".rgba") return std::nullopt;
    const auto underscore = name.rfind('_');
    if (underscore == std::string::npos) return std::nullopt;
    const auto digits = name.substr(underscore + 1, name.size() - underscore - 6);
    if (digits.size() != 6) return std::nullopt;
    if (digits.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    return static_cast<std::uint64_t>(std::stoull(digits));
}

std::vector<FrameFile> frameFiles(const fs::path &directory) {
    std::vector<fs::path> paths;
    std::error_code ignored;
    for (const auto &entry : fs::directory_iterator{directory, ignored}) {
        if (!entry.is_regular_file()) continue;
        if (fileFrame(entry.path())) paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    std::vector<FrameFile> files;
    for (const auto &path : paths) {
        const auto frame = fileFrame(path);
        REQUIRE(frame);
        FrameFile file;
        file.path = path;
        file.frame = *frame;
        const auto bytes = readFile(path);
        file.bytes.assign(bytes.begin(), bytes.end());
        files.push_back(std::move(file));
    }
    return files;
}

std::vector<fs::path> allFilesWithSuffix(const fs::path &root,
                                         std::string_view suffix) {
    std::vector<fs::path> matches;
    std::error_code ignored;
    for (const auto &entry : fs::recursive_directory_iterator{root, ignored}) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            matches.push_back(entry.path());
    }
    return matches;
}

void requirePvo1(const FrameFile &file, std::uint32_t format,
                 std::uint32_t width = 4, std::uint32_t height = 4) {
    INFO(file.path.string());
    REQUIRE(file.bytes.size() == 16 + static_cast<std::size_t>(width) * height * 4);
    CHECK(file.bytes.at(0) == 'P');
    CHECK(file.bytes.at(1) == 'V');
    CHECK(file.bytes.at(2) == 'O');
    CHECK(file.bytes.at(3) == '1');
    CHECK(file.width() == width);
    CHECK(file.height() == height);
    CHECK(file.format() == format);
    CHECK_FALSE(fs::exists(file.path.string() + ".partial"));
}

#ifndef _WIN32
class PlayerRpc {
  public:
    PlayerRpc(const TempProject &project, std::vector<std::string> extra,
              std::string_view name) {
        log_ = project.logPath(name);
        int input[2]{};
        int output[2]{};
        REQUIRE(pipe(input) == 0);
        REQUIRE(pipe(output) == 0);
        process_ = fork();
        REQUIRE(process_ >= 0);
        if (process_ == 0) {
            (void)dup2(input[0], STDIN_FILENO);
            (void)dup2(output[1], STDOUT_FILENO);
            const int error_fd =
                open(log_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (error_fd >= 0) (void)dup2(error_fd, STDERR_FILENO);
            ::close(input[0]);
            ::close(input[1]);
            ::close(output[0]);
            ::close(output[1]);
            if (error_fd >= 0) ::close(error_fd);
            const fs::path player{PELICAN_VO1A_PLAYER};
            const auto working = player.parent_path();
            if (!working.empty()) (void)chdir(working.c_str());
            std::vector<std::string> arguments{
                player.string(),
                "--project",
                project.root.string(),
                "--rpc",
                "--headless",
                "--size",
                "64x64",
                "--log-file",
                log_.string()};
            for (auto &argument : extra) arguments.push_back(std::move(argument));
            std::vector<char *> argv;
            argv.reserve(arguments.size() + 1);
            for (auto &argument : arguments) argv.push_back(argument.data());
            argv.push_back(nullptr);
            execv(argv.front(), argv.data());
            _exit(127);
        }
        ::close(input[0]);
        ::close(output[1]);
        input_ = input[1];
        output_ = output[0];
        reader_ = std::thread([this] { readLoop(); });
    }
    ~PlayerRpc() { close(); }

    PlayerRpc(const PlayerRpc &) = delete;
    PlayerRpc &operator=(const PlayerRpc &) = delete;

    Json result(std::string_view method,
                const Json &params = Json::object()) {
        const int id = next_id_++;
        Json request{{"jsonrpc", "2.0"},
                     {"id", id},
                     {"method", std::string{method}},
                     {"params", params}};
        const auto line = request.dump() + "\n";
        writeAll(line);
        const auto response = nextLine();
        INFO(response.dump());
        REQUIRE(response.contains("id"));
        CHECK(response.at("id") == id);
        if (response.contains("error")) {
            FAIL("JSON-RPC " << method << " failed: " << response.at("error").dump());
        }
        REQUIRE(response.contains("result"));
        return response.at("result");
    }

    Json outputStatus() {
        const auto status = result("get_status");
        REQUIRE(status.contains("output"));
        return status.at("output");
    }

    // A render edit or reload is a ticket; the editor settles it at a frame
    // boundary, which a headless player reaches by stepping.
    Json settle(const Json &accepted) {
        INFO(accepted.dump());
        if (accepted.value("status", std::string{}) == "rejected") return accepted;
        REQUIRE(accepted.at("status") == "accepted");
        for (int attempt = 0; attempt < 32; ++attempt) {
            (void)result("step_frame");
            auto settled =
                result("get_edit_result", {{"ticket", accepted.at("ticket")}});
            if (settled.value("status", std::string{}) != "pending") return settled;
        }
        FAIL("the edit ticket stayed pending");
        return Json{};
    }

    void close() {
        if (closed_) return;
        closed_ = true;
        if (input_ >= 0) {
            ::close(input_);
            input_ = -1;
        }
        if (reader_.joinable()) reader_.join();
        if (process_ > 0) {
            int status = 0;
            bool reaped = false;
            for (int attempt = 0; attempt < 300; ++attempt) {
                const auto waited = waitpid(process_, &status, WNOHANG);
                if (waited == process_) {
                    reaped = true;
                    break;
                }
                if (waited < 0 && errno != EINTR) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
            if (!reaped) {
                (void)kill(process_, SIGTERM);
                for (int attempt = 0; attempt < 50; ++attempt) {
                    if (waitpid(process_, &status, WNOHANG) == process_) {
                        reaped = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{100});
                }
            }
            if (!reaped) {
                (void)kill(process_, SIGKILL);
                (void)waitpid(process_, &status, 0);
            }
            process_ = -1;
            (void)status;
        }
        if (output_ >= 0) {
            ::close(output_);
            output_ = -1;
        }
    }

  private:
    void writeAll(const std::string &line) {
        std::size_t offset = 0;
        while (offset < line.size()) {
            const auto written = write(input_, line.data() + offset,
                                       line.size() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                FAIL("cannot write to the player: " << std::strerror(errno));
            }
            offset += static_cast<std::size_t>(written);
        }
    }

    Json nextLine() {
        std::unique_lock lock{mutex_};
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{300};
        if (!arrived_.wait_until(lock, deadline,
                                  [this] { return !lines_.empty() || closed_; })) {
            lock.unlock();
            FAIL("the player did not answer; see " << log_.string());
        }
        if (lines_.empty()) {
            lock.unlock();
            FAIL("the player exited before answering; see " << log_.string());
        }
        auto line = std::move(lines_.front());
        lines_.pop_front();
        return Json::parse(line);
    }

    void readLoop() {
        std::string pending;
        char buffer[4096];
        for (;;) {
            const auto read_bytes = read(output_, buffer, sizeof buffer);
            if (read_bytes < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (read_bytes == 0) break;
            pending.append(buffer, static_cast<std::size_t>(read_bytes));
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                auto line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                {
                    std::lock_guard lock{mutex_};
                    lines_.push_back(std::move(line));
                }
                arrived_.notify_all();
            }
        }
        {
            std::lock_guard lock{mutex_};
            closed_ = true;
        }
        arrived_.notify_all();
    }

    int input_ = -1;
    int output_ = -1;
    pid_t process_ = -1;
    fs::path log_;
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<std::string> lines_;
    bool closed_ = false;
    int next_id_ = 1;
};

std::vector<std::string> outputFlags(const fs::path &directory,
                                     std::uint64_t max_frames = 1000,
                                     std::uint64_t max_bytes = 4ULL * 1024 * 1024 * 1024) {
    return {"--output-dir", directory.string(),
            "--output-max-frames", std::to_string(max_frames),
            "--output-max-bytes", std::to_string(max_bytes)};
}

Json oneSink(const Json &status) {
    REQUIRE(status.at("sinks").is_array());
    REQUIRE(status.at("sinks").size() == 1);
    return status.at("sinks").at(0);
}
#endif

} // namespace

#ifdef _WIN32

TEST_CASE("VO-1a the file sink runtime is exercised through a headless player",
          "[vo1a-gpu][gpu]") {
    SKIP("VO-1a's GPU runtime driver is POSIX; the Windows player driver is "
         "not part of this worktree's run");
}

#else

TEST_CASE("VO-1a every_n writes numbered PVO1 files with the target's pixels and rows",
          "[vo1a-gpu][gpu]") {
    TempProject project{"pattern"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 2));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "pattern"};

    for (int frame = 0; frame < 6; ++frame) (void)player.result("step_frame");

    // The lowering's retain becomes the target plan's readback reason.
    {
        const auto plan = player.result("get_frame_plan");
        const auto &resources =
            plan.at("physical_target_plan").at("lowering_graph").at("resources");
        bool found = false;
        for (const auto &resource : resources) {
            if (resource.at("name") != "fill_target") continue;
            found = true;
            CHECK(resource.at("lifetime").at("retain") ==
                  Json::array({"readback"}));
        }
        CHECK(found);
    }

    const auto status = player.outputStatus();
    INFO(status.dump());
    CHECK(status.at("active") == true);
    const auto sink = oneSink(status);
    CHECK(sink.at("name") == "shot");
    CHECK(sink.at("kind") == "file");
    CHECK(sink.at("state") == "running");
    CHECK(sink.at("last_error") == "");
    // Six published frames at every_n 2 are exactly frames 0, 2 and 4.
    const auto files = frameFiles(output_dir);
    INFO(files.size());
    REQUIRE(files.size() == 3);
    CHECK(files.at(0).frame == 0);
    CHECK(files.at(1).frame == 2);
    CHECK(files.at(2).frame == 4);
    CHECK(sink.at("frames_written") == files.size());
    REQUIRE(sink.at("frames_written").get<std::uint64_t>() == 3);

    std::vector<std::uint8_t> blues;
    for (const auto &file : files) {
        requirePvo1(file, 0);
        CHECK(file.frame % 2 == 0);
        const auto blue = file.pixel(0, 0)[2];
        blues.push_back(blue);
        // The shader writes the engine's evaluation index (1 on the first
        // step), while the file name is the zero-based logical frame.
        CHECK(static_cast<std::uint64_t>(blue) == file.frame + 1);
        std::array<std::uint8_t, 4> reds{};
        std::array<std::uint8_t, 4> greens{};
        for (std::uint32_t y = 0; y < 4; ++y) {
            greens[y] = file.pixel(0, y)[1];
            for (std::uint32_t x = 0; x < 4; ++x) {
                const auto *pixel = file.pixel(x, y);
                INFO(file.path.string() << " pixel " << x << "," << y);
                if (y == 0) reds[x] = pixel[0];
                CHECK(pixel[0] == reds[x]);
                CHECK(pixel[1] == greens[y]);
                // The shader's alpha varies with x and y and is never 255:
                // raw pixels must keep it, not force opaque.
                CHECK(pixel[3] < 255);
                CHECK(pixel[3] > 0);
                CHECK(pixel[2] == blue);
                if (x > 0) {
                    CHECK(pixel[0] > file.pixel(x - 1, y)[0]);
                    CHECK(pixel[3] > file.pixel(x - 1, y)[3]);
                }
                if (y > 0) CHECK(pixel[3] > file.pixel(x, y - 1)[3]);
            }
        }
        // Row 0 is the first row in the file, and UV y grows with it.
        CHECK(greens[0] < greens[1]);
        CHECK(greens[1] < greens[2]);
        CHECK(greens[2] < greens[3]);
        CHECK(reds[0] < reds[1]);
        CHECK(reds[1] < reds[2]);
        CHECK(reds[2] < reds[3]);
    }
    // Every captured file carries that frame's pixels, not the first frame's.
    for (std::size_t index = 1; index < blues.size(); ++index)
        CHECK(blues[index] != blues[index - 1]);
    for (std::size_t index = 1; index < files.size(); ++index)
        CHECK(files[index].frame == files[index - 1].frame + 2);
    CHECK(allFilesWithSuffix(output_dir, ".partial").empty());
}

TEST_CASE("VO-1a without --output-dir the sink table is inactive and writes nothing",
          "[vo1a-gpu][gpu]") {
    TempProject project{"inactive"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    PlayerRpc player{project, {}, "inactive"};

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    CHECK(status.at("active") == false);
    const auto sink = oneSink(status);
    CHECK(sink.at("name") == "shot");
    CHECK(sink.at("state") == "inactive");
    CHECK(sink.at("frames_written") == 0);
    CHECK(sink.at("last_error") == "");
    CHECK(allFilesWithSuffix(project.root, ".rgba").empty());
}

TEST_CASE("VO-1a a disabled sink writes nothing and asks for no retain",
          "[vo1a-gpu][gpu]") {
    TempProject project{"disabled"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 1, false));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "disabled"};

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    CHECK(status.at("active") == true);
    CHECK(status.at("sinks").empty());
    CHECK(allFilesWithSuffix(project.root, ".rgba").empty());

    const auto plan = player.result("get_frame_plan");
    const auto &resources =
        plan.at("physical_target_plan").at("lowering_graph").at("resources");
    for (const auto &resource : resources) {
        if (!resource.contains("lifetime") ||
            !resource["lifetime"].contains("retain"))
            continue;
        const auto &retain = resource["lifetime"]["retain"];
        CHECK(std::find(retain.begin(), retain.end(), "readback") == retain.end());
    }
}

TEST_CASE("VO-1a --output-max-frames finishes the sink after one file",
          "[vo1a-gpu][gpu]") {
    TempProject project{"limit"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir, 1), "limit"};

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("frames_written") == 1);
    const auto files = frameFiles(output_dir);
    REQUIRE(files.size() == 1);
    requirePvo1(files.front(), 0);
}

TEST_CASE("VO-1a --output-max-bytes counts the header and finishes before one file",
          "[vo1a-gpu][gpu]") {
    TempProject project{"bytes"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    // One 4x4 RGBA8 file is a 16-byte PVO1 header plus 64 pixel bytes. A
    // 65-byte budget admits no complete file, so the header must be counted.
    PlayerRpc player{project, outputFlags(output_dir, 1000, 65), "bytes"};

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "finished");
    CHECK(sink.at("frames_written") == 0);
    CHECK(sink.at("last_error") == "output.byte_limit@1");
    CHECK(frameFiles(output_dir).empty());
}

TEST_CASE("VO-1a an existing final file is untouched and refused without ending the frame",
          "[vo1a-gpu][gpu]") {
    TempProject project{"exists"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    const auto occupied = output_dir / "shot_000000.rgba";
    writeFile(occupied, "this file is not a PVO1 frame");
    PlayerRpc player{project, outputFlags(output_dir), "exists"};

    // The request must succeed: a sink refusal is reported, it does not end
    // the frame or the engine.
    (void)player.result("step_frame");
    (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "refused");
    CHECK(sink.at("frames_written") == 0);
    CHECK(sink.at("last_error").get<std::string>().starts_with("output.exists@1"));
    CHECK(readFile(occupied) == "this file is not a PVO1 frame");
    CHECK(frameFiles(output_dir).size() == 1);
    CHECK(allFilesWithSuffix(output_dir, ".partial").empty());
}

TEST_CASE("VO-1a a non-RGBA8 target is refused by the readback without a file",
          "[vo1a-gpu][gpu]") {
    TempProject project{"format"};
    project.writeShader();
    auto root = project.readRoot();
    root["rendering"]["render_targets"] = Json::array(
        {{{"name", "external_hdr"},
          {"width", 4},
          {"height", 4},
          {"extent_scale", 1.0},
          {"format", "R16G16B16A16_SFLOAT"},
          {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}});
    Json external{{"id", "external"},
                  {"op", "render.target_ref"},
                  {"parameters",
                   {{"name", "external_hdr"}, {"type", sceneColorType()}}},
                  {"inputs", Json::object()},
                  {"outputs", Json::array({"resource"})}};
    attachMainGraph(root, std::move(external),
                    fillNode("fill", "external_hdr", "external"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "format"};

    (void)player.result("step_frame");
    (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "refused");
    CHECK(sink.at("frames_written") == 0);
    CHECK(sink.at("last_error").get<std::string>().starts_with(
        "output.format_unsupported@1"));
    CHECK(frameFiles(output_dir).empty());
}

TEST_CASE("VO-1a a later writer of the target refuses the sink before any pixel is read",
          "[vo1a-gpu][gpu]") {
    TempProject project{"overwritten"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    root["nodes"].push_back(
        fillNode("later_fill", "fill_target", "fill", "color"));
    // Keep both writers in the executed graph: a pass that only writes an
    // otherwise unconsumed target is dead and would not reach the plan.
    root["rendering"]["rendering_passes"][0]["passes"][0]["input"] =
        Json::array({"fill_target"});
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "overwritten"};

    (void)player.result("step_frame");

    const auto status = player.outputStatus();
    INFO(status.dump());
    const auto sink = oneSink(status);
    CHECK(sink.at("state") == "refused");
    CHECK(sink.at("frames_written") == 0);
    const auto error = sink.at("last_error").get<std::string>();
    CHECK(error.starts_with("output.source_overwritten@1"));
    CHECK(error.find("later_fill") != std::string::npos);
    CHECK(frameFiles(output_dir).empty());
}

TEST_CASE("VO-1a reloading a changed every_n keeps the counters and writes every frame",
          "[vo1a-gpu][gpu]") {
    TempProject project{"reload"};
    project.writeShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "R8G8B8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot", "fill", "color", 2));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "reload"};

    for (int frame = 0; frame < 4; ++frame) (void)player.result("step_frame");
    const auto before = frameFiles(output_dir);
    REQUIRE(before.size() >= 2);
    const auto before_status = player.outputStatus();
    const auto before_written =
        oneSink(before_status).at("frames_written").get<std::uint64_t>();
    CHECK(before_written == before.size());

    auto changed = project.readRoot();
    for (auto &node : changed.at("nodes")) {
        if (node.at("op") == "output.file") node["parameters"]["every_n"] = 1;
    }
    project.writeRoot(changed);
    const auto settled = player.settle(player.result("reload_render_documents"));
    INFO(settled.dump(2));
    REQUIRE(settled.at("status") == "committed");
    CHECK(settled.at("operation") == "reload_render_documents");

    for (int frame = 0; frame < 3; ++frame) (void)player.result("step_frame");
    const auto after = frameFiles(output_dir);
    REQUIRE(after.size() > before.size());
    bool saw_odd = false;
    for (const auto &file : after) {
        requirePvo1(file, 0);
        if (file.frame % 2 == 1) saw_odd = true;
    }
    CHECK(saw_odd);
    const auto after_status = player.outputStatus();
    INFO(after_status.dump());
    const auto after_written =
        oneSink(after_status).at("frames_written").get<std::uint64_t>();
    CHECK(after_written == after.size());
    CHECK(after_written > before_written);
}

TEST_CASE("VO-1a a BGRA8 target writes its memory order and format code",
          "[vo1a-gpu][gpu]") {
    TempProject project{"bgra"};
    project.writeConstantBlueShader();
    auto root = project.readRoot();
    attachMainGraph(root, targetNode("fill_target", "B8G8R8A8_UNORM"),
                    fillNode("fill", "fill_target", "fill_target"),
                    sinkNode("shot"));
    project.writeRoot(root);
    const auto output_dir = project.outputPath();
    PlayerRpc player{project, outputFlags(output_dir), "bgra"};

    for (int frame = 0; frame < 2; ++frame) (void)player.result("step_frame");
    const auto files = frameFiles(output_dir);
    REQUIRE(files.size() >= 1);
    const auto &file = files.front();
    requirePvo1(file, 2);
    std::array<std::uint8_t, 4> reds{};
    std::array<std::uint8_t, 4> greens{};
    for (std::uint32_t y = 0; y < 4; ++y) {
        greens[y] = file.pixel(0, y)[1];
        for (std::uint32_t x = 0; x < 4; ++x) {
            const auto *pixel = file.pixel(x, y);
            INFO(file.path.string() << " pixel " << x << "," << y);
            // B8G8R8A8_UNORM stores B, G, R, A.
            if (y == 0) reds[x] = pixel[2];
            CHECK(pixel[0] == 64);
            CHECK(pixel[2] == reds[x]);
            CHECK(pixel[1] == greens[y]);
            CHECK(pixel[3] == 255);
        }
    }
    CHECK(greens[0] < greens[3]);
    CHECK(reds[0] < reds[3]);
}

#endif
