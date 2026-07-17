#include "../src/core/renderdoc/renderdoccapture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace Pelican {
namespace {

struct TempCaptureFile {
    std::filesystem::path root;
    std::filesystem::path capture;

    explicit TempCaptureFile(bool write_contents = true) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("pelican_renderdoc_capture_" + std::to_string(nonce));
        std::filesystem::create_directories(root);
        capture = root / "frame.rdc";
        std::ofstream file{capture, std::ios::binary};
        if (write_contents) file << "renderdoc capture fixture";
    }

    ~TempCaptureFile() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

struct FakeRenderDoc {
    std::filesystem::path capture_path;
    std::uint32_t capture_count = 4;
    std::uint32_t count_increment = 1;
    std::uint64_t timestamp = 123456;
    bool capturing = false;
    bool start_succeeds = true;
    bool end_succeeds = true;
    bool capture_lookup_succeeds = true;
    int disable_keys_calls = 0;
    int start_calls = 0;
    int end_calls = 0;
    int discard_calls = 0;
    int get_capture_calls = 0;
    std::function<void()> on_end;

    RenderDocApiTable table() {
        return {
            .disable_capture_keys = [this] { ++disable_keys_calls; },
            .get_num_captures = [this] { return capture_count; },
            .get_capture = [this](std::uint32_t index, char *filename,
                                  std::uint32_t *length,
                                  std::uint64_t *out_timestamp) {
                ++get_capture_calls;
                if (!capture_lookup_succeeds || index >= capture_count) return 0U;
                const auto value = capture_path.generic_string();
                const auto required = static_cast<std::uint32_t>(value.size() + 1);
                if (length != nullptr) {
                    if (filename != nullptr && *length >= required) {
                        std::memcpy(filename, value.c_str(), required);
                    }
                    *length = required;
                }
                if (out_timestamp != nullptr) *out_timestamp = timestamp;
                return 1U;
            },
            .start_frame_capture = [this](void *, void *) {
                ++start_calls;
                capturing = start_succeeds;
            },
            .end_frame_capture = [this](void *, void *) {
                ++end_calls;
                if (on_end) on_end();
                if (!end_succeeds) return 0U;
                capturing = false;
                capture_count += count_increment;
                return 1U;
            },
            .discard_frame_capture = [this](void *, void *) {
                ++discard_calls;
                capturing = false;
                return 1U;
            },
            .is_frame_capturing = [this] { return capturing ? 1U : 0U; },
        };
    }
};

template <class Function>
void requireCaptureReason(std::string_view expected, Function &&function) {
    try {
        function();
        FAIL("expected RenderDocCaptureError");
    } catch (const RenderDocCaptureError &error) {
        CHECK(error.reason() == std::string{expected});
    }
}

} // namespace

TEST_CASE("RenderDoc capture owns one request and returns the actual new capture",
          "[renderdoc][capture]") {
    TempCaptureFile file;
    FakeRenderDoc fake;
    fake.capture_path = file.capture;
    RenderDocCapture capture{fake.table(), "1.7.0"};

    REQUIRE(fake.disable_keys_calls == 1);
    REQUIRE(capture.state() == RenderDocCaptureState::idle);
    REQUIRE(capture.status().api_version == "1.7.0");

    capture.request(RenderDocCaptureSource::f11, false);
    REQUIRE(capture.state() == RenderDocCaptureState::armed);
    try {
        capture.request(RenderDocCaptureSource::rpc, false);
        FAIL("expected busy RenderDocCaptureError");
    } catch (const RenderDocCaptureError &error) {
        CHECK(error.reason() == "capture_busy");
        CHECK(std::string{error.what()}.find("rpc capture rejected") !=
              std::string::npos);
        CHECK(std::string{error.what()}.find("f11 capture is armed") !=
              std::string::npos);
    }
    REQUIRE(capture.state() == RenderDocCaptureState::armed);

    fake.on_end = [&] {
        CHECK(capture.state() == RenderDocCaptureState::completing);
    };
    int render_calls = 0;
    const auto result = capture.captureArmedFrame(77, reinterpret_cast<void *>(1),
                                                   reinterpret_cast<void *>(2), [&] {
        CHECK(capture.state() == RenderDocCaptureState::capturing);
        ++render_calls;
    });

    CHECK(render_calls == 1);
    CHECK(fake.start_calls == 1);
    CHECK(fake.end_calls == 1);
    CHECK(fake.get_capture_calls == 2);
    CHECK(result.capture_index == 4);
    CHECK(result.frame_index == 77);
    CHECK(result.timestamp == fake.timestamp);
    CHECK(result.path == std::filesystem::canonical(file.capture));
    CHECK(capture.state() == RenderDocCaptureState::idle);
    CHECK_FALSE(capture.status().source.has_value());
}

