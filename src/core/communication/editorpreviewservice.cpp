#define GLM_ENABLE_EXPERIMENTAL
#include "editorpreviewservice.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <set>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

[[noreturn]] void error(EditorPreviewErrorCode code, std::string field,
                        std::string message) {
    auto payload = OrderedJson::object();
    payload["field"] = std::move(field);
    throw EditorPreviewError{code, std::move(message), std::move(payload)};
}

void requireObject(const Json &value, std::string_view path,
                   EditorPreviewErrorCode code) {
    if (!value.is_object()) error(code, std::string{path}, std::string{path} + " must be an object");
}

void requireOnly(const Json &value, std::initializer_list<std::string_view> allowed,
                 std::string_view path, EditorPreviewErrorCode code) {
    for (auto field = value.begin(); field != value.end(); ++field) {
        if (std::find(allowed.begin(), allowed.end(), field.key()) == allowed.end()) {
            error(code, std::string{path} + "/" + field.key(),
                  std::string{path} + " has unknown field: " + field.key());
        }
    }
}

std::uint64_t exactUnsigned(const Json &value, std::string_view path,
                            EditorPreviewErrorCode code) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            error(code, std::string{path}, std::string{path} +
                                                " must be a non-negative integer token");
        }
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        error(code, std::string{path}, std::string{path} + " must be a non-negative integer token");
    }
    if (result > UINT64_C(9007199254740991)) {
        error(code, std::string{path}, std::string{path} + " exceeds 2^53-1");
    }
    return result;
}

float finiteNumber(const Json &value, std::string_view path) {
    if (!value.is_number()) {
        error(EditorPreviewErrorCode::capture_schema_violation, std::string{path},
              std::string{path} + " must be a finite number");
    }
    const auto result = value.get<float>();
    if (!std::isfinite(result)) {
        error(EditorPreviewErrorCode::capture_schema_violation, std::string{path},
              std::string{path} + " must be finite");
    }
    return result;
}

glm::vec3 vec3(const Json &value, std::string_view path) {
    if (!value.is_array() || value.size() != 3) {
        error(EditorPreviewErrorCode::capture_schema_violation, std::string{path},
              std::string{path} + " must be a three-number array");
    }
    return {finiteNumber(value.at(0), std::string{path} + "/0"),
            finiteNumber(value.at(1), std::string{path} + "/1"),
            finiteNumber(value.at(2), std::string{path} + "/2")};
}

glm::mat4 matrix(const Json &value, std::string_view path) {
    if (!value.is_array() || value.size() != 4) {
        error(EditorPreviewErrorCode::capture_schema_violation, std::string{path},
              std::string{path} + " must be a 4x4 matrix");
    }
    glm::mat4 result{1.0f};
    for (std::size_t row = 0; row < 4; ++row) {
        if (!value.at(row).is_array() || value.at(row).size() != 4) {
            error(EditorPreviewErrorCode::capture_schema_violation, std::string{path},
                  std::string{path} + " must be a 4x4 matrix");
        }
        for (std::size_t column = 0; column < 4; ++column) {
            result[static_cast<glm::length_t>(column)][static_cast<glm::length_t>(row)] =
                finiteNumber(value.at(row).at(column), std::string{path} + "/" +
                             std::to_string(row) + "/" + std::to_string(column));
        }
    }
    return result;
}

void checkGate(std::string_view method, const EditorPreviewGateSnapshot &gate,
               std::optional<std::uint64_t> accepted_epoch = std::nullopt) {
    if (!gate.can_preview || (accepted_epoch && gate.epoch != *accepted_epoch)) {
        auto reason = !gate.can_preview
                          ? (gate.reasons.empty() ? std::string{"can_preview=false"}
                                                  : gate.reasons.front())
                          : std::string{"gate_epoch_changed"};
        auto payload = OrderedJson::object();
        payload["method"] = std::string{method};
        payload["reason"] = reason;
        payload["epoch"] = gate.epoch;
        throw EditorPreviewError{EditorPreviewErrorCode::gate_closed,
                                 std::string{method} + " gate is closed: " + reason,
                                 std::move(payload)};
    }
}

