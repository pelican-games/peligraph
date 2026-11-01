// The glue thread pumps events; filesystem, shaderc and engine work stay on
// the worker so startup cannot stall Android's lifecycle acknowledgements.
#include "runtime.hpp"
#include "inputqueue.hpp"
#include "validationlog.hpp"
#include "../../core/log.hpp"
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <quill/Backend.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

int pelican_player_main(int argc, char *argv[]);

namespace {
namespace fs = std::filesystem;
constexpr const char *tag = "pelican";
fs::path pipeline_cache_directory;
using Clock = std::chrono::steady_clock;
Clock::time_point android_entry;
std::atomic<bool> close_requested{false};
std::atomic<bool> is_paused{true};
std::atomic<bool> preserve_modules{false};
std::atomic<std::uint64_t> accepted_presents{0};
std::function<void()> lifecycle_handler;
bool test_input = false;
bool test_no_maintenance = false, test_zero_acquire = false;
unsigned test_build_delay_ms = 0;
fs::path pump_log;
std::atomic<bool> first_present{false};
size_t timing_interval = 120;
size_t timed_frames = 0;
std::vector<double> frame_times;
struct PumpState {
    bool window_ready = false;
    std::optional<Clock::time_point> destroy_deadline;
};
void pumpLog(const std::string &message) {
    __android_log_write(ANDROID_LOG_INFO, tag, message.c_str());
    if (auto *file = std::fopen(pump_log.c_str(), "a")) {
        std::fprintf(file, "%.3f %s\n", std::chrono::duration<double, std::milli>(Clock::now() - android_entry).count(), message.c_str());
        std::fflush(file);
        fsync(fileno(file));
        std::fclose(file);
    }
}

// Read one whitespace-separated argv string. Quotes are not interpreted;
// app-owned paths are supplied below rather than coming from the intent.
std::vector<std::string> intentArguments(android_app *app) {
    JNIEnv *env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        throw std::runtime_error("cannot attach intent reader to JVM");
    const auto activity_class = env->GetObjectClass(app->activity->clazz);
    const auto get_intent = env->GetMethodID(activity_class, "getIntent", "()Landroid/content/Intent;");
    const auto intent = env->CallObjectMethod(app->activity->clazz, get_intent);
    const auto intent_class = env->GetObjectClass(intent);
    const auto get_extra = env->GetMethodID(intent_class, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    const auto key = env->NewStringUTF("pelican_args");
    const auto extra = static_cast<jstring>(env->CallObjectMethod(intent, get_extra, key));
    std::string text;
    if (extra) {
        const auto *chars = env->GetStringUTFChars(extra, nullptr);
        text = chars;
        env->ReleaseStringUTFChars(extra, chars);
        env->DeleteLocalRef(extra);
    }
    env->DeleteLocalRef(key);
    env->DeleteLocalRef(intent_class);
    env->DeleteLocalRef(intent);
    env->DeleteLocalRef(activity_class);
    app->activity->vm->DetachCurrentThread();
    std::istringstream input{text};
    std::vector<std::string> args;
    for (std::string word; input >> word;) args.push_back(std::move(word));
    return args;
}
using Asset = std::unique_ptr<AAsset, decltype(&AAsset_close)>;
Asset openAsset(AAssetManager *manager, const std::string &name) {
    Asset asset{AAssetManager_open(manager, name.c_str(), AASSET_MODE_STREAMING), AAsset_close};
    if (!asset) throw std::runtime_error("missing APK asset: " + name);
    return asset;
}
std::string assetText(AAssetManager *manager, const std::string &name) {
    auto asset = openAsset(manager, name);
    std::string text(static_cast<size_t>(AAsset_getLength64(asset.get())), '\0');
    size_t offset = 0;
    while (offset < text.size()) {
        const int count = AAsset_read(asset.get(), text.data() + offset, text.size() - offset);
        if (count <= 0) throw std::runtime_error("cannot read APK asset: " + name);
        offset += static_cast<size_t>(count);
    }
    return text;
}
fs::path extractProject(AAssetManager *manager, const fs::path &internal) {
    const auto version = assetText(manager, "sa1-version.txt");
    const auto project = internal / "project";
    std::ifstream previous{project / ".apk-version", std::ios::binary};
    const std::string installed{std::istreambuf_iterator<char>{previous}, {}};
    if (installed == version && fs::is_regular_file(project / "project.json")) {
        __android_log_print(ANDROID_LOG_INFO, tag, "assets: reuse version %s", version.c_str());
        return project;
    }
    const auto staging = internal / "project.stage";
    fs::remove_all(staging);
    fs::create_directories(staging);
    std::istringstream inventory{assetText(manager, "sa1-files.txt")};
    std::string name;
    size_t files = 0;
    while (std::getline(inventory, name)) {
        if (close_requested.load(std::memory_order_acquire))
            throw std::runtime_error("asset extraction stopped by DESTROY");
        const fs::path relative{name};
        if (name.empty() || relative.is_absolute() || relative.has_root_name())
            throw std::runtime_error("invalid asset path: " + name);
        for (const auto &part : relative) {
            if (part == ".." || part == ".pelican")
                throw std::runtime_error("invalid asset path: " + name);
        }
        auto asset = openAsset(manager, "project/" + name);
        const auto target = staging / relative;
        fs::create_directories(target.parent_path());
        std::ofstream output{target, std::ios::binary};
        std::array<char, 16384> buffer{};
        int count;
        while ((count = AAsset_read(asset.get(), buffer.data(), buffer.size())) > 0)
            output.write(buffer.data(), count);
        output.close();
        if (count < 0 || !output) throw std::runtime_error("asset extraction failed: " + name);
        ++files;
    }
    std::ofstream marker{staging / ".apk-version", std::ios::binary};
    marker << version;
    marker.close();
    if (!marker) throw std::runtime_error("cannot save APK asset version");
    // Only these fixed directories beneath internalDataPath are replaced.
    fs::remove_all(project);
    fs::rename(staging, project);
    __android_log_print(ANDROID_LOG_INFO, tag, "assets: extracted %zu files, version %s", files, version.c_str());
    return project;
}
void handleCommand(android_app *app, int32_t command) {
    const char *name = command == APP_CMD_INIT_WINDOW ? "INIT_WINDOW" :
        command == APP_CMD_TERM_WINDOW ? "TERM_WINDOW" : command == APP_CMD_PAUSE ? "PAUSE" :
        command == APP_CMD_RESUME ? "RESUME" : command == APP_CMD_DESTROY ? "DESTROY" : "other";
    pumpLog("app command " + std::to_string(command) + " " + name);
    auto &state = *static_cast<PumpState *>(app->userData);
    auto &slot = Pelican::Android::nativeWindowSlot();
    if (command == APP_CMD_INIT_WINDOW && app->window) {
        std::scoped_lock lock{slot.mutex};
        if (slot.window || slot.acked_revision < slot.release_revision) {
            pumpLog("INIT_WINDOW rejected: previous native revision not released");
            Pelican::Android::requestStop();
            ANativeActivity_finish(app->activity);
            return;
        }
        ANativeWindow_acquire(app->window);
        slot.window = app->window;
        ++slot.revision;
        slot.valid = true;
        slot.release_requested.store(false, std::memory_order_release);
        slot.changed.notify_all();
        state.window_ready = true;
        pumpLog("native window published revision=" + std::to_string(slot.revision));
    }
    if (command == APP_CMD_PAUSE) { is_paused.store(true); Pelican::Android::setInputPaused(true); }
    if (command == APP_CMD_RESUME) { is_paused.store(false); Pelican::Android::setInputPaused(false); }
    if (command == APP_CMD_TERM_WINDOW) {
        state.window_ready = false;
        const auto start = Clock::now();
        std::unique_lock lock{slot.mutex};
        slot.valid = false;
        slot.release_revision = slot.revision;
        slot.release_requested.store(true, std::memory_order_release);
        Pelican::Android::acknowledgeSurfaceReleaseLocked(slot);
        const bool acked = slot.changed.wait_until(lock, start + std::chrono::seconds(3), [&] {
            return slot.acked_revision >= slot.release_revision;
        });
        pumpLog(std::string("surface release ") + (acked ? "acked" : "TIMEOUT") +
            " revision=" + std::to_string(slot.release_revision) + " wait_ms=" +
            std::to_string(std::chrono::duration<double, std::milli>(Clock::now() - start).count()) +
            " live=" + std::to_string(slot.live[slot.release_revision]) +
            " quarantined=" + std::to_string(slot.quarantined[slot.release_revision]));
        if (!acked) Pelican::Android::requestStop();
    }
    if (command == APP_CMD_DESTROY) {
        state.destroy_deadline = Clock::now() + std::chrono::seconds(2);
        Pelican::Android::requestStop();
        slot.changed.notify_all();
    }
}
}
namespace Pelican::Android {
const std::filesystem::path &pipelineCacheDirectory() { return pipeline_cache_directory; }
bool closeRequested() { return close_requested.load(std::memory_order_acquire); }
bool paused() { return is_paused.load(); }
void requestStop() { close_requested.store(true, std::memory_order_release); }
bool preserveModulesForProcessExit() { return preserve_modules.load(); }
void quarantineModulesForProcessExit() { preserve_modules.store(true); requestStop(); }
void setLifecycleHandler(std::function<void()> handler) { lifecycle_handler = std::move(handler); }
void processLifecycle() { if (lifecycle_handler) lifecycle_handler(); }
std::uint64_t acceptedPresentCount() { return accepted_presents.load(); }
bool testInput() { return test_input; }
bool testNoMaintenance() { return test_no_maintenance; }
bool testZeroAcquireTimeout() { return test_zero_acquire; }
unsigned testBuildDelayMilliseconds() { return test_build_delay_ms; }
void flushFrameTimings() {
    if (frame_times.empty()) return;
    std::sort(frame_times.begin(), frame_times.end());
    const auto percentile = [](double p) {
        return frame_times[static_cast<size_t>(std::ceil(p * frame_times.size())) - 1];
    };
    LOG_INFO(logger, "Android frame CPU wall ms (includes pacing, nearest-rank): frames={}-{} count={} p50={} p95={} p99={}",
             timed_frames - frame_times.size() + 1, timed_frames, frame_times.size(),
             percentile(0.50), percentile(0.95), percentile(0.99));
    frame_times.clear();
}
void recordFrameMilliseconds(double milliseconds) {
    frame_times.push_back(milliseconds);
    ++timed_frames;
    if (frame_times.size() >= timing_interval) flushFrameTimings();
}
void recordAcceptedPresent() {
    ++accepted_presents;
    if (!first_present.exchange(true)) {
        const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - android_entry).count();
        LOG_INFO(logger, "Android first present: android_main_to_vkQueuePresentKHR_ms={} (accepted, not scanout)", ms);
    }
}
}
extern "C" void android_main(android_app *app) {
    android_entry = Clock::now();
    pump_log = fs::path{app->activity->internalDataPath} / "pump.log";
    std::ofstream{pump_log, std::ios::trunc};
    // This device defaults log.tag to S. Set only this process's threshold;
    // do not change any device properties or persistent logging settings.
    __android_log_set_minimum_priority(ANDROID_LOG_INFO);
    pumpLog("android_main: SA1e; event pump and engine use separate threads");
    PumpState pump;
    app->userData = &pump;
    app->onAppCmd = handleCommand;
    app->onInputEvent = Pelican::Android::handleInput;
    auto extra_args = intentArguments(app);
    // Explicit, process-local test switches. Never change device settings.
    std::erase_if(extra_args, [](const std::string &arg) {
        if (arg == "--sa1-test-input") { test_input = true; return true; }
        if (arg == "--sa1-test-no-maintenance") { test_no_maintenance = true; return true; }
        if (arg == "--sa1-test-short-acquire") { test_zero_acquire = true; return true; }
        if (arg == "--sa1-test-build-delay") { test_build_delay_ms = 800; return true; }
        return false;
    });
    const bool headless = std::find(extra_args.begin(), extra_args.end(), "--headless") != extra_args.end();
    std::atomic<bool> done{false};
    std::thread engine;
    const auto start_engine = [&] {
      engine = std::thread{[app, &done, extra_args, headless] {
        int result = 1;
        try {
            const fs::path internal{app->activity->internalDataPath};
            fs::create_directories(internal);
            fs::current_path(internal);
            // A failed run must never leave a previous capture looking new.
            fs::remove(internal / "sa1_posegraph.png");
            fs::remove(internal / "engine-result.txt");
            // Preserve parser/fatal stderr even before quill is initialized.
            std::freopen((internal / "stderr.log").c_str(), "w", stderr);
            std::fprintf(stderr, "logcat: minimum=%d loggable=%d write=%d\n",
                         __android_log_get_minimum_priority(),
                         __android_log_is_loggable(ANDROID_LOG_INFO, tag, ANDROID_LOG_INFO),
                         __android_log_write(ANDROID_LOG_INFO, tag, "engine: worker entered"));
            const auto project = extractProject(app->activity->assetManager, internal);
            pipeline_cache_directory = internal / "cache";
            fs::create_directories(pipeline_cache_directory);
            std::vector<std::string> args{
                "pelican_player", "--project", project.string(),
                "--user-dir", (internal / "user").string(),
                "--log-file", (internal / "engine.log").string(),
                "--vulkan-validation", "on"};
            if (headless) {
                args.insert(args.end(), {"--size", "960x540", "--render-out", (internal / "sa1_posegraph.png").string()});
            }
            bool frames_given = false;
            for (size_t i = 0; i < extra_args.size(); ++i) {
                if (extra_args[i] == "--timing-frames") {
                    if (++i == extra_args.size()) throw std::runtime_error("--timing-frames needs N");
                    const auto n = std::stoul(extra_args[i]);
                    if (n == 0 || n > 100000) throw std::runtime_error("--timing-frames must be 1..100000");
                    timing_interval = n;
                } else {
                    frames_given |= extra_args[i] == "--frames" || extra_args[i].starts_with("--frames=");
                    args.push_back(extra_args[i]);
                }
            }
            if (!frames_given) args.insert(args.end(), {"--frames", headless ? "3" : "600"});
            std::vector<char *> argv;
            for (auto &arg : args) argv.push_back(arg.data());
            argv.push_back(nullptr);
            __android_log_print(ANDROID_LOG_INFO, tag, "engine: starting project=%s pipeline-cache=%s", project.c_str(), pipeline_cache_directory.c_str());
            if (close_requested.load(std::memory_order_acquire))
                throw std::runtime_error("engine startup stopped by DESTROY");
            result = pelican_player_main(static_cast<int>(args.size()), argv.data());
        } catch (const std::exception &error) {
            std::fprintf(stderr, "engine: startup failed: %s\n", error.what());
            __android_log_print(ANDROID_LOG_ERROR, tag, "engine: startup failed: %s", error.what());
        } catch (...) {
            __android_log_write(ANDROID_LOG_ERROR, tag, "engine: unknown startup failure");
        }
        Pelican::Android::setLifecycleHandler({});
        if (Pelican::logger) {
            LOG_INFO(Pelican::logger, "Android Vulkan validation totals: warnings={} errors={}",
                     Pelican::Android::validation_warnings.load(), Pelican::Android::validation_errors.load());
        }
        quill::Backend::stop();
        std::fflush(stderr);
        std::ofstream{fs::path{app->activity->internalDataPath} / "engine-result.txt"} << result << '\n';
        __android_log_print(ANDROID_LOG_INFO, tag, "engine: returned %d", result);
        done.store(true, std::memory_order_release);
        ALooper_wake(app->looper);
      }};
    };
    if (headless) start_engine(); // SA1b remains the INIT_WINDOW exception.
    bool finishing = false;
    while (app->destroyRequested == 0 || (engine.joinable() && !done.load(std::memory_order_acquire))) {
        if (!engine.joinable() && pump.window_ready && !close_requested.load()) start_engine();
        if (!finishing && done.load(std::memory_order_acquire)) {
            ANativeActivity_finish(app->activity);
            finishing = true;
        }
        android_poll_source *source = nullptr;
        int events = 0;
        int timeout_ms = -1;
        if (pump.destroy_deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*pump.destroy_deadline - Clock::now()).count();
            if (remaining <= 0) {
                pumpLog("DESTROY engine deadline 2000ms exceeded; _exit with worker alive");
                _exit(0);
            }
            timeout_ms = static_cast<int>(remaining);
        }
        if (ALooper_pollOnce(timeout_ms, nullptr, &events, reinterpret_cast<void **>(&source)) >= 0 && source)
            source->process(app, source);
    }
    if (engine.joinable()) engine.join();
    pumpLog("DESTROY engine done; _exit");
    __android_log_write(ANDROID_LOG_INFO, tag, "android_main: destroy acknowledged; exiting process");
    _exit(0);
}
