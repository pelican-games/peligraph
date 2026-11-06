#include "../src/core/output/outputvideo.hpp"
#include "../src/core/output/outputvideoprocess.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

struct TempDirectory {
    fs::path path;
    TempDirectory() : path(fs::temp_directory_path() /
        ("pelican-vo7-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))) {
        REQUIRE(fs::create_directories(path));
    }
    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

struct Environment {
    std::string name;
    explicit Environment(std::string key) : name(std::move(key)) { set(""); }
    void set(const std::string &value) {
#ifdef _WIN32
        REQUIRE(_putenv_s(name.c_str(), value.c_str()) == 0);
#else
        REQUIRE(setenv(name.c_str(), value.c_str(), 1) == 0);
#endif
    }
    ~Environment() {
#ifdef _WIN32
        (void)_putenv_s(name.c_str(), "");
#else
        (void)unsetenv(name.c_str());
#endif
    }
};

Pelican::OutputVideoFrame frame(std::uint64_t capture,
    std::int64_t nanoseconds = 0, std::uint32_t size = 2) {
    return Pelican::OutputVideoFrame{.capture = capture,
        .monotonic_nanoseconds = nanoseconds, .time_revision = 1,
        .playing = true, .width = size, .height = size, .bgra = false};
}

std::vector<std::uint8_t> pixels(std::size_t size, std::uint8_t value) {
    std::vector<std::uint8_t> result(size);
    for (std::size_t at = 0; at < size; at += 4) {
        result[at] = value;
        result[at + 1] = static_cast<std::uint8_t>(value + 1);
        result[at + 2] = static_cast<std::uint8_t>(value + 2);
        result[at + 3] = 255;
    }
    return result;
}

std::uint64_t hashPixels(const std::vector<std::uint8_t> &data) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : data) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<std::uint64_t> sideHashes(const fs::path &side) {
    std::ifstream input{side};
    std::vector<std::uint64_t> result;
    std::string line;
    while (std::getline(input, line))
        if (line.starts_with("hash ")) result.push_back(std::stoull(line.substr(5)));
    return result;
}

std::string readText(const fs::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

bool waitFinished(Pelican::OutputVideoWriter &writer,
                  std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!writer.finished() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return writer.finished();
}

std::unique_ptr<Pelican::OutputVideoWriter> writer(const TempDirectory &temp,
    const fs::path &side, bool live = false, std::uint32_t max_frames = 0,
    std::uint64_t max_bytes = 0) {
    return std::make_unique<Pelican::OutputVideoWriter>("movie", temp.path,
        fs::path{PELICAN_FAKE_FFMPEG}, 10, 1, live, true, max_frames, max_bytes);
}

void send(Pelican::OutputVideoWriter &writer, Pelican::OutputVideoFrame metadata,
          const std::vector<std::uint8_t> &data) {
    auto reservation = writer.reserve(std::move(metadata), true);
    REQUIRE(reservation != nullptr);
    writer.complete(reservation, data);
}
}

TEST_CASE("VO-7 arguments preserve paths and specify BT709 raw H264 input", "[vo7]") {
    Pelican::OutputVideoEncoderOptions options;
    options.executable = fs::path{u8"/tmp/ffmpeg space/動画"};
    options.partial_path = fs::path{u8"/tmp/out space/海.mp4.partial"};
    options.width = 1920;
    options.height = 1080;
    options.fps_num = 60000;
    options.fps_den = 1001;
    options.bgra = true;
    const auto arguments = Pelican::outputVideoEncoderArguments(options);
    REQUIRE(arguments.front() == options.executable.string());
    REQUIRE(arguments.back() == options.partial_path.string());
    CHECK(std::find(arguments.begin(), arguments.end(), "-nostdin") != arguments.end());
    CHECK(std::find(arguments.begin(), arguments.end(), "bgra") != arguments.end());
    CHECK(std::find(arguments.begin(), arguments.end(), "60000/1001") != arguments.end());
    const auto command = Pelican::quoteOutputVideoWindowsCommandLine(
        {"plain", "a b", "quote\"inside", "trailing\\", ""});
    CHECK(command == "\"plain\" \"a b\" \"quote\\\"inside\" \"trailing\\\\\" \"\"");
}

TEST_CASE("VO-7 live position is exact for rational frame rates", "[vo7]") {
    CHECK(Pelican::outputVideoPosition(0, 60000, 1001) == 0);
    CHECK(Pelican::outputVideoPosition(50'000'000, 10, 1) == 1);
    CHECK(Pelican::outputVideoPosition(249'999'999, 10, 1) == 2);
    for (std::uint64_t index = 0; index <= 60000; ++index) {
        const auto ticks = static_cast<std::int64_t>(
            (static_cast<long double>(index) * 1001.0L * 1'000'000'000.0L) /
            60000.0L);
        CHECK(Pelican::outputVideoPosition(ticks, 60000, 1001) == index);
    }
}

TEST_CASE("VO-7 offline sends every frame once and publishes only after clean EOF",
          "[vo7]") {
    TempDirectory temp;
    Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
    const auto side_path = temp.path / "encoder side.txt";
    side.set(side_path.string());
    auto output = writer(temp, side_path);
    const auto first = pixels(16, 10);
    const auto second = pixels(16, 40);
    const auto third = pixels(16, 80);
    send(*output, frame(4, 0), first);
    send(*output, frame(9, 100'000'000), second);
    send(*output, frame(20, 200'000'000), third);
    output->requestStop();
    REQUIRE(waitFinished(*output));
    const auto status = output->status();
    CHECK(status.state == "finished");
    CHECK(status.frames_written == 3);
    CHECK(status.frames_repeated == 0);
    CHECK(sideHashes(side_path) == std::vector<std::uint64_t>{
        hashPixels(first), hashPixels(second), hashPixels(third)});
    CHECK(fs::exists(temp.path / "movie.mp4"));
    CHECK(readText(side_path).find("bytes 48") != std::string::npos);
}

TEST_CASE("VO-7 real ffmpeg writes tagged BT709 video with top-down pixels", "[vo7]") {
    const auto encoder = Pelican::findOutputVideoEncoder(std::nullopt);
#ifdef _WIN32
    const auto probe_name = "ffprobe.exe";
#else
    const auto probe_name = "ffprobe";
#endif
    if (!encoder) SKIP("real ffmpeg was not found on PATH");
    const auto probe = encoder->parent_path() / probe_name;
    if (!fs::is_regular_file(probe)) SKIP("ffprobe was not found next to real ffmpeg");
    const auto quote = [](const fs::path &path) {
#ifdef _WIN32
        return "\"" + path.string() + "\"";
#else
        auto text = path.string();
        std::string result{"'"};
        for (const char c : text) result += c == '\'' ? "'\\''" : std::string(1, c);
        return result + "'";
#endif
    };
#ifdef _WIN32
    const auto codec_probe = quote(*encoder) + " -hide_banner -h encoder=libx264 > NUL 2>&1";
#else
    const auto codec_probe = quote(*encoder) + " -hide_banner -h encoder=libx264 >/dev/null 2>&1";
#endif
    if (std::system(codec_probe.c_str()) != 0) SKIP("real ffmpeg does not provide libx264");
    TempDirectory temp;
    Pelican::OutputVideoWriter output("movie", temp.path, encoder,
                                      10, 1, false, true, 0, 0);
    std::vector<std::uint8_t> source(4 * 4 * 4);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        const auto offset = static_cast<std::size_t>((y * 4 + x) * 4);
        source[offset] = y < 2 ? 255 : 0;
        source[offset + 1] = 0;
        source[offset + 2] = y < 2 ? 0 : 255;
        source[offset + 3] = 255;
    }
    for (int index = 0; index < 3; ++index) send(output, frame(index, 0, 4), source);
    output.requestStop();
    REQUIRE(waitFinished(output, 30s));
    INFO(output.status().last_error);
    REQUIRE(output.status().state == "finished");
    const auto metadata_path = temp.path / "probe.json";
    const auto decoded_path = temp.path / "decoded.rgba";
    const auto metadata_command = quote(probe) +
        " -v error -count_frames -select_streams v:0 -show_entries " +
        "stream=width,height,pix_fmt,color_range,color_space,color_primaries," +
        "color_transfer,nb_read_frames,duration -of json " +
        quote(temp.path / "movie.mp4") + " > " + quote(metadata_path);
    REQUIRE(std::system(metadata_command.c_str()) == 0);
    const auto metadata = nlohmann::json::parse(readText(metadata_path)).at("streams").at(0);
    CHECK(metadata.at("width") == 4);
    CHECK(metadata.at("height") == 4);
    CHECK(metadata.at("pix_fmt") == "yuv420p");
    CHECK(metadata.at("color_range") == "tv");
    CHECK(metadata.at("color_space") == "bt709");
    CHECK(metadata.at("color_primaries") == "bt709");
    CHECK(metadata.at("color_transfer") == "bt709");
    CHECK(std::stoi(metadata.at("nb_read_frames").get<std::string>()) == 3);
    CHECK(std::abs(std::stod(metadata.at("duration").get<std::string>()) - 0.3) < 0.0001);
    const auto decode_command = quote(*encoder) + " -v error -nostdin -i " +
        quote(temp.path / "movie.mp4") + " -frames:v 1 -f rawvideo -pix_fmt rgba " +
        quote(decoded_path);
    REQUIRE(std::system(decode_command.c_str()) == 0);
    const auto decoded = readText(decoded_path);
    REQUIRE(decoded.size() == source.size());
    const auto channel = [&](std::size_t pixel, std::size_t component) {
        return static_cast<unsigned char>(decoded[pixel * 4 + component]);
    };
    CHECK(channel(0, 0) > 180);
    CHECK(channel(0, 1) < 80);
    CHECK(channel(0, 2) < 80);
    CHECK(channel(12, 0) < 80);
    CHECK(channel(12, 1) < 80);
    CHECK(channel(12, 2) > 180);
}

TEST_CASE("VO-7 live repeats the retained frame without using capture numbers",
          "[vo7]") {
    TempDirectory temp;
    Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
    const auto side_path = temp.path / "live.txt";
    side.set(side_path.string());
    auto output = writer(temp, side_path, true);
    const auto first = pixels(16, 10);
    const auto skipped = pixels(16, 50);
    const auto later = pixels(16, 90);
    send(*output, frame(100, 1'000'000'000), first);
    send(*output, frame(200, 1'000'000'000), skipped);
    send(*output, frame(2, 1'150'000'000), later);
    output->requestStop(1'150'000'000);
    REQUIRE(waitFinished(*output));
    const auto hashes = sideHashes(side_path);
    REQUIRE(hashes.size() >= 3);
    CHECK(hashes[0] == hashPixels(first));
    CHECK(hashes[1] == hashPixels(first));
    CHECK(hashes[2] == hashPixels(later));
    CHECK(output->status().frames_repeated >= 1);
    CHECK(output->status().frames_dropped >= 1);
}

TEST_CASE("VO-7 encoder early exit reports its code and leaves the partial",
          "[vo7]") {
    TempDirectory temp;
    Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
    Environment early{"PELICAN_FAKE_FFMPEG_EARLY_EXIT"};
    side.set((temp.path / "early.txt").string());
    early.set("23");
    auto output = writer(temp, temp.path / "early.txt");
    const auto data = pixels(512 * 512 * 4, 20);
    send(*output, frame(0, 0, 512), data);
    REQUIRE(waitFinished(*output));
    const auto status = output->status();
    CHECK(status.state == "refused");
    CHECK(status.last_error.find("output.video_encoder_failed@1") != std::string::npos);
    CHECK(status.last_error.find("exit_code=23") != std::string::npos);
    CHECK(status.last_error.find("early diagnostic tail") != std::string::npos);
    CHECK(!fs::exists(temp.path / "movie.mp4"));
}

TEST_CASE("VO-7 refuses missing encoders bad rates odd sizes and offline time jumps",
          "[vo7]") {
    TempDirectory temp;
    {
        Pelican::OutputVideoWriter output("missing", temp.path,
            temp.path / "no such ffmpeg", 10, 1, false, true, 0, 0);
        send(output, frame(0), pixels(16, 1));
        REQUIRE(waitFinished(output));
        CHECK(output.status().last_error == "output.ffmpeg_missing@1");
    }
    {
        Pelican::OutputVideoWriter output("rate", temp.path,
            fs::path{PELICAN_FAKE_FFMPEG}, 10, 1, false, false, 0, 0);
        send(output, frame(0), pixels(16, 1));
        REQUIRE(waitFinished(output));
        CHECK(output.status().last_error == "output.video_rate_mismatch@1");
    }
    {
        Pelican::OutputVideoWriter output("odd", temp.path,
            fs::path{PELICAN_FAKE_FFMPEG}, 10, 1, false, true, 0, 0);
        send(output, frame(0, 0, 3), pixels(36, 1));
        REQUIRE(waitFinished(output));
        CHECK(output.status().last_error == "output.video_dimensions_invalid@1");
    }
    {
        Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
        side.set((temp.path / "jump.txt").string());
        Pelican::OutputVideoWriter output("jump", temp.path,
            fs::path{PELICAN_FAKE_FFMPEG}, 10, 1, false, true, 0, 0);
        auto metadata = frame(0);
        metadata.playing = false;
        CHECK(output.reserve(metadata, true) == nullptr);
        REQUIRE(waitFinished(output));
        CHECK(output.status().last_error == "output.video_time_jump@1");
        CHECK(!fs::exists(temp.path / "jump.mp4"));
    }
}

TEST_CASE("VO-7 a size change finalizes the old file then refuses", "[vo7]") {
    TempDirectory temp;
    Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
    side.set((temp.path / "size.txt").string());
    auto output = writer(temp, temp.path / "size.txt");
    send(*output, frame(0), pixels(16, 10));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (output->status().encoder_pid == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    REQUIRE(output->status().encoder_pid != 0);
    send(*output, frame(1, 0, 4), pixels(64, 20));
    output->requestStop();
    REQUIRE(waitFinished(*output));
    CHECK(output->status().state == "refused");
    CHECK(output->status().last_error == "output.video_size_changed@1");
    CHECK(fs::exists(temp.path / "movie.mp4"));
}

TEST_CASE("VO-7 existing output and abnormal EOF never replace a final file", "[vo7]") {
    TempDirectory temp;
    Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
    side.set((temp.path / "exists.txt").string());
    const auto final_path = temp.path / "movie.mp4";
    {
        std::ofstream final{final_path};
        final << "keep";
    }
    auto output = writer(temp, temp.path / "exists.txt");
    send(*output, frame(0), pixels(16, 10));
    output->requestStop();
    REQUIRE(waitFinished(*output));
    CHECK(output->status().last_error.starts_with("output.exists@1"));
    CHECK(readText(final_path) == "keep");
    bool partial_found = false;
    for (const auto &entry : fs::directory_iterator(temp.path))
        partial_found = partial_found || entry.path().filename().string().ends_with(".partial");
    CHECK(partial_found);
    output.reset();
    REQUIRE(fs::remove(final_path));
    Environment failure{"PELICAN_FAKE_FFMPEG_EXIT"};
    failure.set("7");
    auto failed = writer(temp, temp.path / "exists.txt");
    send(*failed, frame(0), pixels(16, 20));
    failed->requestStop();
    REQUIRE(waitFinished(*failed));
    CHECK(failed->status().last_error.find("exit_code=7") != std::string::npos);
    CHECK(!fs::exists(final_path));
}

TEST_CASE("VO-7 heavy stderr drains and a non-reading child is killed on stop",
          "[vo7]") {
    TempDirectory temp;
    Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
    side.set((temp.path / "process.txt").string());
    {
        Environment heavy{"PELICAN_FAKE_FFMPEG_HEAVY_STDERR"};
        heavy.set("1");
        auto output = writer(temp, temp.path / "process.txt");
        const auto data = pixels(512 * 512 * 4, 30);
        send(*output, frame(0, 0, 512), data);
        send(*output, frame(1, 0, 512), data);
        output->requestStop();
        REQUIRE(waitFinished(*output));
        CHECK(output->status().state == "finished");
    }
    {
        Environment no_read{"PELICAN_FAKE_FFMPEG_NO_READ"};
        Environment timeout{"PELICAN_VIDEO_ENCODER_TIMEOUT_MS"};
        no_read.set("1");
        timeout.set("100");
        auto output = writer(temp, temp.path / "process.txt");
        const auto data = pixels(1024 * 1024 * 4, 40);
        send(*output, frame(0, 0, 1024), data);
        send(*output, frame(1, 0, 1024), data);
        output->requestStop();
        REQUIRE(waitFinished(*output, 5s));
        CHECK(output->status().state == "refused");
        CHECK(output->status().last_error.find("output.video_encoder_failed@1") !=
              std::string::npos);
    }
}

TEST_CASE("VO-7 configured ffmpeg answers its version probe", "[vo7]") {
    const auto version = Pelican::outputVideoEncoderVersion(
        fs::path{PELICAN_FAKE_FFMPEG}, 1s);
    REQUIRE(version.has_value());
    CHECK(version->find("ffmpeg version fake-1.0") != std::string::npos);
}

TEST_CASE("VO-7 frame and approximate byte limits stop and finalize normally", "[vo7]") {
    TempDirectory temp;
    Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
    side.set((temp.path / "limits.txt").string());
    {
        auto output = writer(temp, temp.path / "limits.txt", false, 2, 0);
        send(*output, frame(0), pixels(16, 10));
        send(*output, frame(1), pixels(16, 20));
        auto third = output->reserve(frame(2), false);
        if (third) output->complete(third, pixels(16, 30));
        REQUIRE(waitFinished(*output));
        CHECK(output->status().state == "finished");
        CHECK(output->status().stop_reason == "max_frames");
        CHECK(output->status().frames_written == 2);
        CHECK(fs::file_size(temp.path / "movie.mp4") != 0);
    }
    fs::remove(temp.path / "movie.mp4");
    Environment growing{"PELICAN_FAKE_FFMPEG_PARTIAL_PRUNE"};
    growing.set("1");
    auto output = writer(temp, temp.path / "limits.txt", false, 0, 1);
    send(*output, frame(0), pixels(16, 10));
    REQUIRE(waitFinished(*output));
    CHECK(output->status().state == "finished");
    CHECK(output->status().stop_reason == "max_bytes");
    CHECK(output->status().final_bytes == 32);
}

TEST_CASE("VO-7 shell metacharacters in the executable are not evaluated", "[vo7]") {
    TempDirectory temp;
    const auto executable = temp.path / "ffmpeg ; echo SHOULD_NOT_EXIST #";
    fs::copy_file(fs::path{PELICAN_FAKE_FFMPEG}, executable);
    const auto marker = temp.path / "SHOULD_NOT_EXIST";
    Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
    side.set((temp.path / "meta.txt").string());
    Pelican::OutputVideoWriter output("movie", temp.path, executable,
                                      10, 1, false, true, 0, 0);
    send(output, frame(0), pixels(16, 10));
    output.requestStop();
    REQUIRE(waitFinished(output));
    CHECK(output.status().state == "finished");
    CHECK(readText(temp.path / "meta.txt").find(executable.string()) != std::string::npos);
    CHECK(!fs::exists(marker));
}

TEST_CASE("VO-7 live rejects an encoder that falls over two seconds behind", "[vo7]") {
    TempDirectory temp;
    Environment side{"PELICAN_FAKE_FFMPEG_SIDE"};
    side.set((temp.path / "slow.txt").string());
    auto output = writer(temp, temp.path / "slow.txt", true);
    send(*output, frame(0, 100), pixels(16, 10));
    send(*output, frame(1, 3'100'000'100), pixels(16, 20));
    output->requestStop(3'100'000'100);
    REQUIRE(waitFinished(*output));
    CHECK(output->status().state == "refused");
    CHECK(output->status().last_error == "output.video_encoder_too_slow@1");
    CHECK(fs::exists(temp.path / "movie.mp4"));
}