void checkXr(std::string_view method, const std::function<bool()> &xr_active) {
    if (xr_active && xr_active()) {
        auto payload = OrderedJson::object();
        payload["method"] = std::string{method};
        payload["reason"] = "xr_active";
        throw EditorPreviewError{EditorPreviewErrorCode::xr_active_unsupported,
                                 std::string{method} + " is unavailable while OpenXR is active",
                                 std::move(payload)};
    }
}

void requireStateUnchanged(const OrderedJson &before, const OrderedJson &after,
                           std::string_view method) {
    if (before != after) {
        auto payload = OrderedJson::object();
        payload["method"] = std::string{method};
        payload["reason"] = "shared_state_mismatch";
        throw EditorPreviewError{EditorPreviewErrorCode::state_changed,
                                 std::string{method} + " changed shared engine state",
                                 std::move(payload)};
    }
}

template <class Invoke>
OrderedJson isolated(std::string_view method,
                     const std::function<OrderedJson()> &snapshot,
                     Invoke &&invoke) {
    const auto before = snapshot();
    try {
        auto result = std::forward<Invoke>(invoke)();
        requireStateUnchanged(before, snapshot(), method);
        return result;
    } catch (...) {
        const auto failure = std::current_exception();
        requireStateUnchanged(before, snapshot(), method);
        std::rethrow_exception(failure);
    }
}

std::string base64(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2u) / 3u) * 4u);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto remaining = bytes.size() - index;
        const std::uint32_t word = static_cast<std::uint32_t>(bytes[index]) << 16u |
            (remaining > 1 ? static_cast<std::uint32_t>(bytes[index + 1]) << 8u : 0u) |
            (remaining > 2 ? bytes[index + 2] : 0u);
        result.push_back(alphabet[(word >> 18u) & 63u]);
        result.push_back(alphabet[(word >> 12u) & 63u]);
        result.push_back(remaining > 1 ? alphabet[(word >> 6u) & 63u] : '=');
        result.push_back(remaining > 2 ? alphabet[word & 63u] : '=');
    }
    return result;
}

struct ParsedCapture {
    PreviewCaptureRequest request;
    Json camera;
};

std::size_t predictedBytes(std::uint32_t width, std::uint32_t height,
                           PreviewPixelEncoding encoding) {
    const auto raw = static_cast<std::size_t>(width) * height * 4u;
    if (encoding == PreviewPixelEncoding::rgba8_srgb) return raw;
    const auto scanlines = raw + height;
    const auto blocks = (scanlines + 65534u) / 65535u;
    return 57u + 2u + scanlines + blocks * 5u + 4u;
}