TEST_CASE("RenderDoc request rejects unavailable XR busy and shutdown boundaries",
          "[renderdoc][state]") {
    SECTION("not injected") {
        RenderDocCapture capture{"renderdoc_not_injected"};
        CHECK(capture.status().status == "absent");
        CHECK(capture.status().state == RenderDocCaptureState::unavailable);
        requireCaptureReason("renderdoc_not_injected", [&] {
            capture.request(RenderDocCaptureSource::rpc, false);
        });
    }

    SECTION("API version mismatch stays distinct") {
        RenderDocCapture capture{"renderdoc_api_version_mismatch"};
        CHECK(capture.status().status == "unavailable");
        requireCaptureReason("renderdoc_api_version_mismatch", [&] {
            capture.request(RenderDocCaptureSource::rpc, false);
        });
    }

    SECTION("XR v1 boundary") {
        FakeRenderDoc fake;
        RenderDocCapture capture{fake.table()};
        requireCaptureReason("capture_xr_unsupported", [&] {
            capture.request(RenderDocCaptureSource::rpc, true);
        });
        CHECK(capture.state() == RenderDocCaptureState::idle);
    }

    SECTION("shutdown") {
        FakeRenderDoc fake;
        RenderDocCapture capture{fake.table()};
        capture.beginShutdown();
        requireCaptureReason("capture_shutdown", [&] {
            capture.request(RenderDocCaptureSource::f11, false);
        });
    }

    SECTION("RenderDoc and engine state mismatch can recover on a later request") {
        FakeRenderDoc fake;
        fake.capturing = true;
        RenderDocCapture capture{fake.table()};
        requireCaptureReason("capture_state_mismatch", [&] {
            capture.request(RenderDocCaptureSource::rpc, false);
        });
        CHECK(capture.state() == RenderDocCaptureState::failed);
        fake.capturing = false;
        capture.request(RenderDocCaptureSource::rpc, false);
        CHECK(capture.state() == RenderDocCaptureState::armed);
    }
}

TEST_CASE("RenderDoc capture reports each completion failure without a second render",
          "[renderdoc][failure]") {
    TempCaptureFile file;
    FakeRenderDoc fake;
    fake.capture_path = file.capture;
    RenderDocCapture capture{fake.table()};
    int render_calls = 0;

    const auto run = [&] {
        capture.request(RenderDocCaptureSource::rpc, false);
        return capture.captureArmedFrame(9, nullptr, nullptr,
                                         [&] { ++render_calls; });
    };

    SECTION("start failure") {
        fake.start_succeeds = false;
        requireCaptureReason("capture_start_failed", run);
        CHECK(render_calls == 0);
    }

    SECTION("end failure") {
        fake.end_succeeds = false;
        requireCaptureReason("capture_end_failed", run);
        CHECK(render_calls == 1);
        CHECK(fake.discard_calls == 1);
    }

    SECTION("capture count did not increase") {
        fake.count_increment = 0;
        requireCaptureReason("capture_count_not_increased", run);
        CHECK(render_calls == 1);
    }

    SECTION("capture count increased by more than one") {
        fake.count_increment = 2;
        requireCaptureReason("capture_count_unexpected", run);
        CHECK(render_calls == 1);
    }

    SECTION("capture lookup failed") {
        fake.capture_lookup_succeeds = false;
        requireCaptureReason("capture_path_unavailable", run);
        CHECK(render_calls == 1);
    }

    SECTION("capture file is missing") {
        fake.capture_path = file.root / "missing.rdc";
        requireCaptureReason("capture_file_missing", run);
        CHECK(render_calls == 1);
    }

    SECTION("capture file is empty") {
        TempCaptureFile empty_file{false};
        fake.capture_path = empty_file.capture;
        requireCaptureReason("capture_file_empty", run);
        CHECK(render_calls == 1);
    }

    SECTION("render failed") {
        capture.request(RenderDocCaptureSource::rpc, false);
        requireCaptureReason("capture_render_failed", [&] {
            capture.captureArmedFrame(9, nullptr, nullptr, [] {
                throw std::runtime_error{"synthetic render error"};
            });
        });
        CHECK(fake.discard_calls == 1);
    }

    CHECK(capture.state() == RenderDocCaptureState::failed);
}

TEST_CASE("RenderDoc armed request rejects an externally changed capture index",
          "[renderdoc][state]") {
    FakeRenderDoc fake;
    RenderDocCapture capture{fake.table()};
    capture.request(RenderDocCaptureSource::f11, false);
    ++fake.capture_count;
    requireCaptureReason("capture_count_changed_before_start", [&] {
        capture.captureArmedFrame(1, nullptr, nullptr, [] {});
    });
    CHECK(fake.start_calls == 0);
    CHECK(capture.state() == RenderDocCaptureState::failed);
}

} // namespace Pelican
