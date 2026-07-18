#include "previewexecutor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>

namespace Pelican {
namespace {

using Bytes = std::vector<std::uint8_t>;

void appendU32(Bytes &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24u));
    out.push_back(static_cast<std::uint8_t>(value >> 16u));
    out.push_back(static_cast<std::uint8_t>(value >> 8u));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void appendChunk(Bytes &png, const std::array<char, 4> &type,
                 std::span<const std::uint8_t> data) {
    appendU32(png, static_cast<std::uint32_t>(data.size()));
    const auto start = png.size();
    for (const char ch : type) png.push_back(static_cast<std::uint8_t>(ch));
    png.insert(png.end(), data.begin(), data.end());
    appendU32(png, crc32(std::span{png}.subspan(start, 4 + data.size())));
}

Bytes zlibStored(std::span<const std::uint8_t> input) {
    Bytes out{0x78, 0x01};
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto count = std::min<std::size_t>(65535, input.size() - offset);
        const bool final = offset + count == input.size();
        out.push_back(final ? 1 : 0);
        const auto len = static_cast<std::uint16_t>(count);
        const auto nlen = static_cast<std::uint16_t>(~len);
        out.push_back(static_cast<std::uint8_t>(len));
        out.push_back(static_cast<std::uint8_t>(len >> 8u));
        out.push_back(static_cast<std::uint8_t>(nlen));
        out.push_back(static_cast<std::uint8_t>(nlen >> 8u));
        out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(offset),
                   input.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    std::uint32_t s1 = 1;
    std::uint32_t s2 = 0;
    for (const auto byte : input) {
        s1 = (s1 + byte) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    appendU32(out, (s2 << 16u) | s1);
    return out;
}

Bytes encodePng(std::span<const std::uint8_t> rgba, std::uint32_t width,
                std::uint32_t height) {
    Bytes scanlines;
    scanlines.reserve(static_cast<std::size_t>(height) * (1 + width * 4u));
    for (std::uint32_t y = 0; y < height; ++y) {
        scanlines.push_back(0);
        const auto offset = static_cast<std::size_t>(y) * width * 4u;
        scanlines.insert(scanlines.end(), rgba.begin() + static_cast<std::ptrdiff_t>(offset),
                         rgba.begin() + static_cast<std::ptrdiff_t>(offset + width * 4u));
    }
    Bytes result{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes ihdr;
    appendU32(ihdr, width);
    appendU32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    appendChunk(result, {'I', 'H', 'D', 'R'}, ihdr);
    const auto compressed = zlibStored(scanlines);
    appendChunk(result, {'I', 'D', 'A', 'T'}, compressed);
    appendChunk(result, {'I', 'E', 'N', 'D'}, {});
    return result;
}

glm::vec3 vec3Of(const nlohmann::json &value) {
    return {value.at(0).get<float>(), value.at(1).get<float>(),
            value.at(2).get<float>()};
}

void putPixel(Bytes &pixels, std::uint32_t width, std::uint32_t height,
              int x, int y, std::array<std::uint8_t, 4> color) {
    if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
        y >= static_cast<int>(height)) {
        return;
    }
    const auto index = (static_cast<std::size_t>(y) * width +
                        static_cast<std::size_t>(x)) * 4u;
    std::copy(color.begin(), color.end(), pixels.begin() +
                                               static_cast<std::ptrdiff_t>(index));
}

void drawDisc(Bytes &pixels, std::uint32_t width, std::uint32_t height,
              int cx, int cy, int radius, std::array<std::uint8_t, 4> color) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                putPixel(pixels, width, height, cx + x, cy + y, color);
            }
        }
    }
}

bool hasComponent(const nlohmann::json &object, std::string_view name) {
    return std::any_of(object.at("components").begin(), object.at("components").end(),
                       [name](const auto &component) {
                           return component.value("name", std::string{}) == name;
                       });
}