ParsedCapture parseCapture(const Json &capture, const PreviewGraphProgram &program) {
    constexpr auto code = EditorPreviewErrorCode::capture_schema_violation;
    requireObject(capture, "capture", code);
    requireOnly(capture, {"width", "height", "pixel_encoding", "camera",
                          "graph_generation", "max_bytes"}, "capture", code);
    for (const auto required : {"width", "height", "pixel_encoding", "camera",
                                "graph_generation", "max_bytes"}) {
        if (!capture.contains(required)) {
            error(code, std::string{"capture/"} + required,
                  std::string{"capture requires field: "} + required);
        }
    }
    const auto width = exactUnsigned(capture.at("width"), "capture/width", code);
    const auto height = exactUnsigned(capture.at("height"), "capture/height", code);
    if (width == 0 || height == 0 || width > preview_capture_max_dimension ||
        height > preview_capture_max_dimension) {
        error(code, "capture/width,height", "capture dimensions must be in range 1..2048");
    }
    if (!capture.at("pixel_encoding").is_string()) {
        error(code, "capture/pixel_encoding", "pixel_encoding must be a string");
    }
    const auto encoding_name = capture.at("pixel_encoding").get<std::string>();
    PreviewPixelEncoding encoding;
    if (encoding_name == "rgba8_srgb") encoding = PreviewPixelEncoding::rgba8_srgb;
    else if (encoding_name == "png") encoding = PreviewPixelEncoding::png;
    else error(code, "capture/pixel_encoding", "pixel_encoding must be png or rgba8_srgb");
    const auto generation = exactUnsigned(capture.at("graph_generation"),
                                          "capture/graph_generation", code);
    if (generation != program.generation) {
        error(code, "capture/graph_generation", "preview graph generation mismatch");
    }
    const auto max_bytes = exactUnsigned(capture.at("max_bytes"),
                                         "capture/max_bytes", code);
    if (max_bytes == 0 || max_bytes > preview_capture_hard_max_bytes) {
        error(code, "capture/max_bytes", "max_bytes must be in range 1..16777216");
    }
    const auto predicted = predictedBytes(static_cast<std::uint32_t>(width),
                                          static_cast<std::uint32_t>(height), encoding);
    if (predicted > max_bytes) {
        auto payload = OrderedJson::object();
        payload["actual_bytes"] = predicted;
        payload["max_bytes"] = max_bytes;
        throw EditorPreviewError{EditorPreviewErrorCode::capture_too_large,
                                 "preview capture exceeds max_bytes",
                                 std::move(payload)};
    }
    static std::atomic_uint64_t next_request_id{1};
    return ParsedCapture{
        .request = {.width = static_cast<std::uint32_t>(width),
                    .height = static_cast<std::uint32_t>(height),
                    .pixel_encoding = encoding,
                    .graph_generation = generation,
                    .max_bytes = static_cast<std::size_t>(max_bytes),
                    .preview_request_id = "preview_request_id:" +
                                          std::to_string(next_request_id.fetch_add(1))},
        .camera = capture.at("camera"),
    };
}

void resolveCamera(const Json &camera, const PreparedProjection &prepared,
                   PreviewCaptureRequest &request) {
    constexpr auto code = EditorPreviewErrorCode::capture_schema_violation;
    requireObject(camera, "capture/camera", code);
    if (camera.contains("object_id")) {
        requireOnly(camera, {"object_id"}, "capture/camera", code);
        const auto object_id = exactUnsigned(camera.at("object_id"),
                                             "capture/camera/object_id", code);
        const auto result = EditorPreviewEvaluationContext{prepared}.evaluate(Json::array({
            {{"kind", "camera"}, {"object_id", object_id},
             {"width", request.width}, {"height", request.height}}
        }));
        request.view = matrix(result.at(0).at("data").at("view"), "prepared_camera/view");
        request.projection = matrix(result.at(0).at("data").at("projection"),
                                    "prepared_camera/projection");
        return;
    }
    requireOnly(camera, {"position", "target", "up", "projection"},
                "capture/camera", code);
    for (const auto required : {"position", "target", "up", "projection"}) {
        if (!camera.contains(required)) {
            error(code, std::string{"capture/camera/"} + required,
                  std::string{"explicit camera requires field: "} + required);
        }
    }
    const auto position = vec3(camera.at("position"), "capture/camera/position");
    const auto target = vec3(camera.at("target"), "capture/camera/target");
    const auto up = vec3(camera.at("up"), "capture/camera/up");
    const auto direction = target - position;
    if (glm::length(direction) < 0.00001f || glm::length(up) < 0.00001f ||
        glm::length(glm::cross(direction, up)) < 0.00001f) {
        error(code, "capture/camera",
              "explicit camera direction and up must be non-zero and non-parallel");
    }
    request.view = glm::lookAtRH(position, target, up);
    const auto &projection = camera.at("projection");
    requireObject(projection, "capture/camera/projection", code);
    if (!projection.contains("kind") || !projection.at("kind").is_string()) {
        error(code, "capture/camera/projection/kind", "projection kind is required");
    }
    const auto kind = projection.at("kind").get<std::string>();
    if (kind == "perspective") {
        requireOnly(projection, {"kind", "yfov", "znear", "zfar"},
                    "capture/camera/projection", code);
        for (const auto field : {"yfov", "znear", "zfar"}) {
            if (!projection.contains(field)) error(code, std::string{"capture/camera/projection/"} + field, "perspective projection field is required");
        }
        const auto yfov = finiteNumber(projection.at("yfov"), "capture/camera/projection/yfov");
        const auto near_value = finiteNumber(projection.at("znear"), "capture/camera/projection/znear");
        const auto far_value = finiteNumber(projection.at("zfar"), "capture/camera/projection/zfar");
        if (yfov <= 0 || yfov >= 3.14159265f || near_value <= 0 || far_value <= near_value) {
            error(code, "capture/camera/projection", "invalid perspective projection range");
        }
        request.projection = glm::perspectiveRH_ZO(
            yfov, static_cast<float>(request.width) / request.height,
            near_value, far_value);
    } else if (kind == "orthographic") {
        requireOnly(projection, {"kind", "xmag", "ymag", "znear", "zfar"},
                    "capture/camera/projection", code);
        for (const auto field : {"xmag", "ymag", "znear", "zfar"}) {
            if (!projection.contains(field)) error(code, std::string{"capture/camera/projection/"} + field, "orthographic projection field is required");
        }
        const auto xmag = finiteNumber(projection.at("xmag"), "capture/camera/projection/xmag");
        const auto ymag = finiteNumber(projection.at("ymag"), "capture/camera/projection/ymag");
        const auto near_value = finiteNumber(projection.at("znear"), "capture/camera/projection/znear");
        const auto far_value = finiteNumber(projection.at("zfar"), "capture/camera/projection/zfar");
        if (xmag <= 0 || ymag <= 0 || far_value <= near_value) {
            error(code, "capture/camera/projection", "invalid orthographic projection range");
        }
        request.projection = glm::orthoRH_ZO(-xmag, xmag, -ymag, ymag,
                                             near_value, far_value);
    } else {
        error(code, "capture/camera/projection/kind", "projection kind must be perspective or orthographic");
    }
}

EditorPreviewError translateProjection(const EditorPreviewProjectionError &source) {
    const auto code = source.code() == EditorPreviewProjectionErrorCode::schema_violation
                          ? EditorPreviewErrorCode::schema_violation
                          : EditorPreviewErrorCode::method_unavailable;
    auto payload = OrderedJson::object();
    payload["field"] = source.field();
    payload["adapter"] = source.adapter();
    return EditorPreviewError{code, source.what(), std::move(payload)};
}

} // namespace

std::string_view editorPreviewErrorCodeName(EditorPreviewErrorCode code) noexcept {
    switch (code) {
    case EditorPreviewErrorCode::schema_violation: return "schema_violation";
    case EditorPreviewErrorCode::method_unavailable: return "method_unavailable";
    case EditorPreviewErrorCode::gate_closed: return "gate_closed";
    case EditorPreviewErrorCode::xr_active_unsupported: return "xr_active_unsupported";
    case EditorPreviewErrorCode::capture_schema_violation: return "capture_schema_violation";
    case EditorPreviewErrorCode::capture_too_large: return "capture_too_large";
    case EditorPreviewErrorCode::state_changed: return "state_changed";
    }
    return "method_unavailable";
}

EditorPreviewError::EditorPreviewError(EditorPreviewErrorCode code,
                                       std::string message,
                                       OrderedJson payload)
    : std::runtime_error{std::move(message)}, code_{code},
      payload_(std::move(payload)) {}

EditorPreviewService::EditorPreviewService(EditorPreviewServiceDependencies dependencies)
    : dependencies_{std::move(dependencies)} {
    if (!dependencies_.document || !dependencies_.preview_graph ||
        !dependencies_.engine_time || !dependencies_.shared_state_snapshot) {
        throw std::invalid_argument("EditorPreviewService requires document, graph, time, and state providers");
    }
}