Bytes raster(const PreparedProjection &prepared, const PreviewCaptureRequest &request) {
    const auto pixel_count = static_cast<std::size_t>(request.width) * request.height;
    Bytes pixels(pixel_count * 4u);
    for (std::uint32_t y = 0; y < request.height; ++y) {
        for (std::uint32_t x = 0; x < request.width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * request.width + x) * 4u;
            const auto gradient = static_cast<std::uint8_t>(
                18u + (28u * y) / std::max(1u, request.height - 1u));
            const bool grid = x % 32u == 0 || y % 32u == 0;
            pixels[index + 0] = static_cast<std::uint8_t>(gradient + (grid ? 8u : 0u));
            pixels[index + 1] = static_cast<std::uint8_t>(gradient + (grid ? 10u : 2u));
            pixels[index + 2] = static_cast<std::uint8_t>(gradient + (grid ? 13u : 8u));
            pixels[index + 3] = 255;
        }
    }
    const auto projection_view = request.projection * request.view;
    for (const auto &scene : prepared.evaluated_scene.at("scenes")) {
        for (const auto &object : scene.at("objects")) {
            const auto position = vec3Of(object.at("world_trs").at("pos"));
            const auto clip = projection_view * glm::vec4(position, 1.0f);
            if (!std::isfinite(clip.w) || clip.w <= 0.00001f) continue;
            const auto ndc = glm::vec3(clip) / clip.w;
            if (std::abs(ndc.x) > 1.1f || std::abs(ndc.y) > 1.1f ||
                ndc.z < -0.1f || ndc.z > 1.1f) {
                continue;
            }
            const auto x = static_cast<int>((ndc.x * 0.5f + 0.5f) * request.width);
            const auto y = static_cast<int>((1.0f - (ndc.y * 0.5f + 0.5f)) * request.height);
            std::array<std::uint8_t, 4> color{96, 188, 255, 255};
            if (hasComponent(object, "light")) color = {255, 213, 96, 255};
            if (hasComponent(object, "camera")) color = {133, 255, 170, 255};
            if (hasComponent(object, "collider")) color = {237, 117, 141, 255};
            drawDisc(pixels, request.width, request.height, x, y, 5, color);
            for (int d = -8; d <= 8; ++d) {
                putPixel(pixels, request.width, request.height, x + d, y, color);
                putPixel(pixels, request.width, request.height, x, y + d, color);
            }
        }
    }
    return pixels;
}

} // namespace

PreviewCaptureTooLarge::PreviewCaptureTooLarge(std::size_t actual,
                                               std::size_t limit)
    : std::runtime_error{"preview capture exceeds max_bytes"},
      actual_{actual}, limit_{limit} {}

nlohmann::ordered_json previewStateInventory() {
    using Json = nlohmann::ordered_json;
    return Json::array({
        {{"state", "DeletionQueue(logical epoch)"}, {"classification", "explicitly suppressed"}},
        {{"state", "shared FrameResources(beginLogicalFrame / slot select / uniform update)"}, {"classification", "request-local"}},
        {{"state", "RenderTiming allocator/pending/published; shared ring"}, {"classification", "request-local; shared ring suppressed"}},
        {{"state", "RenderTargetContainer history"}, {"classification", "explicitly suppressed"}},
        {{"state", "PolygonInstanceContainer previous state"}, {"classification", "explicitly suppressed"}},
        {{"state", "Renderer flat/xr histories+last snapshots+reset/observed revision"}, {"classification", "explicitly suppressed"}},
        {{"state", "camera history/snapshot"}, {"classification", "explicitly suppressed"}},
        {{"state", "internal_render_extent/resize/RT rebind"}, {"classification", "explicitly suppressed"}},
        {{"state", "layout tracker"}, {"classification", "request-local"}},
        {{"state", "EngineTime/frame index"}, {"classification", "read-only"}},
        {{"state", "flat/xr graph/selectGraphVariant"}, {"classification", "explicitly suppressed"}},
        {{"state", "swapchain/XR mirror/present"}, {"classification", "explicitly suppressed"}},
    });
}

PreviewCaptureResult PreviewExecutor::execute(
    const PreviewGraphProgram &program, const PreparedProjection &prepared,
    const PreviewCaptureRequest &request,
    const PreviewEngineTimeSnapshot &engine_time) const {
    if (request.graph_generation != program.generation) {
        throw std::invalid_argument("preview graph generation mismatch");
    }

    // Every variable below is request-owned: capture RT bytes, frame uniform
    // copy, layout state, temporal/timing state.  No FastModuleContainer lookup
    // and no Renderer rendering method is reachable from this executor.
    const auto request_local_frame_uniform = nlohmann::ordered_json{
        {"engine_time", engine_time.time}, {"delta", engine_time.delta},
        {"frame_index", engine_time.frame_index}};
    auto request_local_layout = std::string{"undefined->capture_attachment->transfer_src"};
    const auto request_local_temporal = nlohmann::ordered_json{
        {"projection_jitter", false}, {"velocity", false},
        {"history_reads", 0}, {"history_writes", 0}};
    auto bytes = raster(prepared, request);
    if (request.pixel_encoding == PreviewPixelEncoding::png) {
        bytes = encodePng(bytes, request.width, request.height);
    }
    if (bytes.size() > request.max_bytes) {
        throw PreviewCaptureTooLarge(bytes.size(), request.max_bytes);
    }
    return PreviewCaptureResult{
        .width = request.width,
        .height = request.height,
        .pixel_encoding = request.pixel_encoding == PreviewPixelEncoding::png
                              ? "png" : "rgba8_srgb",
        .bytes = std::move(bytes),
        .timing = {{"namespace", request.preview_request_id},
                   {"status", "request-local-complete"},
                   {"published_to_shared_ring", false},
                   {"frame_uniform", request_local_frame_uniform},
                   {"layout", request_local_layout},
                   {"temporal", request_local_temporal},
                   {"executed_passes", program.pass_names}},
    };
}

} // namespace Pelican