OrderedJson EditorPreviewService::evalPreview(
    const Json &params, const EditorPreviewGateProvider &gate_provider) const {
    constexpr auto method = "eval_preview";
    if (!gate_provider) throw std::invalid_argument("eval_preview requires gate provider");
    const auto accepted_gate = gate_provider();
    checkGate(method, accepted_gate);
    checkXr(method, dependencies_.xr_active);
    return isolated(method, dependencies_.shared_state_snapshot, [&]() -> OrderedJson {
        requireObject(params, method, EditorPreviewErrorCode::schema_violation);
        requireOnly(params, {"overrides", "queries"}, method,
                    EditorPreviewErrorCode::schema_violation);
        const auto overrides = params.value("overrides", Json::array());
        const auto queries = params.value("queries", Json::array());
        try {
            auto prepared = prepareEditorPreviewProjection(
                dependencies_.document(), overrides, dependencies_.projection_fault_hook);
            const auto execution_gate = gate_provider();
            checkGate(method, execution_gate, accepted_gate.epoch);
            checkXr(method, dependencies_.xr_active);
            if (dependencies_.execution_fault_hook) dependencies_.execution_fault_hook(method);
            const auto results = EditorPreviewEvaluationContext{prepared}.evaluate(
                queries, dependencies_.projection_fault_hook);
            return {{"status", "evaluated"},
                    {"scene_revision", dependencies_.document().revision().value},
                    {"graph", "preview"},
                    {"graph_generation", dependencies_.preview_graph().generation},
                    {"results", results}};
        } catch (const EditorPreviewProjectionError &source) {
            throw translateProjection(source);
        }
    });
}

OrderedJson EditorPreviewService::renderPreview(
    const Json &params, const EditorPreviewGateProvider &gate_provider) const {
    constexpr auto method = "render_preview";
    if (!gate_provider) throw std::invalid_argument("render_preview requires gate provider");
    const auto accepted_gate = gate_provider();
    checkGate(method, accepted_gate);
    checkXr(method, dependencies_.xr_active);
    return isolated(method, dependencies_.shared_state_snapshot, [&]() -> OrderedJson {
        requireObject(params, method, EditorPreviewErrorCode::capture_schema_violation);
        requireOnly(params, {"overrides", "capture"}, method,
                    EditorPreviewErrorCode::capture_schema_violation);
        if (!params.contains("capture")) {
            error(EditorPreviewErrorCode::capture_schema_violation,
                  "render_preview/capture", "render_preview capture is required");
        }
        auto parsed = parseCapture(params.at("capture"), dependencies_.preview_graph());
        const auto overrides = params.value("overrides", Json::array());
        try {
            auto prepared = prepareEditorPreviewProjection(
                dependencies_.document(), overrides, dependencies_.projection_fault_hook);
            resolveCamera(parsed.camera, prepared, parsed.request);
            const auto execution_gate = gate_provider();
            checkGate(method, execution_gate, accepted_gate.epoch);
            checkXr(method, dependencies_.xr_active);
            if (dependencies_.execution_fault_hook) dependencies_.execution_fault_hook(method);
            auto result = executor_.execute(dependencies_.preview_graph(), prepared,
                                            parsed.request, dependencies_.engine_time());
            return {{"status", "rendered"},
                    {"scene_revision", dependencies_.document().revision().value},
                    {"graph", "preview"},
                    {"graph_generation", dependencies_.preview_graph().generation},
                    {"capture", {{"width", result.width},
                                  {"height", result.height},
                                  {"pixel_encoding", result.pixel_encoding},
                                  {"byte_count", result.bytes.size()},
                                  {"data_base64", base64(result.bytes)}}},
                    {"timing", result.timing},
                    {"state_inventory", previewStateInventory()}};
        } catch (const PreviewCaptureTooLarge &source) {
            auto payload = OrderedJson::object();
            payload["actual_bytes"] = source.actual();
            payload["max_bytes"] = source.limit();
            throw EditorPreviewError{EditorPreviewErrorCode::capture_too_large,
                                     source.what(),
                                     std::move(payload)};
        } catch (const EditorPreviewProjectionError &source) {
            throw translateProjection(source);
        }
    });
}

} // namespace Pelican
